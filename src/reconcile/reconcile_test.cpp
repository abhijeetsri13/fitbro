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
