#include "broker_exec/modes/trading_mode.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

using broker_exec::errors::ErrorCategory;
using broker_exec::modes::can_place_entry;
using broker_exec::modes::can_place_exit;
using broker_exec::modes::ModePolicy;
using broker_exec::modes::OrderOp;
using broker_exec::modes::policy_for;
using broker_exec::modes::require_op_allowed;
using broker_exec::modes::to_string;
using broker_exec::modes::TradingMode;
using broker_exec::modes::uses_recorded_clock;
using broker_exec::modes::will_execute_live;

namespace {

// All seven modes and all four ops, for table-driven sweeps.
constexpr TradingMode kAllModes[] = {
    TradingMode::Live,        TradingMode::Paper,    TradingMode::DryRun,   TradingMode::Replay,
    TradingMode::MonitorOnly, TradingMode::ExitOnly, TradingMode::Emergency};

constexpr OrderOp kAllOps[] = {OrderOp::Entry, OrderOp::Exit, OrderOp::Cancel, OrderOp::SquareOff};

}  // namespace

TEST_CASE("policy_for yields the exact documented flags for every mode", "[modes]") {
  const ModePolicy live = policy_for(TradingMode::Live);
  CHECK(live.allows_entry);
  CHECK(live.allows_exit);
  CHECK(live.live_execution);
  CHECK_FALSE(live.read_only);
  CHECK_FALSE(live.replay_clock);

  const ModePolicy paper = policy_for(TradingMode::Paper);
  CHECK(paper.allows_entry);
  CHECK(paper.allows_exit);
  CHECK_FALSE(paper.live_execution);
  CHECK_FALSE(paper.read_only);
  CHECK_FALSE(paper.replay_clock);

  const ModePolicy dry = policy_for(TradingMode::DryRun);
  CHECK(dry.allows_entry);
  CHECK(dry.allows_exit);
  CHECK_FALSE(dry.live_execution);
  CHECK_FALSE(dry.read_only);
  CHECK_FALSE(dry.replay_clock);

  const ModePolicy replay = policy_for(TradingMode::Replay);
  CHECK_FALSE(replay.allows_entry);
  CHECK_FALSE(replay.allows_exit);
  CHECK_FALSE(replay.live_execution);
  CHECK(replay.read_only);
  CHECK(replay.replay_clock);

  const ModePolicy monitor = policy_for(TradingMode::MonitorOnly);
  CHECK_FALSE(monitor.allows_entry);
  CHECK_FALSE(monitor.allows_exit);
  CHECK_FALSE(monitor.live_execution);
  CHECK(monitor.read_only);
  CHECK_FALSE(monitor.replay_clock);

  const ModePolicy exit_only = policy_for(TradingMode::ExitOnly);
  CHECK_FALSE(exit_only.allows_entry);
  CHECK(exit_only.allows_exit);
  CHECK(exit_only.live_execution);
  CHECK_FALSE(exit_only.read_only);
  CHECK_FALSE(exit_only.replay_clock);

  const ModePolicy emergency = policy_for(TradingMode::Emergency);
  CHECK_FALSE(emergency.allows_entry);
  CHECK(emergency.allows_exit);
  CHECK(emergency.live_execution);
  CHECK_FALSE(emergency.read_only);
  CHECK_FALSE(emergency.replay_clock);
}

TEST_CASE("derived helpers agree with the policy table", "[modes]") {
  for (const TradingMode m : kAllModes) {
    CHECK(can_place_entry(m) == policy_for(m).allows_entry);
    CHECK(can_place_exit(m) == policy_for(m).allows_exit);
    CHECK(will_execute_live(m) == policy_for(m).live_execution);
    CHECK(uses_recorded_clock(m) == policy_for(m).replay_clock);
  }
}

TEST_CASE("dry-run validates the op but never executes live (AC-3)", "[modes]") {
  CHECK(can_place_entry(TradingMode::DryRun));
  CHECK_FALSE(will_execute_live(TradingMode::DryRun));
  // The op is allowed to be VALIDATED even though it will never be sent.
  CHECK(require_op_allowed(TradingMode::DryRun, OrderOp::Entry).has_value());
  CHECK(require_op_allowed(TradingMode::DryRun, OrderOp::Exit).has_value());
}

TEST_CASE("emergency allows only cancel/square-off (AC-3)", "[modes]") {
  CHECK(require_op_allowed(TradingMode::Emergency, OrderOp::Cancel).has_value());
  CHECK(require_op_allowed(TradingMode::Emergency, OrderOp::SquareOff).has_value());

  const auto entry = require_op_allowed(TradingMode::Emergency, OrderOp::Entry);
  REQUIRE_FALSE(entry.has_value());
  CHECK(entry.error().category == ErrorCategory::RiskRejected);

  const auto exit = require_op_allowed(TradingMode::Emergency, OrderOp::Exit);
  REQUIRE_FALSE(exit.has_value());
  CHECK(exit.error().category == ErrorCategory::RiskRejected);

  CHECK(will_execute_live(TradingMode::Emergency));
}

TEST_CASE("replay binds the recorded clock and is read-only (AC-2)", "[modes]") {
  CHECK(uses_recorded_clock(TradingMode::Replay));
  // Every OTHER mode does not use the recorded clock.
  CHECK_FALSE(uses_recorded_clock(TradingMode::Live));
  CHECK_FALSE(uses_recorded_clock(TradingMode::Paper));
  CHECK_FALSE(uses_recorded_clock(TradingMode::DryRun));
  CHECK_FALSE(uses_recorded_clock(TradingMode::MonitorOnly));
  CHECK_FALSE(uses_recorded_clock(TradingMode::ExitOnly));
  CHECK_FALSE(uses_recorded_clock(TradingMode::Emergency));

  // Read-only: every op blocked.
  for (const OrderOp op : kAllOps) {
    const auto r = require_op_allowed(TradingMode::Replay, op);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().category == ErrorCategory::NotSupported);
  }
}

TEST_CASE("monitor-only blocks every op and is read-only", "[modes]") {
  CHECK(policy_for(TradingMode::MonitorOnly).read_only);
  for (const OrderOp op : kAllOps) {
    const auto r = require_op_allowed(TradingMode::MonitorOnly, op);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().category == ErrorCategory::NotSupported);
  }
}

TEST_CASE("exit-only blocks entry, allows exits, hits the real broker", "[modes]") {
  const auto entry = require_op_allowed(TradingMode::ExitOnly, OrderOp::Entry);
  REQUIRE_FALSE(entry.has_value());
  CHECK(entry.error().category == ErrorCategory::NotSupported);

  CHECK(require_op_allowed(TradingMode::ExitOnly, OrderOp::Exit).has_value());
  CHECK(require_op_allowed(TradingMode::ExitOnly, OrderOp::Cancel).has_value());
  CHECK(require_op_allowed(TradingMode::ExitOnly, OrderOp::SquareOff).has_value());

  CHECK(will_execute_live(TradingMode::ExitOnly));
}

TEST_CASE("live vs paper: both allow all ops, only live hits the real broker", "[modes]") {
  for (const OrderOp op : kAllOps) {
    CHECK(require_op_allowed(TradingMode::Live, op).has_value());
    CHECK(require_op_allowed(TradingMode::Paper, op).has_value());
  }
  CHECK(will_execute_live(TradingMode::Live));
  CHECK_FALSE(will_execute_live(TradingMode::Paper));
}

TEST_CASE("a blocked-op error names the mode and the op", "[modes]") {
  const auto r = require_op_allowed(TradingMode::Emergency, OrderOp::Entry);
  REQUIRE_FALSE(r.has_value());
  const std::string& message = r.error().message;
  CHECK(message.find(std::string(to_string(TradingMode::Emergency))) != std::string::npos);
  CHECK(message.find(std::string(to_string(OrderOp::Entry))) != std::string::npos);
}

TEST_CASE("an unmapped/future mode fails closed (most restrictive)", "[modes]") {
  // The safety net: a TradingMode value outside the enum (a future/garbage mode)
  // must resolve to the most restrictive policy — read-only, no live execution,
  // every op blocked — so a missing mapping can never silently permit a live send.
  const auto bogus = static_cast<TradingMode>(9999);
  const ModePolicy p = policy_for(bogus);
  CHECK(p.read_only);
  CHECK_FALSE(p.allows_entry);
  CHECK_FALSE(p.allows_exit);
  CHECK_FALSE(p.live_execution);
  CHECK_FALSE(p.replay_clock);
  CHECK_FALSE(will_execute_live(bogus));
  for (const OrderOp op : {OrderOp::Entry, OrderOp::Exit, OrderOp::Cancel, OrderOp::SquareOff}) {
    CHECK_FALSE(require_op_allowed(bogus, op).has_value());  // every op blocked
  }
}

TEST_CASE("will_execute_live is true for exactly {Live, ExitOnly, Emergency}", "[modes]") {
  CHECK(will_execute_live(TradingMode::Live));
  CHECK(will_execute_live(TradingMode::ExitOnly));
  CHECK(will_execute_live(TradingMode::Emergency));
  CHECK_FALSE(will_execute_live(TradingMode::Paper));
  CHECK_FALSE(will_execute_live(TradingMode::DryRun));
  CHECK_FALSE(will_execute_live(TradingMode::Replay));
  CHECK_FALSE(will_execute_live(TradingMode::MonitorOnly));
}

TEST_CASE("to_string names are the stable observability contract", "[modes]") {
  CHECK(to_string(TradingMode::Live) == "live");
  CHECK(to_string(TradingMode::Paper) == "paper");
  CHECK(to_string(TradingMode::DryRun) == "dry_run");
  CHECK(to_string(TradingMode::Replay) == "replay");
  CHECK(to_string(TradingMode::MonitorOnly) == "monitor_only");
  CHECK(to_string(TradingMode::ExitOnly) == "exit_only");
  CHECK(to_string(TradingMode::Emergency) == "emergency");
  CHECK(to_string(OrderOp::Entry) == "entry");
  CHECK(to_string(OrderOp::Exit) == "exit");
  CHECK(to_string(OrderOp::Cancel) == "cancel");
  CHECK(to_string(OrderOp::SquareOff) == "square_off");
}
