#include "broker_exec/options/hedge_first.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

using broker_exec::Result;
using broker_exec::options::execute_hedge_first;
using broker_exec::options::HedgeFirstOutcome;
using broker_exec::options::HedgeFirstResult;
using broker_exec::options::HedgeFirstSeams;
using broker_exec::ports::AlertLevel;

namespace ports = broker_exec::ports;
namespace errors = broker_exec::errors;

namespace {

// Spy AlertSink: records the alert count + last level/message and can be
// configured to FAIL the send (return an Error) so we can prove that a dead
// alert channel never suppresses the emergency action. Mirrors the recording
// sinks in the health / marketdata tests.
class SpyAlertSink final : public ports::AlertSink {
 public:
  explicit SpyAlertSink(bool fail_send = false) : fail_send_(fail_send) {}

  Result<ports::Ok> send(AlertLevel level, const std::string& message) override {
    ++count_;
    last_level_ = level;
    last_message_ = message;
    if (fail_send_) {
      return broker_exec::fail(
          errors::make_error(errors::ErrorCategory::Network, "alert channel down"));
    }
    return ports::ok();
  }
  Result<ports::Ok> send_test_alert() override { return ports::ok(); }

  [[nodiscard]] std::size_t count() const noexcept { return count_; }
  [[nodiscard]] AlertLevel last_level() const noexcept { return last_level_; }
  [[nodiscard]] const std::string& last_message() const noexcept { return last_message_; }

 private:
  bool fail_send_;
  std::size_t count_ = 0;
  AlertLevel last_level_ = AlertLevel::Info;
  std::string last_message_;
};

// ── Seam result helpers ───────────────────────────────────────────────────
[[nodiscard]] Result<ports::BrokerAck> ok_ack(std::string id) {
  return ports::BrokerAck{std::move(id), "client-ref"};
}
[[nodiscard]] Result<ports::BrokerAck> ack_error() {
  return broker_exec::fail(errors::make_error(errors::ErrorCategory::Network, "place failed"));
}
[[nodiscard]] Result<bool> bool_error() {
  return broker_exec::fail(errors::make_error(errors::ErrorCategory::Timeout, "check failed"));
}

// Count how many times a given step was recorded in the call log.
[[nodiscard]] std::size_t count_of(const std::vector<std::string>& calls, const std::string& step) {
  return static_cast<std::size_t>(std::count(calls.begin(), calls.end(), step));
}

}  // namespace

TEST_CASE("AC-1 happy path: hedge -> confirm -> short -> recheck => HedgedShortLive, exact order") {
  std::vector<std::string> calls;
  SpyAlertSink alerts;

  HedgeFirstSeams seams;
  seams.place_hedge = [&]() -> Result<ports::BrokerAck> {
    calls.push_back("hedge");
    return ok_ack("HEDGE-1");
  };
  seams.confirm_hedge = [&](const std::string&) -> Result<bool> {
    calls.push_back("confirm");
    return true;
  };
  seams.place_short = [&]() -> Result<ports::BrokerAck> {
    calls.push_back("short");
    return ok_ack("SHORT-1");
  };
  seams.recheck_hedge_live = [&](const std::string&) -> Result<bool> {
    calls.push_back("recheck");
    return true;
  };
  seams.emergency_action = [&]() -> Result<ports::Ok> {
    calls.push_back("emergency");
    return ports::ok();
  };

  const HedgeFirstResult result = execute_hedge_first(seams, alerts);

  CHECK(result.outcome == HedgeFirstOutcome::HedgedShortLive);
  CHECK(result.hedge_order_id == "HEDGE-1");
  CHECK(result.short_order_id == "SHORT-1");
  CHECK_FALSE(result.emergency_action_ran);
  // The recorded sequence PROVES the hedge precedes the short (FR-16 never-naked).
  CHECK(calls == std::vector<std::string>{"hedge", "confirm", "short", "recheck"});
  // No remediation on the happy path.
  CHECK(count_of(calls, "emergency") == 0);
  CHECK(alerts.count() == 0);
}

TEST_CASE("AC-2 hedge placement fails: Error => HedgePlacementFailed, short NEVER invoked") {
  std::vector<std::string> calls;
  SpyAlertSink alerts;

  HedgeFirstSeams seams;
  seams.place_hedge = [&]() -> Result<ports::BrokerAck> {
    calls.push_back("hedge");
    return ack_error();
  };
  seams.confirm_hedge = [&](const std::string&) -> Result<bool> {
    calls.push_back("confirm");
    return true;
  };
  seams.place_short = [&]() -> Result<ports::BrokerAck> {
    calls.push_back("short");
    return ok_ack("SHORT-1");
  };

  const HedgeFirstResult result = execute_hedge_first(seams, alerts);

  CHECK(result.outcome == HedgeFirstOutcome::HedgePlacementFailed);
  CHECK(count_of(calls, "short") == 0);  // no naked window ever opened
  CHECK(count_of(calls, "confirm") == 0);
  CHECK(alerts.count() == 0);
}

TEST_CASE("AC-2 null place_hedge seam: fail-closed => HedgePlacementFailed, short NEVER invoked") {
  std::vector<std::string> calls;
  SpyAlertSink alerts;

  HedgeFirstSeams seams;  // place_hedge is null
  seams.place_short = [&]() -> Result<ports::BrokerAck> {
    calls.push_back("short");
    return ok_ack("SHORT-1");
  };

  const HedgeFirstResult result = execute_hedge_first(seams, alerts);

  CHECK(result.outcome == HedgeFirstOutcome::HedgePlacementFailed);
  CHECK(count_of(calls, "short") == 0);
}

TEST_CASE(
    "AC-2 hedge unconfirmed: confirm returns false => HedgeUnconfirmed, short NEVER invoked") {
  std::vector<std::string> calls;
  SpyAlertSink alerts;

  HedgeFirstSeams seams;
  seams.place_hedge = [&]() -> Result<ports::BrokerAck> {
    calls.push_back("hedge");
    return ok_ack("HEDGE-1");
  };
  seams.confirm_hedge = [&](const std::string&) -> Result<bool> {
    calls.push_back("confirm");
    return false;
  };
  seams.place_short = [&]() -> Result<ports::BrokerAck> {
    calls.push_back("short");
    return ok_ack("SHORT-1");
  };

  const HedgeFirstResult result = execute_hedge_first(seams, alerts);

  CHECK(result.outcome == HedgeFirstOutcome::HedgeUnconfirmed);
  CHECK(result.hedge_order_id == "HEDGE-1");
  CHECK(count_of(calls, "short") == 0);  // AC-2: never sent on unconfirmed hedge
  CHECK(alerts.count() == 0);
}

TEST_CASE(
    "AC-2 confirm returns Error: fail-closed-on-error => HedgeUnconfirmed, short NEVER invoked") {
  std::vector<std::string> calls;
  SpyAlertSink alerts;

  HedgeFirstSeams seams;
  seams.place_hedge = [&]() -> Result<ports::BrokerAck> {
    calls.push_back("hedge");
    return ok_ack("HEDGE-1");
  };
  seams.confirm_hedge = [&](const std::string&) -> Result<bool> {
    calls.push_back("confirm");
    return bool_error();  // an ERROR checking confirmation == NOT confirmed
  };
  seams.place_short = [&]() -> Result<ports::BrokerAck> {
    calls.push_back("short");
    return ok_ack("SHORT-1");
  };

  const HedgeFirstResult result = execute_hedge_first(seams, alerts);

  CHECK(result.outcome == HedgeFirstOutcome::HedgeUnconfirmed);
  CHECK(count_of(calls, "short") == 0);  // never proceed optimistically on error
}

TEST_CASE("AC-2 null confirm seam: fail-closed => HedgeUnconfirmed, short NEVER invoked") {
  std::vector<std::string> calls;
  SpyAlertSink alerts;

  HedgeFirstSeams seams;
  seams.place_hedge = [&]() -> Result<ports::BrokerAck> {
    calls.push_back("hedge");
    return ok_ack("HEDGE-1");
  };
  // confirm_hedge is null
  seams.place_short = [&]() -> Result<ports::BrokerAck> {
    calls.push_back("short");
    return ok_ack("SHORT-1");
  };

  const HedgeFirstResult result = execute_hedge_first(seams, alerts);

  CHECK(result.outcome == HedgeFirstOutcome::HedgeUnconfirmed);
  CHECK(count_of(calls, "short") == 0);
}

TEST_CASE(
    "short fails: confirmed hedge, place_short Error => ShortPlacementFailed, SAFE (no alert, no "
    "emergency)") {
  std::vector<std::string> calls;
  SpyAlertSink alerts;

  HedgeFirstSeams seams;
  seams.place_hedge = [&]() -> Result<ports::BrokerAck> {
    calls.push_back("hedge");
    return ok_ack("HEDGE-1");
  };
  seams.confirm_hedge = [&](const std::string&) -> Result<bool> {
    calls.push_back("confirm");
    return true;
  };
  seams.place_short = [&]() -> Result<ports::BrokerAck> {
    calls.push_back("short");
    return ack_error();
  };
  seams.recheck_hedge_live = [&](const std::string&) -> Result<bool> {
    calls.push_back("recheck");
    return true;
  };
  seams.emergency_action = [&]() -> Result<ports::Ok> {
    calls.push_back("emergency");
    return ports::ok();
  };

  const HedgeFirstResult result = execute_hedge_first(seams, alerts);

  CHECK(result.outcome == HedgeFirstOutcome::ShortPlacementFailed);
  CHECK(result.hedge_order_id == "HEDGE-1");  // hedge stands; caller can keep/close
  CHECK(result.short_order_id.empty());
  // A lone hedge is SAFE: no recheck, no emergency, no Critical alert.
  CHECK(count_of(calls, "recheck") == 0);
  CHECK(count_of(calls, "emergency") == 0);
  CHECK_FALSE(result.emergency_action_ran);
  CHECK(alerts.count() == 0);
}

TEST_CASE(
    "AC-3 late hedge fail (recheck false): Critical alert + emergency ran => "
    "NakedShortRemediated") {
  std::vector<std::string> calls;
  SpyAlertSink alerts;

  HedgeFirstSeams seams;
  seams.place_hedge = [&]() -> Result<ports::BrokerAck> {
    calls.push_back("hedge");
    return ok_ack("HEDGE-1");
  };
  seams.confirm_hedge = [&](const std::string&) -> Result<bool> {
    calls.push_back("confirm");
    return true;
  };
  seams.place_short = [&]() -> Result<ports::BrokerAck> {
    calls.push_back("short");
    return ok_ack("SHORT-1");
  };
  seams.recheck_hedge_live = [&](const std::string&) -> Result<bool> {
    calls.push_back("recheck");
    return false;  // hedge vanished/rejected after the short went live
  };
  seams.emergency_action = [&]() -> Result<ports::Ok> {
    calls.push_back("emergency");
    return ports::ok();
  };

  const HedgeFirstResult result = execute_hedge_first(seams, alerts);

  CHECK(result.outcome == HedgeFirstOutcome::NakedShortRemediated);
  CHECK(result.short_order_id == "SHORT-1");
  CHECK(result.emergency_action_ran);
  CHECK(count_of(calls, "emergency") == 1);
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Critical);
}

TEST_CASE("AC-3 recheck Error (can't prove live): SAME remediation path (fail-closed)") {
  std::vector<std::string> calls;
  SpyAlertSink alerts;

  HedgeFirstSeams seams;
  seams.place_hedge = [&]() -> Result<ports::BrokerAck> { return ok_ack("HEDGE-1"); };
  seams.confirm_hedge = [&](const std::string&) -> Result<bool> { return true; };
  seams.place_short = [&]() -> Result<ports::BrokerAck> { return ok_ack("SHORT-1"); };
  seams.recheck_hedge_live = [&](const std::string&) -> Result<bool> {
    calls.push_back("recheck");
    return bool_error();  // cannot prove the hedge is live -> treat as failed
  };
  seams.emergency_action = [&]() -> Result<ports::Ok> {
    calls.push_back("emergency");
    return ports::ok();
  };

  const HedgeFirstResult result = execute_hedge_first(seams, alerts);

  CHECK(result.outcome == HedgeFirstOutcome::NakedShortRemediated);
  CHECK(result.emergency_action_ran);
  CHECK(count_of(calls, "emergency") == 1);
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Critical);
}

TEST_CASE(
    "AC-3 null recheck seam: fail-closed => Critical alert + emergency ran => "
    "NakedShortRemediated") {
  std::vector<std::string> calls;
  SpyAlertSink alerts;

  HedgeFirstSeams seams;
  seams.place_hedge = [&]() -> Result<ports::BrokerAck> { return ok_ack("HEDGE-1"); };
  seams.confirm_hedge = [&](const std::string&) -> Result<bool> { return true; };
  seams.place_short = [&]() -> Result<ports::BrokerAck> { return ok_ack("SHORT-1"); };
  // recheck_hedge_live is null
  seams.emergency_action = [&]() -> Result<ports::Ok> {
    calls.push_back("emergency");
    return ports::ok();
  };

  const HedgeFirstResult result = execute_hedge_first(seams, alerts);

  CHECK(result.outcome == HedgeFirstOutcome::NakedShortRemediated);
  CHECK(result.emergency_action_ran);
  CHECK(count_of(calls, "emergency") == 1);
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Critical);
}

TEST_CASE(
    "AC-3 null emergency seam: still Critical-alerts, emergency_action_ran false, "
    "NakedShortRemediated") {
  SpyAlertSink alerts;

  HedgeFirstSeams seams;
  seams.place_hedge = [&]() -> Result<ports::BrokerAck> { return ok_ack("HEDGE-1"); };
  seams.confirm_hedge = [&](const std::string&) -> Result<bool> { return true; };
  seams.place_short = [&]() -> Result<ports::BrokerAck> { return ok_ack("SHORT-1"); };
  seams.recheck_hedge_live = [&](const std::string&) -> Result<bool> { return false; };
  // emergency_action is null

  const HedgeFirstResult result = execute_hedge_first(seams, alerts);

  CHECK(result.outcome == HedgeFirstOutcome::NakedShortRemediated);
  CHECK_FALSE(result.emergency_action_ran);  // nothing wired, but the operator is told
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Critical);
}

TEST_CASE("AC-3 alert send failure does NOT suppress emergency action") {
  std::vector<std::string> calls;
  SpyAlertSink alerts(/*fail_send=*/true);  // the alert channel is down

  HedgeFirstSeams seams;
  seams.place_hedge = [&]() -> Result<ports::BrokerAck> { return ok_ack("HEDGE-1"); };
  seams.confirm_hedge = [&](const std::string&) -> Result<bool> { return true; };
  seams.place_short = [&]() -> Result<ports::BrokerAck> { return ok_ack("SHORT-1"); };
  seams.recheck_hedge_live = [&](const std::string&) -> Result<bool> { return false; };
  seams.emergency_action = [&]() -> Result<ports::Ok> {
    calls.push_back("emergency");
    return ports::ok();
  };

  const HedgeFirstResult result = execute_hedge_first(seams, alerts);

  CHECK(result.outcome == HedgeFirstOutcome::NakedShortRemediated);
  CHECK(result.emergency_action_ran);  // emergency still ran...
  CHECK(count_of(calls, "emergency") == 1);
  CHECK(alerts.count() == 1);  // ...even though the alert send was attempted
  CHECK(alerts.last_level() == AlertLevel::Critical);
}

TEST_CASE("to_string: stable outcome names") {
  CHECK(broker_exec::options::to_string(HedgeFirstOutcome::HedgedShortLive) == "HedgedShortLive");
  CHECK(broker_exec::options::to_string(HedgeFirstOutcome::HedgePlacementFailed) ==
        "HedgePlacementFailed");
  CHECK(broker_exec::options::to_string(HedgeFirstOutcome::HedgeUnconfirmed) == "HedgeUnconfirmed");
  CHECK(broker_exec::options::to_string(HedgeFirstOutcome::ShortPlacementFailed) ==
        "ShortPlacementFailed");
  CHECK(broker_exec::options::to_string(HedgeFirstOutcome::NakedShortRemediated) ==
        "NakedShortRemediated");
}

// ── Review-added coverage (Story 5.1 adversarial review) ────────────────────

TEST_CASE("confirmed hedge + null place_short seam: fail-closed => ShortPlacementFailed, safe") {
  std::vector<std::string> calls;
  SpyAlertSink alerts;

  HedgeFirstSeams seams;  // place_short is null
  seams.place_hedge = [&]() -> Result<ports::BrokerAck> {
    calls.push_back("hedge");
    return ok_ack("HEDGE-1");
  };
  seams.confirm_hedge = [&](const std::string&) -> Result<bool> {
    calls.push_back("confirm");
    return true;
  };
  seams.emergency_action = [&]() -> Result<ports::Ok> {
    calls.push_back("emergency");
    return ports::ok();
  };

  const HedgeFirstResult result = execute_hedge_first(seams, alerts);

  // A null short seam can't open a short -> a lone hedge is SAFE, never remediated.
  CHECK(result.outcome == HedgeFirstOutcome::ShortPlacementFailed);
  CHECK(result.hedge_order_id == "HEDGE-1");
  CHECK(count_of(calls, "emergency") == 0);
  CHECK(alerts.count() == 0);
}

TEST_CASE(
    "AC-3 emergency action returns Error: ran-but-failed => emergency_action_ran false, still "
    "remediated+alerted") {
  std::vector<std::string> calls;
  SpyAlertSink alerts;

  HedgeFirstSeams seams;
  seams.place_hedge = [&]() -> Result<ports::BrokerAck> { return ok_ack("HEDGE-1"); };
  seams.confirm_hedge = [&](const std::string&) -> Result<bool> { return true; };
  seams.place_short = [&]() -> Result<ports::BrokerAck> { return ok_ack("SHORT-1"); };
  seams.recheck_hedge_live = [&](const std::string&) -> Result<bool> { return false; };
  seams.emergency_action = [&]() -> Result<ports::Ok> {
    calls.push_back("emergency");
    return broker_exec::fail(
        errors::make_error(errors::ErrorCategory::Network, "square-off rejected"));
  };

  const HedgeFirstResult result = execute_hedge_first(seams, alerts);

  CHECK(result.outcome == HedgeFirstOutcome::NakedShortRemediated);
  CHECK(count_of(calls, "emergency") == 1);  // it DID run
  CHECK_FALSE(result.emergency_action_ran);  // ...but did not succeed
  CHECK(alerts.count() == 1);                // operator still alerted
  CHECK(alerts.last_level() == AlertLevel::Critical);
}

namespace {
// An AlertSink that THROWS from send() — models a real comms adapter failing in
// exactly the conditions that trigger AC-3. The emergency square-off must still run.
class ThrowingAlertSink final : public ports::AlertSink {
 public:
  Result<ports::Ok> send(AlertLevel, const std::string&) override {
    throw std::runtime_error("alert transport blew up");
  }
  Result<ports::Ok> send_test_alert() override { return ports::ok(); }
};
}  // namespace

TEST_CASE("AC-3 hardening: a THROWING alert sink never skips the emergency square-off") {
  std::vector<std::string> calls;
  ThrowingAlertSink alerts;

  HedgeFirstSeams seams;
  seams.place_hedge = [&]() -> Result<ports::BrokerAck> { return ok_ack("HEDGE-1"); };
  seams.confirm_hedge = [&](const std::string&) -> Result<bool> { return true; };
  seams.place_short = [&]() -> Result<ports::BrokerAck> { return ok_ack("SHORT-1"); };
  seams.recheck_hedge_live = [&](const std::string&) -> Result<bool> { return false; };
  seams.emergency_action = [&]() -> Result<ports::Ok> {
    calls.push_back("emergency");
    return ports::ok();
  };

  // execute_hedge_first must NOT propagate the sink's exception (no-throw contract)
  // and the square-off must have run BEFORE the (throwing) alert.
  HedgeFirstResult result = execute_hedge_first(seams, alerts);

  CHECK(result.outcome == HedgeFirstOutcome::NakedShortRemediated);
  CHECK(result.emergency_action_ran);
  CHECK(count_of(calls, "emergency") == 1);
}
