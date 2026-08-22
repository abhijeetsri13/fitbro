#include "broker_exec/lifecycle/lifecycle.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/idempotency/idempotency.hpp"

using broker_exec::domain::Order;
using broker_exec::domain::OrderState;
using broker_exec::domain::Price;
using broker_exec::domain::Quantity;

namespace life = broker_exec::lifecycle;
namespace idem = broker_exec::idempotency;

namespace {

// A BrokerView builder so each test reads as the one field it varies.
life::BrokerView view(const std::string& client_ref, OrderState state, std::int64_t key,
                      std::int64_t filled = 0, const std::string& broker_id = "BRK-1") {
  life::BrokerView v;
  v.client_ref = client_ref;
  v.broker_order_id = broker_id;
  v.observed_state = state;
  v.filled_qty = Quantity::of(filled);
  v.avg_price = Price::from_rupees(100);
  v.ordering_key = key;
  return v;
}

Order order_in(OrderState state, const std::string& client_ref = "alpha-1234abcd-uuid") {
  Order o;
  o.intent.client_ref = client_ref;
  o.state = state;
  return o;
}

}  // namespace

TEST_CASE("is_terminal: only Filled/Rejected/Cancelled are absorbing", "[lifecycle]") {
  CHECK(life::is_terminal(OrderState::Filled));
  CHECK(life::is_terminal(OrderState::Rejected));
  CHECK(life::is_terminal(OrderState::Cancelled));

  CHECK_FALSE(life::is_terminal(OrderState::Created));
  CHECK_FALSE(life::is_terminal(OrderState::Sent));
  CHECK_FALSE(life::is_terminal(OrderState::Acknowledged));
  CHECK_FALSE(life::is_terminal(OrderState::PartiallyFilled));
  CHECK_FALSE(life::is_terminal(OrderState::Unknown));
  CHECK_FALSE(life::is_terminal(OrderState::PartiallyPlaced));
  CHECK_FALSE(life::is_terminal(OrderState::ManualInterventionRequired));
}

TEST_CASE("is_valid_transition: happy path edges legal, illegal jumps rejected", "[lifecycle]") {
  CHECK(life::is_valid_transition(OrderState::Created, OrderState::Validated));
  CHECK(life::is_valid_transition(OrderState::Validated, OrderState::PendingSend));
  CHECK(life::is_valid_transition(OrderState::PendingSend, OrderState::Sent));
  CHECK(life::is_valid_transition(OrderState::Sent, OrderState::Acknowledged));
  CHECK(life::is_valid_transition(OrderState::Acknowledged, OrderState::PartiallyFilled));
  CHECK(life::is_valid_transition(OrderState::PartiallyFilled, OrderState::Filled));

  // Any non-terminal may go Unknown.
  CHECK(life::is_valid_transition(OrderState::Sent, OrderState::Unknown));

  // Illegal backwards / skipping jumps.
  CHECK_FALSE(life::is_valid_transition(OrderState::Filled, OrderState::Acknowledged));
  CHECK_FALSE(life::is_valid_transition(OrderState::Acknowledged, OrderState::Created));
  CHECK_FALSE(life::is_valid_transition(OrderState::Created, OrderState::Filled));

  // Terminal states are absorbing: no outgoing transition.
  CHECK_FALSE(life::is_valid_transition(OrderState::Filled, OrderState::Cancelled));
  CHECK_FALSE(life::is_valid_transition(OrderState::Rejected, OrderState::Unknown));
}

TEST_CASE("apply: full Created->Sent->Acknowledged->PartiallyFilled->Filled happy path",
          "[lifecycle]") {
  life::LifecycleEngine engine;
  Order o = order_in(OrderState::Created);
  const std::string ref = o.intent.client_ref;

  REQUIRE(engine.apply(o, view(ref, OrderState::Validated, 1)) == life::ApplyOutcome::Applied);
  REQUIRE(engine.apply(o, view(ref, OrderState::PendingSend, 2)) == life::ApplyOutcome::Applied);
  REQUIRE(engine.apply(o, view(ref, OrderState::Sent, 3)) == life::ApplyOutcome::Applied);
  REQUIRE(engine.apply(o, view(ref, OrderState::Acknowledged, 4)) == life::ApplyOutcome::Applied);
  REQUIRE(engine.apply(o, view(ref, OrderState::PartiallyFilled, 5, 25)) ==
          life::ApplyOutcome::Applied);
  CHECK(o.filled_qty == Quantity::of(25));
  REQUIRE(engine.apply(o, view(ref, OrderState::Filled, 6, 50)) == life::ApplyOutcome::Applied);

  CHECK(o.state == OrderState::Filled);
  CHECK(o.filled_qty == Quantity::of(50));
  CHECK(o.broker_order_id == "BRK-1");
  CHECK(engine.last_key(ref) == 6);
}

TEST_CASE("apply: terminal states are absorbing — a post-Filled view is dropped", "[lifecycle]") {
  life::LifecycleEngine engine;
  Order o = order_in(OrderState::Acknowledged);
  const std::string ref = o.intent.client_ref;

  REQUIRE(engine.apply(o, view(ref, OrderState::Filled, 10, 50)) == life::ApplyOutcome::Applied);
  REQUIRE(o.state == OrderState::Filled);

  // A later, numerically-NEWER view must not move it out of the terminal sink.
  const auto outcome = engine.apply(o, view(ref, OrderState::PartiallyFilled, 99, 10));
  CHECK(outcome == life::ApplyOutcome::DroppedTerminal);
  CHECK(o.state == OrderState::Filled);
  CHECK(o.filled_qty == Quantity::of(50));  // untouched
}

TEST_CASE("apply: forward-progressing — lower ordering_key dropped, equal/higher applied",
          "[lifecycle]") {
  life::LifecycleEngine engine;
  Order o = order_in(OrderState::Sent);
  const std::string ref = o.intent.client_ref;

  REQUIRE(engine.apply(o, view(ref, OrderState::Acknowledged, 5)) == life::ApplyOutcome::Applied);
  CHECK(engine.last_key(ref) == 5);

  // Lower key -> stale, dropped, order untouched.
  CHECK(engine.apply(o, view(ref, OrderState::PartiallyFilled, 4, 10)) ==
        life::ApplyOutcome::DroppedStale);
  CHECK(o.state == OrderState::Acknowledged);
  CHECK(engine.last_key(ref) == 5);

  // Equal key, same state, no fill change -> idempotent NoChange (key holds).
  CHECK(engine.apply(o, view(ref, OrderState::Acknowledged, 5)) == life::ApplyOutcome::NoChange);
  CHECK(engine.last_key(ref) == 5);

  // Higher key -> applied.
  CHECK(engine.apply(o, view(ref, OrderState::PartiallyFilled, 6, 10)) ==
        life::ApplyOutcome::Applied);
  CHECK(o.state == OrderState::PartiallyFilled);
  CHECK(engine.last_key(ref) == 6);
}

TEST_CASE("apply: illegal transition is refused (NoChange), order left untouched", "[lifecycle]") {
  life::LifecycleEngine engine;
  Order o = order_in(OrderState::Created);
  const std::string ref = o.intent.client_ref;

  // Created -> Filled is not a legal edge.
  const auto outcome = engine.apply(o, view(ref, OrderState::Filled, 1, 50));
  CHECK(outcome == life::ApplyOutcome::NoChange);
  CHECK(o.state == OrderState::Created);
  CHECK(o.filled_qty == Quantity::of(0));
  // The key is NOT recorded: it is the high-water mark of what was APPLIED, not
  // of what was seen. See the test below for what recording it would cost.
  CHECK(engine.last_key(ref) == std::nullopt);
}

TEST_CASE("apply: a refused view neither canonicalizes nor raises the ordering high-water mark",
          "[lifecycle]") {
  life::LifecycleEngine engine;
  Order o = order_in(OrderState::Created);
  const std::string ref = o.intent.client_ref;

  // BOUNDARY of the working-order canonicalization below: before the order has
  // reached the broker, `Sent` still means the forward edge it means on the happy
  // path (PendingSend -> Sent). It is NOT rewritten into Acknowledged, so
  // Created -> Sent stays the illegal jump it has always been.
  CHECK(engine.apply(o, view(ref, OrderState::Sent, 10, 50)) == life::ApplyOutcome::NoChange);
  CHECK(o.state == OrderState::Created);
  CHECK(o.filled_qty == Quantity::of(0));

  // And the key of a view the machine never believed must not become the
  // high-water mark — otherwise this legal, lower-keyed view, which carries real
  // broker truth, is dropped as stale and one refusal silently costs us the next
  // good observation as well.
  CHECK(engine.last_key(ref) == std::nullopt);
  CHECK(engine.apply(o, view(ref, OrderState::Validated, 3)) == life::ApplyOutcome::Applied);
  CHECK(o.state == OrderState::Validated);
  CHECK(engine.last_key(ref) == 3);
}

// ── Adapter spellings of "still working at the broker" ──────────────────────
//
// The Kite adapter maps EVERY working broker status (OPEN, TRIGGER PENDING,
// MODIFY PENDING, ...) onto OrderState::Sent and publishes it carrying the row's
// filled_quantity. Read literally that is a backward jump out of every state a
// live order is actually held in, so the table refused it and the fill, the
// average price and the broker order id it carried were all discarded — for the
// entire working life of the order, with no alert anywhere.

TEST_CASE("apply: a working-order view on an Acknowledged order records the fill", "[lifecycle]") {
  life::LifecycleEngine engine;
  Order o = order_in(OrderState::Acknowledged);
  const std::string ref = o.intent.client_ref;

  // The exact production pairing: the dispatcher stores a placed order as
  // Acknowledged, then Kite reports it "OPEN" with filled_quantity 50. The view
  // means "still working, 50 done" -> PartiallyFilled/50. Leaving filled_qty at 0
  // here is what let a shrink-to-30 modify past modifyguard and cancel the
  // working remainder of a real 50-lot position.
  REQUIRE(engine.apply(o, view(ref, OrderState::Sent, 7, 50)) == life::ApplyOutcome::Applied);
  CHECK(o.state == OrderState::PartiallyFilled);
  CHECK(o.filled_qty == Quantity::of(50));
  CHECK(o.broker_order_id == "BRK-1");
}

TEST_CASE("apply: a working-order view with no fill holds Acknowledged and still lands the id",
          "[lifecycle]") {
  life::LifecycleEngine engine;
  Order o = order_in(OrderState::Acknowledged);
  const std::string ref = o.intent.client_ref;
  REQUIRE(o.broker_order_id.empty());

  // filled_quantity 0 -> the canonical working state is Acknowledged, a self
  // transition. The broker order id and average price in the same view are still
  // real new information and must not be thrown out with the state.
  CHECK(engine.apply(o, view(ref, OrderState::Sent, 7, 0)) == life::ApplyOutcome::Applied);
  CHECK(o.state == OrderState::Acknowledged);
  CHECK(o.broker_order_id == "BRK-1");
  CHECK(o.filled_qty == Quantity::of(0));
}

TEST_CASE("apply: a working-order view advances the fill on an already-partial order",
          "[lifecycle]") {
  life::LifecycleEngine engine;
  Order o = order_in(OrderState::PartiallyFilled);
  o.filled_qty = Quantity::of(50);
  const std::string ref = o.intent.client_ref;

  // Kite keeps reporting "OPEN" as the remainder works off, so the same refusal
  // froze filled_qty at whatever the last believed view said.
  CHECK(engine.apply(o, view(ref, OrderState::Sent, 8, 80)) == life::ApplyOutcome::Applied);
  CHECK(o.state == OrderState::PartiallyFilled);
  CHECK(o.filled_qty == Quantity::of(80));
}

TEST_CASE("apply: a working-order view resolves an Unknown order instead of pinning it",
          "[lifecycle]") {
  life::LifecycleEngine engine;

  // UnknownResolver::adopt ignores apply's return value, so an UNKNOWN order
  // whose broker truth is a live working order could never be resolved: the
  // engine stayed in the UNKNOWN pause, blocking every non-risk-reducing entry.
  Order idle = order_in(OrderState::Unknown, "alpha-1234abcd-idle");
  CHECK(engine.apply(idle, view(idle.intent.client_ref, OrderState::Sent, 9, 0)) ==
        life::ApplyOutcome::Applied);
  CHECK(idle.state == OrderState::Acknowledged);

  Order partial = order_in(OrderState::Unknown, "alpha-1234abcd-part");
  CHECK(engine.apply(partial, view(partial.intent.client_ref, OrderState::Sent, 9, 30)) ==
        life::ApplyOutcome::Applied);
  CHECK(partial.state == OrderState::PartiallyFilled);
  CHECK(partial.filled_qty == Quantity::of(30));
}

TEST_CASE("apply: a working-order view is never read as Filled, from Reconciled either",
          "[lifecycle]") {
  life::LifecycleEngine engine;
  Order o = order_in(OrderState::Reconciled);
  o.intent.quantity = Quantity::of(50);
  const std::string ref = o.intent.client_ref;

  // The whole order quantity reported as filled while the broker STILL calls the
  // order working. The canonical state is PartiallyFilled, never the terminal,
  // absorbing Filled: that sink has to come from the broker saying so, not from
  // our own arithmetic, because nothing ever moves an order back out of it.
  CHECK(engine.apply(o, view(ref, OrderState::Sent, 4, 50)) == life::ApplyOutcome::Applied);
  CHECK(o.state == OrderState::PartiallyFilled);
  CHECK_FALSE(life::is_terminal(o.state));
  CHECK(o.filled_qty == Quantity::of(50));
}

TEST_CASE("apply: a no-key order with no prior view applies the first view", "[lifecycle]") {
  life::LifecycleEngine engine;
  Order o = order_in(OrderState::Created);
  CHECK(engine.last_key(o.intent.client_ref) == std::nullopt);
  CHECK(engine.apply(o, view(o.intent.client_ref, OrderState::Validated, 0)) ==
        life::ApplyOutcome::Applied);
}

TEST_CASE("fold_parent_state: all-filled -> Filled", "[lifecycle]") {
  const std::vector<OrderState> children{OrderState::Filled, OrderState::Filled,
                                         OrderState::Filled};
  CHECK(life::fold_parent_state(children) == OrderState::Filled);
}

TEST_CASE("fold_parent_state: any child Unknown -> PartiallyPlaced", "[lifecycle]") {
  const std::vector<OrderState> children{OrderState::Filled, OrderState::Unknown,
                                         OrderState::Acknowledged};
  CHECK(life::fold_parent_state(children) == OrderState::PartiallyPlaced);
}

TEST_CASE("fold_parent_state: any child Rejected (no Unknown) -> ManualInterventionRequired",
          "[lifecycle]") {
  const std::vector<OrderState> children{OrderState::Filled, OrderState::Rejected};
  CHECK(life::fold_parent_state(children) == OrderState::ManualInterventionRequired);
}

TEST_CASE(
    "fold_parent_state: mixed active/terminal -> PartiallyPlaced; mixed terminal-only "
    "-> PartiallyFilled",
    "[lifecycle]") {
  // One filled, one still acknowledged (active) -> placement in progress.
  const std::vector<OrderState> in_progress{OrderState::Filled, OrderState::Acknowledged};
  CHECK(life::fold_parent_state(in_progress) == OrderState::PartiallyPlaced);

  // One filled, one cancelled (both terminal, mixed) -> partially filled.
  const std::vector<OrderState> mixed_terminal{OrderState::Filled, OrderState::Cancelled};
  CHECK(life::fold_parent_state(mixed_terminal) == OrderState::PartiallyFilled);

  // All cancelled -> Cancelled.
  const std::vector<OrderState> all_cancelled{OrderState::Cancelled, OrderState::Cancelled};
  CHECK(life::fold_parent_state(all_cancelled) == OrderState::Cancelled);
}

TEST_CASE("fold_parent_state: empty children -> Created", "[lifecycle]") {
  CHECK(life::fold_parent_state({}) == OrderState::Created);
}

TEST_CASE("apply_child: folds children and recovers parent from a slice ref", "[lifecycle]") {
  life::LifecycleEngine engine;
  const std::string parent = "alpha-1234abcd-uuid";
  const std::string child1 = idem::child_ref(parent, 1);  // "alpha-...#1"
  const std::string child2 = idem::child_ref(parent, 2);  // "alpha-...#2"
  REQUIRE(idem::is_child_ref(child1));

  // First child filled, parent has only one (filled) child so far -> Filled.
  CHECK(engine.apply_child(parent, child1, OrderState::Filled) == OrderState::Filled);

  // Second child Unknown -> parent goes to the "unknown present" posture.
  CHECK(engine.apply_child(parent, child2, OrderState::Unknown) == OrderState::PartiallyPlaced);
  CHECK(engine.parent_state(parent) == OrderState::PartiallyPlaced);

  // Resolve the unknown child to Filled -> all filled -> parent Filled.
  CHECK(engine.apply_child(parent, child2, OrderState::Filled) == OrderState::Filled);
  CHECK(engine.parent_state(parent) == OrderState::Filled);
}

TEST_CASE("parent_state: unknown parent -> nullopt", "[lifecycle]") {
  life::LifecycleEngine engine;
  CHECK(engine.parent_state("nobody") == std::nullopt);
}

TEST_CASE("to_string: ApplyOutcome names are stable", "[lifecycle]") {
  CHECK(life::to_string(life::ApplyOutcome::Applied) == "APPLIED");
  CHECK(life::to_string(life::ApplyOutcome::DroppedStale) == "DROPPED_STALE");
  CHECK(life::to_string(life::ApplyOutcome::DroppedTerminal) == "DROPPED_TERMINAL");
  CHECK(life::to_string(life::ApplyOutcome::NoChange) == "NO_CHANGE");
}
