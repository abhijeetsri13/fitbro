#include "broker_exec/protection/stop_supervisor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

using broker_exec::Result;
using broker_exec::domain::Money;
using broker_exec::domain::OrderState;
using broker_exec::domain::Side;
using broker_exec::protection::evaluate_protection;
using broker_exec::protection::PriceBand;
using broker_exec::protection::ProtectionState;
using broker_exec::protection::ProtectiveStop;
using broker_exec::protection::StopInputs;
using broker_exec::ports::AlertLevel;

namespace ports = broker_exec::ports;
namespace errors = broker_exec::errors;

namespace {

// Spy AlertSink: records the alert count + last level/message, and can be told to
// FAIL the send (return an Error) so we can prove a dead alert channel never
// suppresses the protective decision. Mirrors the recording sinks in the sibling
// modules' tests.
class SpyAlertSink final : public ports::AlertSink {
 public:
  explicit SpyAlertSink(bool fail_send = false) : fail_send_(fail_send) {}

  Result<ports::Ok> send(AlertLevel level, const std::string& message) override {
    ++count_;
    last_level_ = level;
    last_message_ = message;
    last_provenance_ = ports::AlertContext{};
    if (fail_send_) {
      return broker_exec::fail(
          errors::make_error(errors::ErrorCategory::Network, "alert channel down"));
    }
    return ports::ok();
  }
  // IMP-16: the SYMBOL rides in the typed context now (a real index-option symbol
  // is >=20 chars and would be redacted inside the scrubbed body), so the spy has
  // to record it — the base default would silently drop it and every assertion
  // below would pass against an alert that named no instrument.
  Result<ports::Ok> send_with_context(AlertLevel level, const std::string& message,
                                      const ports::AlertContext& provenance) override {
    const Result<ports::Ok> out = send(level, message);
    last_provenance_ = provenance;
    return out;
  }
  Result<ports::Ok> send_test_alert() override { return ports::ok(); }

  [[nodiscard]] std::size_t count() const noexcept { return count_; }
  [[nodiscard]] AlertLevel last_level() const noexcept { return last_level_; }
  [[nodiscard]] const std::string& last_message() const noexcept { return last_message_; }
  [[nodiscard]] const ports::AlertContext& last_provenance() const noexcept {
    return last_provenance_;
  }

 private:
  bool fail_send_;
  std::size_t count_ = 0;
  AlertLevel last_level_ = AlertLevel::Info;
  std::string last_message_;
  ports::AlertContext last_provenance_;
};

// An AlertSink that THROWS from send() — models a real comms adapter blowing up
// in exactly the volatile conditions that trigger a re-arm. The protective
// decision must still come back intact.
class ThrowingAlertSink final : public ports::AlertSink {
 public:
  Result<ports::Ok> send(AlertLevel, const std::string&) override {
    throw std::runtime_error("alert transport blew up");
  }
  Result<ports::Ok> send_test_alert() override { return ports::ok(); }
};

// A wide, valid band that never clamps so re-arm tests can assert on side/qty
// without the price moving.
[[nodiscard]] PriceBand wide_band() {
  return PriceBand{Money::from_rupees(1), Money::from_rupees(100000), true};
}

// A long protected position with a sensible protective limit inside wide_band.
[[nodiscard]] ProtectiveStop long_stop(std::int64_t qty = 50) {
  ProtectiveStop stop;
  stop.position_id = "POS-1";
  stop.symbol = "NIFTY24JUNFUT";
  stop.position_qty = qty;  // +long
  stop.stop_trigger = Money::from_rupees(100);
  stop.protective_limit = Money::from_rupees(99);
  return stop;
}

}  // namespace

TEST_CASE("flat position (qty 0) => Closed, no emit, no alert") {
  SpyAlertSink alerts;
  ProtectiveStop stop = long_stop(/*qty=*/0);

  StopInputs in;
  in.trigger_crossed = true;  // even with a crossed trigger, flat = nothing to protect
  in.band = wide_band();

  const auto d = evaluate_protection(stop, in, alerts);

  CHECK(d.state == ProtectionState::Closed);
  CHECK_FALSE(d.emit_exit);
  CHECK_FALSE(d.alert);
  CHECK(alerts.count() == 0);
}

TEST_CASE("armed: long position, trigger NOT crossed => Armed, no emit") {
  SpyAlertSink alerts;
  ProtectiveStop stop = long_stop();

  StopInputs in;
  in.trigger_crossed = false;
  in.protective_order_known = false;
  in.band = wide_band();

  const auto d = evaluate_protection(stop, in, alerts);

  CHECK(d.state == ProtectionState::Armed);
  CHECK_FALSE(d.emit_exit);
  CHECK_FALSE(d.alert);
  CHECK(alerts.count() == 0);
}

TEST_CASE("protected: the protective exit flattened the position (qty 0) => Closed, no emit") {
  SpyAlertSink alerts;
  // A real protective fill is reflected as a flat position (qty 0) — exposure is
  // read from the live position, not an order flag.
  ProtectiveStop stop = long_stop(/*qty=*/0);

  StopInputs in;
  in.trigger_crossed = true;
  in.protective_order_known = true;
  in.protective_order_state = OrderState::Filled;
  in.band = wide_band();

  const auto d = evaluate_protection(stop, in, alerts);

  CHECK(d.state == ProtectionState::Closed);
  CHECK_FALSE(d.emit_exit);
  CHECK_FALSE(d.alert);
  CHECK(alerts.count() == 0);
}

TEST_CASE("FAIL-OPEN GUARD: a Filled flag while the position is STILL exposed re-arms (live "
          "position wins over the order flag)") {
  // The HIGH defect the review caught: a stale/leftover/residual Filled flag must
  // NOT close an exposed (non-zero) crossed position — that would leave it naked.
  for (const bool known : {true, false}) {
    SpyAlertSink alerts;
    ProtectiveStop stop = long_stop(/*qty=*/50);  // STILL exposed

    StopInputs in;
    in.trigger_crossed = true;
    in.protective_order_known = known;
    in.protective_order_state = OrderState::Filled;  // claims filled...
    in.band = wide_band();

    const auto d = evaluate_protection(stop, in, alerts);

    CHECK(d.state == ProtectionState::ReArmNeeded);  // ...but the live position is exposed
    CHECK(d.emit_exit);
    CHECK(d.exit.side == Side::Sell);
    CHECK(d.exit.qty == 50);
    CHECK(alerts.last_level() == AlertLevel::Critical);
  }
}

// ── THE CORE CASE: GTT fired-but-unfilled drives a re-arm for EACH failure. ──

TEST_CASE("RE-ARM: long, trigger crossed, protective order Rejected => ReArmNeeded Sell exit + Critical") {
  SpyAlertSink alerts;
  ProtectiveStop stop = long_stop(/*qty=*/75);

  StopInputs in;
  in.trigger_crossed = true;
  in.protective_order_known = true;
  in.protective_order_state = OrderState::Rejected;
  in.band = wide_band();

  const auto d = evaluate_protection(stop, in, alerts);

  CHECK(d.state == ProtectionState::ReArmNeeded);
  CHECK(d.emit_exit);
  CHECK(d.exit.side == Side::Sell);       // long exits Sell
  CHECK(d.exit.qty == 75);                // abs(position_qty)
  CHECK(d.exit.symbol == stop.symbol);
  CHECK(d.alert);
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Critical);
  // IMP-16: the alert NAMES THE INSTRUMENT, via the typed context rather than the
  // body. "your position is unprotected" is worthless without it, and interpolated
  // into the scrubbed body a real >=20-char option symbol is destroyed.
  CHECK(alerts.last_provenance().symbol == stop.symbol);
  CHECK(alerts.last_message().find(stop.symbol) == std::string::npos);
}

TEST_CASE("RE-ARM: a >=20-char option symbol survives the protective alert (IMP-16 / M4)",
          "[protection][provenance]") {
  // The exact instruments this library trades. scrub() redacts any >=20-char run
  // mixing letters and digits, so BANKNIFTY24JUN52000CE (21) interpolated into the
  // alert body reached the operator as ***REDACTED*** — the most urgent alert in
  // the module, naming no contract. NIFTY24JUNFUT survived only by being shorter.
  SpyAlertSink alerts;
  ProtectiveStop stop = long_stop(/*qty=*/75);
  stop.symbol = "BANKNIFTY24JUN52000CE";

  StopInputs in;
  in.trigger_crossed = true;
  in.protective_order_known = true;
  in.protective_order_state = OrderState::Rejected;
  in.band = wide_band();

  const auto d = evaluate_protection(stop, in, alerts);

  CHECK(d.state == ProtectionState::ReArmNeeded);
  REQUIRE(alerts.count() == 1);
  CHECK(alerts.last_provenance().symbol == "BANKNIFTY24JUN52000CE");
  CHECK(alerts.last_message().find("BANKNIFTY") == std::string::npos);
  // `decision.detail` is an in-process record that never meets a scrubbing sink,
  // so it still carries the symbol inline for the caller and the audit log.
  CHECK(d.detail.find("BANKNIFTY24JUN52000CE") != std::string::npos);
}

TEST_CASE("RE-ARM: every fired-but-unfilled state drives a re-arm") {
  ProtectiveStop stop = long_stop(/*qty=*/40);

  SECTION("Cancelled") {
    SpyAlertSink alerts;
    StopInputs in;
    in.trigger_crossed = true;
    in.protective_order_known = true;
    in.protective_order_state = OrderState::Cancelled;
    in.band = wide_band();
    const auto d = evaluate_protection(stop, in, alerts);
    CHECK(d.state == ProtectionState::ReArmNeeded);
    CHECK(d.emit_exit);
    CHECK(d.exit.side == Side::Sell);
    CHECK(d.exit.qty == 40);
    CHECK(alerts.last_level() == AlertLevel::Critical);
  }
  SECTION("Unknown") {
    SpyAlertSink alerts;
    StopInputs in;
    in.trigger_crossed = true;
    in.protective_order_known = true;
    in.protective_order_state = OrderState::Unknown;
    in.band = wide_band();
    const auto d = evaluate_protection(stop, in, alerts);
    CHECK(d.state == ProtectionState::ReArmNeeded);
    CHECK(d.emit_exit);
    CHECK(alerts.last_level() == AlertLevel::Critical);
  }
  SECTION("PartiallyFilled (remainder still naked)") {
    SpyAlertSink alerts;
    StopInputs in;
    in.trigger_crossed = true;
    in.protective_order_known = true;
    in.protective_order_state = OrderState::PartiallyFilled;
    in.band = wide_band();
    const auto d = evaluate_protection(stop, in, alerts);
    CHECK(d.state == ProtectionState::ReArmNeeded);
    CHECK(d.emit_exit);
    CHECK(alerts.last_level() == AlertLevel::Critical);
  }
  SECTION("no protective order ever placed (!known)") {
    SpyAlertSink alerts;
    StopInputs in;
    in.trigger_crossed = true;
    in.protective_order_known = false;             // never placed
    in.protective_order_state = OrderState::Unknown;
    in.band = wide_band();
    const auto d = evaluate_protection(stop, in, alerts);
    CHECK(d.state == ProtectionState::ReArmNeeded);
    CHECK(d.emit_exit);
    CHECK(alerts.last_level() == AlertLevel::Critical);
  }
}

TEST_CASE("RE-ARM: short position exits Buy with abs(qty)") {
  SpyAlertSink alerts;
  ProtectiveStop stop = long_stop(/*qty=*/-60);  // short 60
  stop.stop_trigger = Money::from_rupees(100);
  stop.protective_limit = Money::from_rupees(101);  // marketable for a Buy exit

  StopInputs in;
  in.trigger_crossed = true;
  in.protective_order_known = true;
  in.protective_order_state = OrderState::Rejected;
  in.band = wide_band();

  const auto d = evaluate_protection(stop, in, alerts);

  CHECK(d.state == ProtectionState::ReArmNeeded);
  CHECK(d.exit.side == Side::Buy);  // short exits Buy
  CHECK(d.exit.qty == 60);
  CHECK(alerts.last_level() == AlertLevel::Critical);
}

// ── Band-aware clamp (the LPP/circuit fix). ──

TEST_CASE("band clamp: long exit (Sell) with protective_limit BELOW band.lower => clamped UP to band.lower") {
  SpyAlertSink alerts;
  ProtectiveStop stop = long_stop(/*qty=*/25);
  stop.protective_limit = Money::from_rupees(80);  // below the band floor

  StopInputs in;
  in.trigger_crossed = true;
  in.protective_order_known = true;
  in.protective_order_state = OrderState::Rejected;
  in.band = PriceBand{Money::from_rupees(90), Money::from_rupees(110), true};

  const auto d = evaluate_protection(stop, in, alerts);

  CHECK(d.state == ProtectionState::ReArmNeeded);
  CHECK(d.exit.side == Side::Sell);
  CHECK(d.exit.limit_price == Money::from_rupees(90));  // clamped up into the band
}

TEST_CASE("band clamp: short exit (Buy) with protective_limit ABOVE band.upper => clamped DOWN to band.upper") {
  SpyAlertSink alerts;
  ProtectiveStop stop = long_stop(/*qty=*/-25);          // short
  stop.protective_limit = Money::from_rupees(130);       // above the band ceiling

  StopInputs in;
  in.trigger_crossed = true;
  in.protective_order_known = true;
  in.protective_order_state = OrderState::Rejected;
  in.band = PriceBand{Money::from_rupees(90), Money::from_rupees(110), true};

  const auto d = evaluate_protection(stop, in, alerts);

  CHECK(d.state == ProtectionState::ReArmNeeded);
  CHECK(d.exit.side == Side::Buy);
  CHECK(d.exit.limit_price == Money::from_rupees(110));  // clamped down into the band
}

TEST_CASE("inverted band (lower>upper) is treated as unusable: emit unclamped + Critical, not an "
          "edge price the exchange would still reject") {
  SpyAlertSink alerts;
  ProtectiveStop stop = long_stop(/*qty=*/25);
  stop.protective_limit = Money::from_rupees(99);

  StopInputs in;
  in.trigger_crossed = true;
  in.protective_order_known = true;
  in.protective_order_state = OrderState::Rejected;
  in.band = PriceBand{Money::from_rupees(110), Money::from_rupees(90), true};  // inverted

  const auto d = evaluate_protection(stop, in, alerts);

  CHECK(d.state == ProtectionState::ReArmNeeded);
  CHECK(d.emit_exit);
  CHECK(d.exit.limit_price == Money::from_rupees(99));  // unclamped (not an inverted edge)
  CHECK(d.detail.find("band unknown") != std::string::npos);
  CHECK(alerts.last_level() == AlertLevel::Critical);
}

TEST_CASE("band clamp: a protective limit already inside the band is left unchanged") {
  SpyAlertSink alerts;
  ProtectiveStop stop = long_stop(/*qty=*/25);
  stop.protective_limit = Money::from_rupees(99);  // inside [90,110]

  StopInputs in;
  in.trigger_crossed = true;
  in.protective_order_known = true;
  in.protective_order_state = OrderState::Rejected;
  in.band = PriceBand{Money::from_rupees(90), Money::from_rupees(110), true};

  const auto d = evaluate_protection(stop, in, alerts);

  CHECK(d.exit.limit_price == Money::from_rupees(99));
}

// ── Band unknown (fail-closed for the price, NOT for the protection). ──

TEST_CASE("band unknown (valid==false): still EMITS the exit (unclamped) + Critical alert noting band unknown") {
  SpyAlertSink alerts;
  ProtectiveStop stop = long_stop(/*qty=*/30);
  stop.protective_limit = Money::from_rupees(99);

  StopInputs in;
  in.trigger_crossed = true;
  in.protective_order_known = true;
  in.protective_order_state = OrderState::Rejected;
  in.band = PriceBand{};  // valid == false

  const auto d = evaluate_protection(stop, in, alerts);

  CHECK(d.state == ProtectionState::ReArmNeeded);
  CHECK(d.emit_exit);                                   // the exit is NEVER skipped
  CHECK(d.exit.limit_price == Money::from_rupees(99));  // emitted unclamped (raw)
  CHECK(d.detail.find("band unknown") != std::string::npos);
  CHECK(d.alert);
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Critical);
}

// ── A throwing alert sink must never derail the protective decision. ──

TEST_CASE("throwing alert sink: evaluate_protection still returns the ReArmNeeded decision with emit_exit") {
  ThrowingAlertSink alerts;
  ProtectiveStop stop = long_stop(/*qty=*/55);

  StopInputs in;
  in.trigger_crossed = true;
  in.protective_order_known = true;
  in.protective_order_state = OrderState::Rejected;
  in.band = wide_band();

  // No throw escapes (no-throw contract) and the decision survives a dead channel.
  const auto d = evaluate_protection(stop, in, alerts);

  CHECK(d.state == ProtectionState::ReArmNeeded);
  CHECK(d.emit_exit);
  CHECK(d.exit.side == Side::Sell);
  CHECK(d.exit.qty == 55);
  CHECK(d.alert);  // an alert was attempted even though it threw
}

TEST_CASE("failing alert sink (returns Error) does not suppress the protective decision") {
  SpyAlertSink alerts(/*fail_send=*/true);
  ProtectiveStop stop = long_stop(/*qty=*/55);

  StopInputs in;
  in.trigger_crossed = true;
  in.protective_order_known = true;
  in.protective_order_state = OrderState::Rejected;
  in.band = wide_band();

  const auto d = evaluate_protection(stop, in, alerts);

  CHECK(d.state == ProtectionState::ReArmNeeded);
  CHECK(d.emit_exit);
  CHECK(alerts.count() == 1);  // the send was attempted (and returned Error)
}

// ── Fail-closed Unprotected for an unformable exit (qty overflow). ──

TEST_CASE("Unprotected: INT64_MIN qty cannot form a valid exit => Unprotected + Critical, no emit") {
  SpyAlertSink alerts;
  ProtectiveStop stop = long_stop();
  stop.position_qty = INT64_MIN;  // negation overflows -> magnitude stays non-positive

  StopInputs in;
  in.trigger_crossed = true;
  in.band = wide_band();

  const auto d = evaluate_protection(stop, in, alerts);

  CHECK(d.state == ProtectionState::Unprotected);
  CHECK_FALSE(d.emit_exit);   // cannot ship an exit with a bad quantity
  CHECK(d.alert);
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Critical);
}

TEST_CASE("to_string: stable state names") {
  CHECK(broker_exec::protection::to_string(ProtectionState::Armed) == "Armed");
  CHECK(broker_exec::protection::to_string(ProtectionState::Closed) == "Closed");
  CHECK(broker_exec::protection::to_string(ProtectionState::ReArmNeeded) == "ReArmNeeded");
  CHECK(broker_exec::protection::to_string(ProtectionState::Unprotected) == "Unprotected");
}
