#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "broker_exec/adapters/fake/fake_broker.hpp"
#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/lifecycle/lifecycle.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/reconcile/reconciler.hpp"

using broker_exec::domain::Order;
using broker_exec::domain::OrderState;
using broker_exec::domain::Position;
using broker_exec::domain::Price;
using broker_exec::domain::Quantity;
using broker_exec::ports::AlertLevel;

namespace fake = broker_exec::adapters::fake;
namespace life = broker_exec::lifecycle;
namespace rec = broker_exec::reconcile;

namespace {

// An in-test AlertSink that records every alert, so a test can assert that (and
// only that) a mismatch escalates. Mirrors conformance_kit's recording sink.
class CountingAlertSink final : public broker_exec::ports::AlertSink {
 public:
  broker_exec::Result<broker_exec::ports::Ok> send(AlertLevel level,
                                                   const std::string& message) override {
    ++count_;
    last_level_ = level;
    last_message_ = message;
    last_provenance_ = broker_exec::ports::AlertContext{};
    return broker_exec::ports::ok();
  }
  // IMP-16: the order ids ride in the TYPED context now (the free-form body is
  // scrubbed by any real sink, which destroyed an interpolated client_ref), so
  // the stub records it rather than inheriting the base default that drops it.
  broker_exec::Result<broker_exec::ports::Ok> send_with_context(
      AlertLevel level, const std::string& message,
      const broker_exec::ports::AlertContext& provenance) override {
    const auto out = send(level, message);
    last_provenance_ = provenance;
    return out;
  }
  broker_exec::Result<broker_exec::ports::Ok> send_test_alert() override {
    return broker_exec::ports::ok();
  }

  [[nodiscard]] std::size_t count() const noexcept { return count_; }
  [[nodiscard]] AlertLevel last_level() const noexcept { return last_level_; }
  [[nodiscard]] const std::string& last_message() const noexcept { return last_message_; }
  [[nodiscard]] const broker_exec::ports::AlertContext& last_provenance() const noexcept {
    return last_provenance_;
  }

 private:
  std::size_t count_ = 0;
  AlertLevel last_level_ = AlertLevel::Info;
  std::string last_message_;
  broker_exec::ports::AlertContext last_provenance_;
};

// A local order in a given state with a known client_ref.
Order local_order(const std::string& client_ref, OrderState state) {
  Order o;
  o.intent.client_ref = client_ref;
  o.state = state;
  o.broker_order_id = "BRK-" + client_ref;
  return o;
}

// A broker order row as it would appear in a ReconcileResult snapshot.
Order broker_order(const std::string& client_ref, OrderState state) {
  Order o;
  o.intent.client_ref = client_ref;
  o.state = state;
  o.broker_order_id = "BRK-" + client_ref;
  o.avg_price = Price::from_rupees(100);
  return o;
}

rec::ReconcileResult result_with(std::vector<Order> orders, std::int64_t ordering_key) {
  rec::ReconcileResult r;
  r.orders = std::move(orders);
  r.ordering_key = ordering_key;
  return r;
}

}  // namespace

// ── fetch (AC-1): reads-only, all fields populated; a read failure -> Error ──

TEST_CASE("fetch: drives a FakeBroker and populates the immutable result", "[reconcile]") {
  broker_exec::clock::TestClock clock(
      std::chrono::steady_clock::time_point{},
      std::chrono::system_clock::time_point{} + std::chrono::seconds{42});
  fake::FakeBroker broker(clock);
  broker.set_funds({broker_exec::domain::Money::from_rupees(5000),
                    broker_exec::domain::Money::from_rupees(100)});

  broker_exec::domain::OrderIntent intent;
  intent.client_ref = "alpha-1";
  intent.symbol = "NIFTY";
  intent.quantity = Quantity::of(50);
  intent.price = Price::from_rupees(100);
  REQUIRE(broker.place(intent));  // order enters the broker book

  const rec::Reconciler reconciler(clock);
  auto fetched = reconciler.fetch(broker, /*snapshot_seq=*/7);
  REQUIRE(fetched);

  const rec::ReconcileResult& result = fetched.value();
  // AC-1: all four broker reads are packaged. The placed order is filled by the
  // fake (a trade is emitted and a non-zero position derived), so trades AND
  // positions are populated alongside orders and funds.
  CHECK_FALSE(result.orders.empty());
  CHECK_FALSE(result.trades.empty());
  CHECK_FALSE(result.positions.empty());
  CHECK(result.ordering_key == 7);
  CHECK(result.fetched_at == std::chrono::system_clock::time_point{} + std::chrono::seconds{42});
  CHECK(result.funds.available_margin == broker_exec::domain::Money::from_rupees(5000));
}

TEST_CASE("fetch: a broker read failure surfaces a typed Error", "[reconcile]") {
  broker_exec::clock::TestClock clock;
  fake::FakeBroker broker(clock);
  // Reject every subsequent request as RateLimited (0 accepted before throttling).
  fake::FaultConfig cfg;
  cfg.rate_limit_after = 0;
  broker.set_fault(cfg);

  const rec::Reconciler reconciler(clock);
  auto fetched = reconciler.fetch(broker, 1);
  REQUIRE_FALSE(fetched);
  CHECK(fetched.error().category == broker_exec::errors::ErrorCategory::RateLimited);
}

// ── apply forward-progress + idempotence (AC-1, sole-writer) ──

TEST_CASE("apply: a broker view advances a non-terminal local order, re-apply is idempotent",
          "[reconcile]") {
  life::LifecycleEngine engine;
  CountingAlertSink alerts;
  rec::ReconcileApplier applier(engine, alerts);

  std::vector<Order> local{local_order("alpha-1", OrderState::Sent)};
  const auto result = result_with({broker_order("alpha-1", OrderState::Acknowledged)}, 5);

  const rec::ReconcileOutcome first = applier.apply(result, local);
  CHECK(first.applied == 1);
  CHECK(first.advanced == 1);
  CHECK(first.mismatches == 0);
  CHECK_FALSE(first.block_new_orders);
  CHECK(local.front().state == OrderState::Acknowledged);
  CHECK(alerts.count() == 0);

  // Same result (same ordering_key) -> idempotent same-observation: no advance.
  const rec::ReconcileOutcome second = applier.apply(result, local);
  CHECK(second.applied == 1);
  CHECK(second.advanced == 0);
  CHECK(local.front().state == OrderState::Acknowledged);
}

TEST_CASE("apply: a stale (lower ordering_key) view is dropped, not regressed", "[reconcile]") {
  life::LifecycleEngine engine;
  CountingAlertSink alerts;
  rec::ReconcileApplier applier(engine, alerts);

  std::vector<Order> local{local_order("alpha-1", OrderState::Sent)};

  // Advance to Acknowledged at key 5.
  applier.apply(result_with({broker_order("alpha-1", OrderState::Acknowledged)}, 5), local);
  REQUIRE(local.front().state == OrderState::Acknowledged);

  // An older snapshot (key 4) carrying a different state is dropped as stale.
  const rec::ReconcileOutcome stale =
      applier.apply(result_with({broker_order("alpha-1", OrderState::PartiallyFilled)}, 4), local);
  CHECK(stale.applied == 1);
  CHECK(stale.advanced == 0);
  CHECK(stale.dropped_stale == 1);
  CHECK(local.front().state == OrderState::Acknowledged);  // not regressed
}

// ── terminal-absorbing (AC-1) ──

TEST_CASE("apply: a terminal local order is not moved by a contradicting view", "[reconcile]") {
  life::LifecycleEngine engine;
  CountingAlertSink alerts;
  rec::ReconcileApplier applier(engine, alerts);

  std::vector<Order> local{local_order("alpha-1", OrderState::Filled)};
  const auto result = result_with({broker_order("alpha-1", OrderState::PartiallyFilled)}, 99);

  const rec::ReconcileOutcome outcome = applier.apply(result, local);
  CHECK(outcome.applied == 1);
  CHECK(outcome.advanced == 0);
  CHECK(local.front().state == OrderState::Filled);  // absorbing sink

  // ...AND THE DISAGREEMENT IS RECORDED. This half is what the test used to
  // omit, which is how it came to pin the defect: DroppedTerminal shared one
  // empty `break` with NoChange, so a broker row contradicting a terminal local
  // order was counted nowhere, alerted nobody and left block_new_orders false.
  // The order still must not move — only the silence was wrong.
  CHECK(outcome.mismatches == 1);
  CHECK(outcome.block_new_orders);
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Warning);
}

// ── refused broker truth is a mismatch (the DroppedTerminal / NoChange sink) ──

TEST_CASE("apply: a locally-Cancelled order the broker reports FILLED blocks new orders",
          "[reconcile]") {
  life::LifecycleEngine engine;
  CountingAlertSink alerts;
  rec::ReconcileApplier applier(engine, alerts);

  // The bot believes it is flat; the broker says the order took 50 lots. Refusing
  // to move the terminal order is correct (the sink is the safety property), but
  // dropping the row without a trace let RecoveryCoordinator::recover() — which
  // gates only on mismatches/block_new_orders — report ResumedSafe over a live
  // 50-lot position.
  std::vector<Order> local{local_order("alpha-1", OrderState::Cancelled)};
  Order filled = broker_order("alpha-1", OrderState::Filled);
  filled.filled_qty = Quantity::of(50);
  const auto result = result_with({filled}, 10);

  const rec::ReconcileOutcome outcome = applier.apply(result, local);
  CHECK(outcome.applied == 1);
  CHECK(outcome.advanced == 0);
  CHECK(local.front().state == OrderState::Cancelled);  // the sink still holds
  CHECK(outcome.mismatches == 1);
  CHECK(outcome.block_new_orders);
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Warning);
  // The two OrderState names are in the BODY on purpose — short all-letter tokens
  // that scrub()'s >=20-char letters-AND-digits rule cannot touch — so the
  // operator sees WHICH WAY the sides disagree. The ids still ride in the typed
  // provenance and never in the body (IMP-16).
  CHECK(alerts.last_message().find("CANCELLED") != std::string::npos);
  CHECK(alerts.last_message().find("FILLED") != std::string::npos);
  CHECK(alerts.last_provenance().client_ref == "alpha-1");
  CHECK(alerts.last_message().find("alpha-1") == std::string::npos);
}

TEST_CASE("apply: a locally-Rejected order the broker still lists LIVE is a mismatch",
          "[reconcile]") {
  life::LifecycleEngine engine;
  CountingAlertSink alerts;
  rec::ReconcileApplier applier(engine, alerts);

  // THE REACHABLE ACK-LOST PATH. Dispatcher::place marks an order Rejected from
  // LOCAL error classification alone (Validation / InsufficientFunds /
  // RateLimited / SessionExpired — no broker confirmation that the order does not
  // exist), while KiteBrokerAdapter::place registers the correlation tag BEFORE
  // the wire call precisely so fetch_orders() can recover the row if the order did
  // reach the exchange. Recovering it and then dropping it here defeated that
  // mechanism outright.
  std::vector<Order> local{local_order("alpha-1", OrderState::Rejected)};
  const auto result = result_with({broker_order("alpha-1", OrderState::Acknowledged)}, 10);

  const rec::ReconcileOutcome outcome = applier.apply(result, local);
  CHECK(outcome.applied == 1);
  CHECK(local.front().state == OrderState::Rejected);  // never laundered back to live
  CHECK(outcome.mismatches == 1);
  CHECK(outcome.block_new_orders);
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_provenance().client_ref == "alpha-1");
}

TEST_CASE("apply: a broker fill correction on a terminal order is not discarded silently",
          "[reconcile]") {
  life::LifecycleEngine engine;
  CountingAlertSink alerts;
  rec::ReconcileApplier applier(engine, alerts);

  // Both sides agree the order is Filled, but the broker has since amended the
  // average price (a trade bust / correction). The FSM cannot rewrite a sink, so
  // the corrected number would be dropped forever and the ledger would keep the
  // wrong one — a money error that never surfaces. It has to reach a human.
  Order done = local_order("alpha-1", OrderState::Filled);
  done.filled_qty = Quantity::of(50);
  done.avg_price = Price::from_rupees(100);
  std::vector<Order> local{done};

  Order corrected = broker_order("alpha-1", OrderState::Filled);
  corrected.filled_qty = Quantity::of(50);
  corrected.avg_price = Price::from_rupees(101);
  const auto result = result_with({corrected}, 10);

  const rec::ReconcileOutcome outcome = applier.apply(result, local);
  CHECK(outcome.mismatches == 1);
  CHECK(outcome.block_new_orders);
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_message().find("fill correction") != std::string::npos);
  CHECK(local.front().avg_price == Price::from_rupees(100));  // the sink is not rewritten
}

TEST_CASE("apply: only a DISAGREEING terminal row escalates, never an identical one",
          "[reconcile]") {
  life::LifecycleEngine engine;
  CountingAlertSink alerts;
  rec::ReconcileApplier applier(engine, alerts);

  // "done-1" is the ROUTINE case the new escalation must stay silent about: the
  // broker keeps listing a terminal row we already match field for field. Firing
  // on that would block the bot on every poll after a fill. "alpha-2" is the real
  // contradiction. Both are in one snapshot so the counts prove the boundary.
  Order agreed = local_order("done-1", OrderState::Filled);
  agreed.filled_qty = Quantity::of(50);
  agreed.avg_price = Price::from_rupees(100);
  std::vector<Order> local{agreed, local_order("alpha-2", OrderState::Cancelled)};

  Order agreed_row = broker_order("done-1", OrderState::Filled);
  agreed_row.filled_qty = Quantity::of(50);
  agreed_row.avg_price = Price::from_rupees(100);
  const auto result = result_with({agreed_row, broker_order("alpha-2", OrderState::Filled)}, 10);

  const rec::ReconcileOutcome outcome = applier.apply(result, local);
  CHECK(outcome.applied == 2);
  CHECK(outcome.mismatches == 1);  // exactly one: the identical row added nothing
  CHECK(alerts.count() == 1);
  CHECK(outcome.block_new_orders);
  CHECK(alerts.last_provenance().client_ref == "alpha-2");
}

TEST_CASE("apply: an ILLEGAL broker transition is a mismatch, a benign re-observation is not",
          "[reconcile]") {
  life::LifecycleEngine engine;
  CountingAlertSink alerts;
  rec::ReconcileApplier applier(engine, alerts);

  // PartiallyFilled -> Acknowledged is not in the transition table, so the FSM
  // refuses the jump and returns NoChange — the SAME NoChange a benign unchanged
  // view returns, which is why a broker contradicting itself across snapshots was
  // indistinguishable from "nothing happened". "beta-2" is that benign view
  // (identical state, fills and broker id) and must stay silent, so the counts
  // below pin both halves.
  Order benign = local_order("beta-2", OrderState::Acknowledged);
  benign.avg_price = Price::from_rupees(100);  // matches the broker row exactly
  std::vector<Order> local{local_order("alpha-1", OrderState::PartiallyFilled), benign};
  const auto result = result_with({broker_order("alpha-1", OrderState::Acknowledged),
                                   broker_order("beta-2", OrderState::Acknowledged)},
                                  10);

  const rec::ReconcileOutcome outcome = applier.apply(result, local);
  CHECK(outcome.applied == 2);
  CHECK(outcome.advanced == 0);
  CHECK(local.front().state == OrderState::PartiallyFilled);  // the jump was refused
  CHECK(outcome.mismatches == 1);
  CHECK(outcome.block_new_orders);
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_provenance().client_ref == "alpha-1");
}

TEST_CASE("apply: a contradiction escalates from a FRESH snapshot and never from a stale one",
          "[reconcile]") {
  life::LifecycleEngine engine;
  CountingAlertSink alerts;
  rec::ReconcileApplier applier(engine, alerts);

  std::vector<Order> local{local_order("alpha-1", OrderState::Cancelled)};

  const rec::ReconcileOutcome fresh =
      applier.apply(result_with({broker_order("alpha-1", OrderState::Filled)}, 10), local);
  CHECK(fresh.mismatches == 1);
  CHECK(fresh.block_new_orders);
  CHECK(alerts.count() == 1);

  // The same contradiction re-delivered on an OLDER snapshot is routine
  // out-of-order delivery: the stale-snapshot guard has to cover the new
  // escalation exactly as it already covers phantom/vanished.
  const rec::ReconcileOutcome stale =
      applier.apply(result_with({broker_order("alpha-1", OrderState::Filled)}, 5), local);
  CHECK(stale.applied == 1);
  CHECK(stale.mismatches == 0);
  CHECK_FALSE(stale.block_new_orders);
  CHECK(alerts.count() == 1);  // no second alert
}

TEST_CASE("apply: a per-order OLDER view of a terminal order is not a contradiction",
          "[reconcile]") {
  life::LifecycleEngine engine;
  CountingAlertSink alerts;
  rec::ReconcileApplier applier(engine, alerts);

  // A push update already advanced "alpha-1" to Filled at ordering key 100. The
  // snapshot below is fresh AS A SNAPSHOT (none applied yet) but carries an older
  // observation of that order. The FSM checks terminal (rule 1) BEFORE ordering
  // (rule 2), so that view returns DroppedTerminal instead of DroppedStale —
  // escalating it would turn ordinary out-of-order delivery into a false block.
  // "beta-2" has no per-order key at all and IS a genuine contradiction, so the
  // counts distinguish "suppressed" from "never fired".
  Order pushed = local_order("alpha-1", OrderState::Acknowledged);
  life::BrokerView push;
  push.client_ref = "alpha-1";
  push.observed_state = OrderState::Filled;
  push.ordering_key = 100;
  REQUIRE(engine.apply(pushed, push) == life::ApplyOutcome::Applied);
  REQUIRE(pushed.state == OrderState::Filled);

  std::vector<Order> local{pushed, local_order("beta-2", OrderState::Cancelled)};
  const auto result = result_with({broker_order("alpha-1", OrderState::PartiallyFilled),
                                   broker_order("beta-2", OrderState::Filled)},
                                  20);

  const rec::ReconcileOutcome outcome = applier.apply(result, local);
  CHECK(outcome.applied == 2);
  CHECK(outcome.mismatches == 1);  // beta-2 only; alpha-1's view was simply older
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_provenance().client_ref == "beta-2");
}

// ── mismatch (AC-3) ──

TEST_CASE("apply: a phantom broker order raises an alert and blocks new orders", "[reconcile]") {
  life::LifecycleEngine engine;
  CountingAlertSink alerts;
  rec::ReconcileApplier applier(engine, alerts);

  std::vector<Order> local;  // bot created nothing
  const auto result = result_with({broker_order("ghost-1", OrderState::Acknowledged)}, 10);

  const rec::ReconcileOutcome outcome = applier.apply(result, local);
  CHECK(outcome.mismatches == 1);
  CHECK(outcome.block_new_orders);
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Warning);
  // IMP-16: the unmatched order is NAMED, in the typed context rather than
  // interpolated into the free-form body (which a real sink scrubs). The old
  // `ref_of()` helper carried the client_ref OR the broker id; the typed columns
  // carry BOTH when both exist.
  CHECK(alerts.last_provenance().client_ref == "ghost-1");
  CHECK(alerts.last_message().find("ghost-1") == std::string::npos);
}

TEST_CASE("apply: a pre-ack local order (empty broker id) absent from snapshot is NOT a mismatch",
          "[reconcile]") {
  life::LifecycleEngine engine;
  CountingAlertSink alerts;
  rec::ReconcileApplier applier(engine, alerts);

  // A freshly-placed order the broker has not listed yet: non-terminal (Sent) but
  // with NO broker_order_id (no ack ever observed). Its absence from the snapshot
  // is the normal place->reconcile race, NOT a mismatch (Fix 1).
  Order pending = local_order("alpha-9", OrderState::Sent);
  pending.broker_order_id.clear();
  std::vector<Order> local{pending};
  const auto result = result_with({}, 10);  // empty broker snapshot

  const rec::ReconcileOutcome outcome = applier.apply(result, local);
  CHECK(outcome.mismatches == 0);
  CHECK_FALSE(outcome.block_new_orders);
  CHECK(alerts.count() == 0);
}

TEST_CASE("apply: a vanished acked local order (non-empty broker id) is flagged + blocks",
          "[reconcile]") {
  life::LifecycleEngine engine;
  CountingAlertSink alerts;
  rec::ReconcileApplier applier(engine, alerts);

  // The broker had already acked this order (non-empty broker_order_id from the
  // local_order helper), so its disappearance from the snapshot is the real
  // manual-intervention candidate -> alert + block (Fix 1).
  std::vector<Order> local{local_order("alpha-9", OrderState::Sent)};
  REQUIRE_FALSE(local.front().broker_order_id.empty());
  const auto result = result_with({}, 10);  // empty broker snapshot

  const rec::ReconcileOutcome outcome = applier.apply(result, local);
  CHECK(outcome.mismatches == 1);
  CHECK(outcome.block_new_orders);
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Warning);
  // IMP-16: the vanished order is NAMED in the typed context — both ids, since a
  // broker-acked local order has both.
  CHECK(alerts.last_provenance().client_ref == "alpha-9");
  CHECK_FALSE(alerts.last_provenance().broker_order_id.empty());
  CHECK(alerts.last_message().find("alpha-9") == std::string::npos);
}

TEST_CASE("apply: a stale (older ordering_key) snapshot does not re-escalate mismatches",
          "[reconcile]") {
  life::LifecycleEngine engine;
  CountingAlertSink alerts;
  rec::ReconcileApplier applier(engine, alerts);

  std::vector<Order> local;  // bot created nothing

  // A fresh snapshot at key 10 with a phantom order -> mismatch + block.
  const rec::ReconcileOutcome fresh =
      applier.apply(result_with({broker_order("ghost-1", OrderState::Acknowledged)}, 10), local);
  CHECK(fresh.mismatches == 1);
  CHECK(fresh.block_new_orders);
  CHECK(alerts.count() == 1);

  // A later-arriving OLDER snapshot at key 5, also carrying a phantom, is below
  // the high-water snapshot key: it must NOT add a mismatch or set block (Fix 2).
  const rec::ReconcileOutcome stale =
      applier.apply(result_with({broker_order("ghost-2", OrderState::Acknowledged)}, 5), local);
  CHECK(stale.mismatches == 0);
  CHECK_FALSE(stale.block_new_orders);
  CHECK(alerts.count() == 1);  // no new alert raised by the stale snapshot
}

TEST_CASE("apply: a vanished terminal local order is NOT a mismatch", "[reconcile]") {
  life::LifecycleEngine engine;
  CountingAlertSink alerts;
  rec::ReconcileApplier applier(engine, alerts);

  std::vector<Order> local{local_order("alpha-done", OrderState::Filled)};
  const rec::ReconcileOutcome outcome = applier.apply(result_with({}, 10), local);
  CHECK(outcome.mismatches == 0);
  CHECK_FALSE(outcome.block_new_orders);
  CHECK(alerts.count() == 0);
}

// ── cadence (AC-2) ──

TEST_CASE("cadence: in-flight or open position -> tight; flat -> loose", "[reconcile]") {
  // Any in-flight (SENT) order -> tight.
  const rec::ReconcileState inflight = rec::derive_state({local_order("a", OrderState::Sent)}, {});
  CHECK(inflight.any_inflight);
  CHECK(rec::next_cadence(inflight) == std::chrono::milliseconds(1500));

  // Flat: a terminal order, no positions -> loose.
  const rec::ReconcileState flat = rec::derive_state({local_order("a", OrderState::Filled)}, {});
  CHECK_FALSE(flat.any_inflight);
  CHECK_FALSE(flat.any_open_position);
  CHECK(rec::next_cadence(flat) == std::chrono::milliseconds(20000));

  // An open position (net_qty != 0) -> tight even with no in-flight order.
  Position pos;
  pos.symbol = "NIFTY";
  pos.net_qty = Quantity::of(-50);  // short
  const rec::ReconcileState open = rec::derive_state({local_order("a", OrderState::Filled)}, {pos});
  CHECK(open.any_open_position);
  CHECK(rec::next_cadence(open) == std::chrono::milliseconds(1500));

  // A zero net_qty position is flat.
  Position zero;
  zero.symbol = "NIFTY";
  zero.net_qty = Quantity::of(0);
  const rec::ReconcileState zero_state = rec::derive_state({}, {zero});
  CHECK_FALSE(zero_state.any_open_position);
}

TEST_CASE("cadence: Unknown is in-flight (uncertain) -> tight", "[reconcile]") {
  const rec::ReconcileState s = rec::derive_state({local_order("a", OrderState::Unknown)}, {});
  CHECK(s.any_inflight);
  CHECK(rec::next_cadence(s) == std::chrono::milliseconds(1500));
}
