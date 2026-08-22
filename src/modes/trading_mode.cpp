#include "broker_exec/modes/trading_mode.hpp"

#include <string>

#include "broker_exec/ports/ports_common.hpp"

namespace broker_exec::modes {

namespace {

// The most restrictive policy: all-false, read-only. Used as the fail-CLOSED
// fallback for an unmapped future mode so a missing mapping never permits a live
// send (an unknown mode behaves like a no-orders observer).
[[nodiscard]] ModePolicy locked_down() noexcept {
  ModePolicy p;
  p.read_only = true;
  return p;
}

// Build a policy positionally — keeps policy_for's table dense and readable while
// every field stays explicit (no aggregate-init ordering traps).
[[nodiscard]] ModePolicy make_policy(bool entry, bool exit, bool live, bool read_only,
                                     bool replay) noexcept {
  ModePolicy p;
  p.allows_entry = entry;
  p.allows_exit = exit;
  p.live_execution = live;
  p.read_only = read_only;
  p.replay_clock = replay;
  return p;
}

// A blocked-op Error, redaction-safe: names the mode + op only, no secrets. The
// category captures WHY: NotSupported for an op the mode does not offer at all
// (read-only / unsupported-in-mode); RiskRejected for Emergency refusing a
// non-cancel/square-off order on risk grounds.
[[nodiscard]] errors::Error blocked(errors::ErrorCategory category, TradingMode m, OrderOp op,
                                    std::string reason) {
  errors::Error err =
      errors::make_error(category, "mode " + std::string(to_string(m)) + ": " + reason + " (" +
                                       std::string(to_string(op)) + " not allowed)");
  err.action = errors::SuggestedAction::DoNotRetry;
  return err;
}

}  // namespace

std::string_view to_string(TradingMode m) noexcept {
  switch (m) {
    case TradingMode::Live:
      return "live";
    case TradingMode::Paper:
      return "paper";
    case TradingMode::DryRun:
      return "dry_run";
    case TradingMode::Replay:
      return "replay";
    case TradingMode::MonitorOnly:
      return "monitor_only";
    case TradingMode::ExitOnly:
      return "exit_only";
    case TradingMode::Emergency:
      return "emergency";
  }
  return "unknown";
}

std::string_view to_string(OrderOp op) noexcept {
  switch (op) {
    case OrderOp::Entry:
      return "entry";
    case OrderOp::Exit:
      return "exit";
    case OrderOp::Cancel:
      return "cancel";
    case OrderOp::SquareOff:
      return "square_off";
  }
  return "unknown";
}

ModePolicy policy_for(TradingMode m) noexcept {
  switch (m) {
    //                       entry  exit   live   ro     replay
    case TradingMode::Live:
      return make_policy(true, true, true, false, false);
    case TradingMode::Paper:
      return make_policy(true, true, false, false, false);
    case TradingMode::DryRun:
      // Validates every op (allows_entry/exit true) but live_execution is false:
      // the gate runs, the runtime suppresses the actual send (AC-3).
      return make_policy(true, true, false, false, false);
    case TradingMode::Replay:
      // Read-only replay of a recorded decision-path; Clock bound to the recorded
      // timeline (AC-2).
      return make_policy(false, false, false, true, true);
    case TradingMode::MonitorOnly:
      return make_policy(false, false, false, true, false);
    case TradingMode::ExitOnly:
      return make_policy(false, true, true, false, false);
    case TradingMode::Emergency:
      // Only cancel/square-off reach the broker (enforced in require_op_allowed);
      // allows_exit reflects that risk-removing exits are permitted (AC-3).
      return make_policy(false, true, true, false, false);
  }
  // Unreachable for a valid enumerator (the switch is exhaustive; -Wswitch/WX
  // guards an added one). Fail CLOSED: an unmapped future mode is locked down to
  // a no-orders, read-only observer rather than silently permitting a live send.
  return locked_down();
}

bool can_place_entry(TradingMode m) noexcept {
  return policy_for(m).allows_entry;
}

bool can_place_exit(TradingMode m) noexcept {
  return policy_for(m).allows_exit;
}

bool will_execute_live(TradingMode m) noexcept {
  return policy_for(m).live_execution;
}

bool uses_recorded_clock(TradingMode m) noexcept {
  return policy_for(m).replay_clock;
}

Result<ports::Ok> require_op_allowed(TradingMode m, OrderOp op) {
  const ModePolicy policy = policy_for(m);

  // Read-only modes (Replay, MonitorOnly): no orders at all — EVERY op blocked.
  if (policy.read_only) {
    return fail(blocked(errors::ErrorCategory::NotSupported, m, op, "is read-only"));
  }

  // Emergency: ONLY Cancel and SquareOff get through (AC-3). Entry and a generic
  // Exit are refused on risk grounds.
  if (m == TradingMode::Emergency) {
    if (op == OrderOp::Cancel || op == OrderOp::SquareOff) {
      return ports::ok();
    }
    return fail(
        blocked(errors::ErrorCategory::RiskRejected, m, op, "permits only cancel/square-off"));
  }

  // ExitOnly: risk-reducing exits only — Entry blocked, the rest allowed.
  if (op == OrderOp::Entry && !policy.allows_entry) {
    return fail(blocked(errors::ErrorCategory::NotSupported, m, op, "does not allow entries"));
  }

  // Live/Paper/DryRun: all four ops allowed. For DryRun this is "allowed to be
  // validated"; will_execute_live(DryRun) is false, so the runtime never sends.
  return ports::ok();
}

}  // namespace broker_exec::modes
