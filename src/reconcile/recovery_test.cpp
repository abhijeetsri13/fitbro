#include "broker_exec/reconcile/recovery.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

#include "broker_exec/adapters/fake/fake_broker.hpp"
#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/lifecycle/lifecycle.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

using broker_exec::domain::Order;
using broker_exec::domain::OrderIntent;
using broker_exec::domain::OrderState;
using broker_exec::domain::Price;
using broker_exec::domain::Quantity;
using broker_exec::ports::AlertLevel;

namespace fake = broker_exec::adapters::fake;
namespace life = broker_exec::lifecycle;
namespace ports = broker_exec::ports;
namespace rec = broker_exec::reconcile;

namespace {

// Recording AlertSink: counts alerts and tracks the highest-severity (Critical)
// escalations, so a test can assert that the double-fault path escalates Critical
// while the safe paths stay quiet. Mirrors reconcile_test.cpp's sink.
class CountingAlertSink final : public ports::AlertSink {
 public:
  broker_exec::Result<ports::Ok> send(AlertLevel level, const std::string& message) override {
    ++count_;
    last_level_ = level;
    last_message_ = message;
    if (level == AlertLevel::Critical) {
      ++critical_count_;
    }
    return ports::ok();
  }
  broker_exec::Result<ports::Ok> send_test_alert() override { return ports::ok(); }

  [[nodiscard]] std::size_t count() const noexcept { return count_; }
  [[nodiscard]] std::size_t critical_count() const noexcept { return critical_count_; }
  [[nodiscard]] AlertLevel last_level() const noexcept { return last_level_; }
  [[nodiscard]] const std::string& last_message() const noexcept { return last_message_; }

 private:
  std::size_t count_ = 0;
  std::size_t critical_count_ = 0;
  AlertLevel last_level_ = AlertLevel::Info;
  std::string last_message_;
};

// A hand-built loaded (replayed) order in a given state. `acked` controls whether
// the broker_order_id is set (an acked order can "vanish"; a pre-ack one cannot).
Order loaded_order(const std::string& client_ref, OrderState state, bool acked = true) {
  Order o;
  o.intent.client_ref = client_ref;
  o.state = state;
  if (acked) {
    o.broker_order_id = "BRK-" + client_ref;
  }
  return o;
}

// Seam helpers: a Result<Ok> that succeeds / fails, and a load_state that yields
// a fixed vector / fails.
broker_exec::Result<ports::Ok> ok_session() { return ports::ok(); }
broker_exec::Result<ports::Ok> bad_session() {
  return broker_exec::fail(broker_exec::errors::make_error(
      broker_exec::errors::ErrorCategory::SessionExpired, "no session"));
}

// Place an order into the FakeBroker's broker-truth book so a later fetch_orders()
// reveals it (the fake fully-fills a placed order). This is TEST SETUP, not part
// of recovery — recovery itself never places. Returns the client_ref used.
void seed_broker_truth(fake::FakeBroker& broker, const std::string& client_ref) {
  OrderIntent intent;
  intent.client_ref = client_ref;
  intent.symbol = "NIFTY";
  intent.quantity = Quantity::of(50);
  intent.price = Price::from_rupees(100);
  REQUIRE(broker.place(intent));  // enters the book as Filled
}

// Count book entries the fake considers Cancelled — the footprint a square_off or
// cancel would leave. Recovery must never produce one.
[[nodiscard]] std::size_t cancelled_count(const fake::FakeBroker& broker) {
  std::size_t n = 0;
  for (const auto& entry : broker.book()) {
    if (entry.order.state == OrderState::Cancelled) {
      ++n;
    }
  }
  return n;
}

}  // namespace

// ── CLEAN (AC-1, AC-2): converge to broker truth, no unknowns -> ResumedSafe ──

TEST_CASE("recover: clean reconcile resumes safe and issues no broker mutation", "[recovery]") {
  broker_exec::clock::TestClock clock;
  fake::FakeBroker broker(clock);
  CountingAlertSink alerts;
  life::LifecycleEngine engine;

  // Broker truth holds the order (Filled); the loaded order is Acknowledged and
  // will converge forward to Filled — no Unknowns, nothing to escalate.
  seed_broker_truth(broker, "alpha-1");
  const std::size_t book_before = broker.book().size();

  auto load_state = []() -> broker_exec::Result<std::vector<Order>> {
    return std::vector<Order>{loaded_order("alpha-1", OrderState::Acknowledged)};
  };
  rec::RecoveryCoordinator coord(broker, alerts, clock, engine, load_state, ok_session,
                                 ok_session);

  const rec::RecoveryOutcome out = coord.recover();
  CHECK(out.status == rec::RecoveryStatus::ResumedSafe);
  CHECK(out.unknowns_unresolved == 0);
  CHECK_FALSE(out.escalated);
  CHECK(out.orders.front().state == OrderState::Filled);  // converged to truth

  // AC-2 zero duplicates: recovery placed/squared NOTHING — the book is unchanged
  // (no new entry) and no entry was cancelled/squared-off.
  CHECK(broker.book().size() == book_before);
  CHECK(cancelled_count(broker) == 0);
  CHECK(alerts.critical_count() == 0);
}

// ── UNKNOWN RESOLVED (AC-1): an Unknown order that broker truth advances ──

TEST_CASE("recover: an Unknown order resolved by broker truth resumes safe", "[recovery]") {
  broker_exec::clock::TestClock clock;
  fake::FakeBroker broker(clock);
  CountingAlertSink alerts;
  life::LifecycleEngine engine;

  // Broker truth holds the order (Filled). The loaded order is UNKNOWN with the
  // same client_ref, so the applier advances Unknown -> Filled (a legal resolve).
  seed_broker_truth(broker, "alpha-1");

  auto load_state = []() -> broker_exec::Result<std::vector<Order>> {
    return std::vector<Order>{loaded_order("alpha-1", OrderState::Unknown)};
  };
  rec::RecoveryCoordinator coord(broker, alerts, clock, engine, load_state, ok_session,
                                 ok_session);

  const rec::RecoveryOutcome out = coord.recover();
  CHECK(out.status == rec::RecoveryStatus::ResumedSafe);
  CHECK(out.unknowns_resolved == 1);
  CHECK(out.unknowns_unresolved == 0);
  CHECK(out.orders.front().state == OrderState::Filled);
  CHECK(cancelled_count(broker) == 0);  // never squared off
}

// ── UNKNOWN UNRESOLVED (AC-1): broker reachable, no match -> Blocked ──

TEST_CASE("recover: an Unknown order with no broker match blocks (not resumed)", "[recovery]") {
  broker_exec::clock::TestClock clock;
  fake::FakeBroker broker(clock);  // benign, but its book is EMPTY (no match)
  CountingAlertSink alerts;
  life::LifecycleEngine engine;

  // Pre-ack Unknown (empty broker_order_id) so its absence is not a vanished
  // mismatch — it simply stays Unknown (unresolvable) and blocks the resume.
  auto load_state = []() -> broker_exec::Result<std::vector<Order>> {
    return std::vector<Order>{loaded_order("ghost-1", OrderState::Unknown, /*acked=*/false)};
  };
  rec::RecoveryCoordinator coord(broker, alerts, clock, engine, load_state, ok_session,
                                 ok_session);

  const rec::RecoveryOutcome out = coord.recover();
  CHECK(out.status == rec::RecoveryStatus::Blocked);
  CHECK(out.unknowns_unresolved == 1);
  CHECK_FALSE(out.escalated);
  CHECK(out.orders.front().state == OrderState::Unknown);  // not auto-resolved
  CHECK(cancelled_count(broker) == 0);                     // NOT squared off
}

// ── DOUBLE FAULT (AC-3): Unknown + broker unreachable -> MANUAL_INTERVENTION ──

TEST_CASE("recover: Unknown order + unreachable broker escalates to manual intervention",
          "[recovery]") {
  broker_exec::clock::TestClock clock;
  // rate_limit_after = 0 -> every read fails as RateLimited: the broker is
  // UNREACHABLE for recovery's fetch. Rejected requests are NOT counted, so the
  // request_count stays 0 (a clean witness that recovery issued no calls).
  fake::FaultConfig cfg;
  cfg.rate_limit_after = 0;
  fake::FakeBroker broker(clock, cfg);
  CountingAlertSink alerts;
  life::LifecycleEngine engine;

  auto load_state = []() -> broker_exec::Result<std::vector<Order>> {
    return std::vector<Order>{loaded_order("alpha-1", OrderState::Unknown)};
  };
  rec::RecoveryCoordinator coord(broker, alerts, clock, engine, load_state, ok_session,
                                 ok_session);

  const rec::RecoveryOutcome out = coord.recover();
  CHECK(out.status == rec::RecoveryStatus::ManualInterventionRequired);
  CHECK(out.escalated);
  CHECK(out.unknowns_unresolved == 1);
  CHECK(out.unknowns_resolved == 0);
  // The ambiguous order is escalated, never auto-squared.
  CHECK(out.orders.front().state == OrderState::ManualInterventionRequired);
  // A Critical escalation was sent.
  CHECK(alerts.critical_count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Critical);
  // No broker mutation of ANY kind: nothing placed, nothing squared off.
  CHECK(broker.book().empty());
  CHECK(broker.request_count() == 0);
  CHECK(cancelled_count(broker) == 0);
}

// ── SESSION BAD (AC-1): dead session -> Blocked, never resume ──

TEST_CASE("recover: a dead session blocks before any fetch", "[recovery]") {
  broker_exec::clock::TestClock clock;
  fake::FakeBroker broker(clock);
  CountingAlertSink alerts;
  life::LifecycleEngine engine;

  auto load_state = []() -> broker_exec::Result<std::vector<Order>> {
    return std::vector<Order>{loaded_order("alpha-1", OrderState::Sent)};
  };
  rec::RecoveryCoordinator coord(broker, alerts, clock, engine, load_state, bad_session,
                                 ok_session);

  const rec::RecoveryOutcome out = coord.recover();
  CHECK(out.status == rec::RecoveryStatus::Blocked);
  CHECK_FALSE(out.escalated);
  // Fetch never ran: the broker received no request at all.
  CHECK(broker.request_count() == 0);
  CHECK(cancelled_count(broker) == 0);
}

// ── SAFE-START FAIL (AC-1): all else ok, gate refuses -> Blocked ──

TEST_CASE("recover: a refused safe-start gate blocks the resume", "[recovery]") {
  broker_exec::clock::TestClock clock;
  fake::FakeBroker broker(clock);
  CountingAlertSink alerts;
  life::LifecycleEngine engine;

  seed_broker_truth(broker, "alpha-1");
  auto load_state = []() -> broker_exec::Result<std::vector<Order>> {
    return std::vector<Order>{loaded_order("alpha-1", OrderState::Acknowledged)};
  };
  // Session ok, but the safe-start gate refuses.
  rec::RecoveryCoordinator coord(broker, alerts, clock, engine, load_state, ok_session,
                                 bad_session);

  const rec::RecoveryOutcome out = coord.recover();
  CHECK(out.status == rec::RecoveryStatus::Blocked);
  CHECK_FALSE(out.escalated);
  CHECK(cancelled_count(broker) == 0);
}

// ── LOAD FAIL (AC-1): cannot replay state -> Blocked + alert ──

TEST_CASE("recover: a failed state load blocks and alerts", "[recovery]") {
  broker_exec::clock::TestClock clock;
  fake::FakeBroker broker(clock);
  CountingAlertSink alerts;
  life::LifecycleEngine engine;

  auto load_state = []() -> broker_exec::Result<std::vector<Order>> {
    return broker_exec::fail(
        broker_exec::errors::make_error(broker_exec::errors::ErrorCategory::Internal, "no state"));
  };
  rec::RecoveryCoordinator coord(broker, alerts, clock, engine, load_state, ok_session,
                                 ok_session);

  const rec::RecoveryOutcome out = coord.recover();
  CHECK(out.status == rec::RecoveryStatus::Blocked);
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Warning);
  CHECK(broker.request_count() == 0);  // never reached the broker
}

// ── BROKER OUTAGE, NO UNKNOWN (AC-3 boundary): unreachable broker but nothing
// in-flight -> Blocked (retryable), NEVER ManualInterventionRequired ──

TEST_CASE("recover: unreachable broker with no Unknown order blocks but never escalates",
          "[recovery]") {
  broker_exec::clock::TestClock clock;
  // rate_limit_after = 0 -> every read fails as RateLimited: the broker is
  // UNREACHABLE for recovery's fetch (mirrors the double-fault test's setup).
  fake::FaultConfig cfg;
  cfg.rate_limit_after = 0;
  fake::FakeBroker broker(clock, cfg);
  CountingAlertSink alerts;
  life::LifecycleEngine engine;

  // Crucially the loaded orders are all NON-Unknown (Acknowledged + Sent): there
  // is no ambiguous in-flight order, so an unreachable broker is a recoverable
  // outage, NOT a double fault. Recovery must block-and-retry, not escalate.
  auto load_state = []() -> broker_exec::Result<std::vector<Order>> {
    return std::vector<Order>{loaded_order("alpha-1", OrderState::Acknowledged),
                              loaded_order("beta-2", OrderState::Sent)};
  };
  rec::RecoveryCoordinator coord(broker, alerts, clock, engine, load_state, ok_session,
                                 ok_session);

  const rec::RecoveryOutcome out = coord.recover();
  CHECK(out.status == rec::RecoveryStatus::Blocked);
  CHECK(out.status != rec::RecoveryStatus::ManualInterventionRequired);
  CHECK_FALSE(out.escalated);
  // A recoverable outage with no in-flight UNKNOWN must NOT mutate and must NOT
  // escalate: nothing placed/squared, no rejected request counted, no Critical.
  CHECK(broker.request_count() == 0);
  CHECK(cancelled_count(broker) == 0);
  CHECK(alerts.critical_count() == 0);
}

// ── PHANTOM MISMATCH (Fix 1): clean loaded orders, session + safe-start ok, but
// the broker reveals a phantom -> Blocked, NOT ResumedSafe ──

TEST_CASE("recover: a phantom broker order blocks the resume despite a clean local view",
          "[recovery]") {
  broker_exec::clock::TestClock clock;
  fake::FakeBroker broker(clock);
  CountingAlertSink alerts;
  life::LifecycleEngine engine;

  // Broker truth holds the loaded order ("alpha-1", which converges forward) AND
  // a PHANTOM ("phantom-1") that is live at the broker but absent from replayed
  // state — exactly the crash shape: the bot placed but died before the intent
  // log was complete. The applier sees phantom-1 with no local match -> mismatch
  // + block_new_orders. Recovery must NOT resume even though every other gate is
  // green and there are no Unknowns.
  seed_broker_truth(broker, "alpha-1");
  seed_broker_truth(broker, "phantom-1");

  auto load_state = []() -> broker_exec::Result<std::vector<Order>> {
    return std::vector<Order>{loaded_order("alpha-1", OrderState::Acknowledged)};
  };
  // Session ok AND safe-start ok: the ONLY thing standing between this and a
  // (wrong) ResumedSafe is the reconcile mismatch gate.
  rec::RecoveryCoordinator coord(broker, alerts, clock, engine, load_state, ok_session,
                                 ok_session);

  const rec::RecoveryOutcome out = coord.recover();
  CHECK(out.status == rec::RecoveryStatus::Blocked);
  CHECK(out.status != rec::RecoveryStatus::ResumedSafe);
  CHECK(out.mismatches > 0);          // the phantom was counted
  CHECK(out.unknowns_unresolved == 0);  // no ambiguity — purely the phantom gate
  CHECK_FALSE(out.escalated);
  CHECK(cancelled_count(broker) == 0);  // never squared off the phantom
}
