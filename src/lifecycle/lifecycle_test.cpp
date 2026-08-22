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
  // The key is still recorded so a later equal/higher key is consistent.
  CHECK(engine.last_key(ref) == 1);
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
