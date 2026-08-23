#include "broker_exec/reconcile/manual_intervention.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>
#include <vector>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/reconcile/reconciler.hpp"

using broker_exec::domain::Order;
using broker_exec::domain::OrderState;
using broker_exec::domain::Position;
using broker_exec::domain::Price;
using broker_exec::domain::Quantity;
using broker_exec::ports::AlertLevel;

namespace rec = broker_exec::reconcile;

namespace {

using Kind = rec::ManualInterventionEvent::Kind;

// Records every alert so a test can assert that (and only that) a manual change
// escalates. Mirrors reconcile_test.cpp's CountingAlertSink.
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
  // IMP-16: the ids ride in the TYPED context now, so the stub records it (the
  // base default would drop it) and a test can assert the alert names the order.
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

// An AlertSink whose send ALWAYS fails, to prove the detector swallows the Result
// (no throw) and still returns the events.
class FailingAlertSink final : public broker_exec::ports::AlertSink {
 public:
  broker_exec::Result<broker_exec::ports::Ok> send(AlertLevel /*level*/,
                                                   const std::string& /*message*/) override {
    return broker_exec::fail(broker_exec::errors::make_error(
        broker_exec::errors::ErrorCategory::Unknown, "alert channel down"));
  }
  broker_exec::Result<broker_exec::ports::Ok> send_test_alert() override {
    return broker_exec::fail(broker_exec::errors::make_error(
        broker_exec::errors::ErrorCategory::Unknown, "alert channel down"));
  }
};

// A believed/local position in a symbol at a signed net quantity.
Position position(const std::string& symbol, std::int64_t net_qty) {
  Position p;
  p.symbol = symbol;
  p.net_qty = Quantity::of(net_qty);
  p.avg_price = Price::from_rupees(100);
  return p;
}

// A local order on a symbol in a given state, broker-acked (non-empty id) by
// default so it can "vanish".
Order local_order(const std::string& client_ref, const std::string& symbol, OrderState state) {
  Order o;
  o.intent.client_ref = client_ref;
  o.intent.symbol = symbol;
  o.state = state;
  o.broker_order_id = "BRK-" + client_ref;
  return o;
}

// A ReconcileResult carrying broker truth positions (+ optional orders).
rec::ReconcileResult truth_with(std::vector<Position> positions, std::vector<Order> orders = {}) {
  rec::ReconcileResult r;
  r.positions = std::move(positions);
  r.orders = std::move(orders);
  r.ordering_key = 1;
  return r;
}

}  // namespace

// ── AC-1 + AC-3: a manual close is detected, alerted, and named ──

TEST_CASE("detect: a believed-open position the broker shows flat is a manual close",
          "[manual_intervention]") {
  CountingAlertSink alerts;
  const rec::ManualInterventionDetector detector(alerts);

  const std::vector<Position> believed{position("X", 50)};
  const std::vector<Order> local_orders;  // no bot order on X
  const auto truth = truth_with({});      // X absent at broker == flat

  const auto events = detector.detect(believed, local_orders, truth);

  REQUIRE(events.size() == 1);
  CHECK(events.front().kind == Kind::PositionClosedManually);
  CHECK(events.front().symbol == "X");
  CHECK(events.front().believed_qty == 50);
  CHECK(events.front().broker_qty == 0);
  CHECK(alerts.count() > 0);
  CHECK(alerts.last_level() == AlertLevel::Warning);

  // IMP-16: the INSTRUMENT rides in the TYPED context, not in the free-form body.
  // A sink scrubs the body, and an option symbol of >=20 chars
  // (BANKNIFTY24JUN52000CE) is a token-shaped run there — so a manual-close alert
  // for a real index option named no instrument at all. `event.detail` is an
  // in-process record that never meets a sink, so it still carries the symbol.
  CHECK(alerts.last_provenance().symbol == "X");
  CHECK(alerts.last_message().find("X") == std::string::npos);
  CHECK(events.front().detail.find("X") != std::string::npos);
}

TEST_CASE("detect: a 21-char index-option symbol reaches the alert intact (IMP-16 / M4)",
          "[manual_intervention][provenance]") {
  // The symbol length that used to decide whether the operator learned WHICH
  // position was closed behind their back: scrub() redacts any >=20-char run
  // mixing letters and digits, and BANKNIFTY24JUN52000CE is 21.
  CountingAlertSink alerts;
  const rec::ManualInterventionDetector detector(alerts);

  const std::string banknifty = "BANKNIFTY24JUN52000CE";
  const std::vector<Position> believed{position(banknifty, 50)};
  const std::vector<Order> local_orders;
  const auto truth = truth_with({});

  const auto events = detector.detect(believed, local_orders, truth);

  REQUIRE(events.size() == 1);
  CHECK(alerts.count() == 1);
  // Carried as a typed column (rendered through the SYMBOL shape rule, which is
  // what keeps it out of scrub()'s hands) and absent from the scrubbed body.
  CHECK(alerts.last_provenance().symbol == banknifty);
  CHECK(alerts.last_message().find(banknifty) == std::string::npos);
  // The quantities still travel in the body — they are integers, not secrets.
  CHECK(alerts.last_message().find("believed=50") != std::string::npos);
}

TEST_CASE("detect: an explicitly-flat (net 0) broker position is also a manual close",
          "[manual_intervention]") {
  CountingAlertSink alerts;
  const rec::ManualInterventionDetector detector(alerts);

  const auto events = detector.detect({position("X", 50)}, {}, truth_with({position("X", 0)}));
  REQUIRE(events.size() == 1);
  CHECK(events.front().kind == Kind::PositionClosedManually);
}

// ── AC-2 (the crux): reconcile_positions -> needs_exit false; contrast still-open ──

TEST_CASE("reconcile_positions: a manually-closed position becomes flat -> no duplicate exit",
          "[manual_intervention]") {
  CountingAlertSink alerts;
  const rec::ManualInterventionDetector detector(alerts);

  std::vector<Position> local_positions{position("X", 50)};
  const auto truth = truth_with({});  // broker shows X flat (absent)

  const auto events = detector.detect(local_positions, {}, truth);
  REQUIRE(events.size() == 1);

  detector.reconcile_positions(truth, local_positions);

  // The local X is now flat, so needs_exit is false -> the bot would NOT send a
  // second/duplicate exit. This is the structural no-duplicate-exit guarantee.
  const Position* x = nullptr;
  for (const Position& p : local_positions) {
    if (p.symbol == "X") {
      x = &p;
    }
  }
  REQUIRE(x != nullptr);
  CHECK(x->net_qty == Quantity::of(0));
  CHECK_FALSE(rec::ManualInterventionDetector::needs_exit(*x));
}

TEST_CASE("reconcile_positions: a still-open broker position keeps needs_exit true",
          "[manual_intervention]") {
  CountingAlertSink alerts;
  const rec::ManualInterventionDetector detector(alerts);

  std::vector<Position> local_positions{position("X", 50)};
  const auto truth = truth_with({position("X", 50)});  // broker still long 50

  detector.reconcile_positions(truth, local_positions);

  REQUIRE(local_positions.size() == 1);
  CHECK(local_positions.front().net_qty == Quantity::of(50));
  CHECK(rec::ManualInterventionDetector::needs_exit(local_positions.front()));
}

// ── NOT a manual intervention: a close explained by a live bot order ──

TEST_CASE("detect: a flat position explained by a live bot order is NOT flagged",
          "[manual_intervention]") {
  CountingAlertSink alerts;
  const rec::ManualInterventionDetector detector(alerts);

  // The bot has a live (non-terminal) order on X that the broker STILL shows
  // (present in truth.orders) — a legit in-flight bot close, not a manual one.
  // The order must be present in broker truth, else it would (correctly) be a
  // separate OrderCancelledManually; here we isolate the position-suppression.
  const std::vector<Order> local_orders{local_order("exit-1", "X", OrderState::Sent)};
  const auto events =
      detector.detect({position("X", 50)}, local_orders, truth_with({}, local_orders));

  CHECK(events.empty());
  CHECK(alerts.count() == 0);
}

TEST_CASE("detect: a FILLED bot exit on the symbol explains the flat -> NOT a manual close",
          "[manual_intervention]") {
  CountingAlertSink alerts;
  const rec::ManualInterventionDetector detector(alerts);

  // A Filled bot order on X is a completed bot exit that plausibly closed the
  // position, so a flat broker state is explained by the bot, not a human -> the
  // manual-close flag is suppressed (safety bias: suppress over false-page).
  const std::vector<Order> local_orders{local_order("done-1", "X", OrderState::Filled)};
  const auto events = detector.detect({position("X", 50)}, local_orders, truth_with({}));

  CHECK(events.empty());
  CHECK(alerts.count() == 0);
}

// ── partial reduce ──

TEST_CASE("detect: a believed-100 position the broker shows at 40 is a manual reduce",
          "[manual_intervention]") {
  CountingAlertSink alerts;
  const rec::ManualInterventionDetector detector(alerts);

  std::vector<Position> local_positions{position("X", 100)};
  const auto truth = truth_with({position("X", 40)});  // reduced, no bot order

  const auto events = detector.detect(local_positions, {}, truth);
  REQUIRE(events.size() == 1);
  CHECK(events.front().kind == Kind::PositionReducedManually);
  CHECK(events.front().believed_qty == 100);
  CHECK(events.front().broker_qty == 40);
  CHECK(alerts.count() > 0);

  detector.reconcile_positions(truth, local_positions);
  REQUIRE(local_positions.size() == 1);
  CHECK(local_positions.front().net_qty == Quantity::of(40));
  // 40 still open -> the bot still needs to manage/exit the remaining position.
  CHECK(rec::ManualInterventionDetector::needs_exit(local_positions.front()));
}

TEST_CASE("detect: a broker magnitude >= believed (same sign) is NOT a reduce",
          "[manual_intervention]") {
  CountingAlertSink alerts;
  const rec::ManualInterventionDetector detector(alerts);

  // Broker grew the position (100 -> 120): nothing the close/reduce path flags.
  const auto events = detector.detect({position("X", 100)}, {}, truth_with({position("X", 120)}));
  CHECK(events.empty());
}

TEST_CASE("detect: a sign flip (long believed, broker short) is a manual reduce/reverse",
          "[manual_intervention]") {
  CountingAlertSink alerts;
  const rec::ManualInterventionDetector detector(alerts);

  const auto events = detector.detect({position("X", 50)}, {}, truth_with({position("X", -30)}));
  REQUIRE(events.size() == 1);
  CHECK(events.front().kind == Kind::PositionReducedManually);
  CHECK(events.front().believed_qty == 50);
  CHECK(events.front().broker_qty == -30);
}

TEST_CASE("detect: a sign flip whose broker magnitude EXCEEDS believed is still a manual reduce",
          "[manual_intervention]") {
  CountingAlertSink alerts;
  const rec::ManualInterventionDetector detector(alerts);

  // believed long 50, broker short 100: the sign flipped, so it is classified a
  // PositionReducedManually regardless of the larger broker magnitude (the
  // magnitude-grew exemption only applies when the SIGN is unchanged).
  const auto events = detector.detect({position("X", 50)}, {}, truth_with({position("X", -100)}));
  REQUIRE(events.size() == 1);
  CHECK(events.front().kind == Kind::PositionReducedManually);
  CHECK(events.front().believed_qty == 50);
  CHECK(events.front().broker_qty == -100);
}

// ── order manual-cancel ──

TEST_CASE("detect: a broker-acked live order absent at the broker is a manual cancel",
          "[manual_intervention]") {
  CountingAlertSink alerts;
  const rec::ManualInterventionDetector detector(alerts);

  // A non-terminal, broker-acked order with NO matching broker order in truth.
  const std::vector<Order> local_orders{local_order("ord-1", "X", OrderState::Acknowledged)};
  const auto truth = truth_with({}, /*orders=*/{});  // order absent at broker

  const auto events = detector.detect({}, local_orders, truth);
  REQUIRE(events.size() == 1);
  CHECK(events.front().kind == Kind::OrderCancelledManually);
  CHECK(events.front().client_ref == "ord-1");
  CHECK(events.front().symbol == "X");
  CHECK(alerts.count() > 0);
  CHECK(alerts.last_level() == AlertLevel::Warning);

  // IMP-16: the ALERT names the order too, via the TYPED context. Before this the
  // ref was interpolated into the free-form body, which a sink scrubs — so the
  // operator saw `ref=***REDACTED***`. `event.detail` (an in-process typed record
  // that never meets a sink) is unchanged and still carries the ref inline.
  CHECK(alerts.last_provenance().client_ref == "ord-1");
  CHECK(alerts.last_message().find("ord-1") == std::string::npos);
  CHECK(events.front().detail.find("ord-1") != std::string::npos);
}

TEST_CASE("detect: a broker order present-but-CANCELLED is a manual cancel",
          "[manual_intervention]") {
  CountingAlertSink alerts;
  const rec::ManualInterventionDetector detector(alerts);

  const std::vector<Order> local_orders{local_order("ord-1", "X", OrderState::Acknowledged)};
  const auto truth =
      truth_with({}, {local_order("ord-1", "X", OrderState::Cancelled)});  // broker says cancelled

  const auto events = detector.detect({}, local_orders, truth);
  REQUIRE(events.size() == 1);
  CHECK(events.front().kind == Kind::OrderCancelledManually);
}

TEST_CASE("detect: a pre-ack (empty broker id) order absent at the broker is NOT a cancel",
          "[manual_intervention]") {
  CountingAlertSink alerts;
  const rec::ManualInterventionDetector detector(alerts);

  Order pre_ack = local_order("ord-1", "X", OrderState::Sent);
  pre_ack.broker_order_id.clear();  // never acked -> a normal place->reconcile race
  const auto events = detector.detect({}, {pre_ack}, truth_with({}));
  CHECK(events.empty());
}

TEST_CASE("detect: a live order still present at the broker is NOT a cancel",
          "[manual_intervention]") {
  CountingAlertSink alerts;
  const rec::ManualInterventionDetector detector(alerts);

  const std::vector<Order> local_orders{local_order("ord-1", "X", OrderState::Acknowledged)};
  const auto truth = truth_with({}, {local_order("ord-1", "X", OrderState::Acknowledged)});

  const auto events = detector.detect({}, local_orders, truth);
  CHECK(events.empty());
}

// ── no-throw + stability ──

TEST_CASE("detect: a failing AlertSink still returns the events (no throw)",
          "[manual_intervention]") {
  FailingAlertSink alerts;  // every send returns an Error
  const rec::ManualInterventionDetector detector(alerts);

  const std::vector<Position> believed{position("X", 50)};
  const auto truth = truth_with({});

  std::vector<rec::ManualInterventionEvent> first;
  REQUIRE_NOTHROW(first = detector.detect(believed, {}, truth));
  REQUIRE(first.size() == 1);
  CHECK(first.front().kind == Kind::PositionClosedManually);

  // Detecting twice on the same truth is stable (same classification).
  const auto second = detector.detect(believed, {}, truth);
  REQUIRE(second.size() == 1);
  CHECK(second.front().kind == first.front().kind);
  CHECK(second.front().symbol == first.front().symbol);
  CHECK(second.front().believed_qty == first.front().believed_qty);
}
