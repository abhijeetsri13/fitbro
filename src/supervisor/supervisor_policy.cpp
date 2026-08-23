#include "broker_exec/supervisor/supervisor_policy.hpp"

#include <string>

namespace broker_exec::supervisor {

ExitReason exit_reason_from_code(int exit_code) noexcept {
  // Only the two canonical codes are "known"; EVERYTHING else (negative, 1, 137,
  // 139, signal-derived 128+n, …) is a Crash. Fail-safe: an unrecognized stop is
  // suspicious — restart-with-backoff (bounded by the circuit-breaker) is the
  // safe default, never a silent clean exit.
  if (exit_code == kExitClean) {
    return ExitReason::CleanShutdown;
  }
  if (exit_code == kExitFailClosedNeedsHuman) {
    return ExitReason::FailClosedNeedsHuman;
  }
  return ExitReason::Crash;
}

std::string_view to_string(ExitReason reason) noexcept {
  switch (reason) {
    case ExitReason::CleanShutdown:
      return "clean_shutdown";
    case ExitReason::Crash:
      return "crash";
    case ExitReason::FailClosedNeedsHuman:
      return "fail_closed_needs_human";
  }
  return "unknown";
}

std::string_view to_string(SupervisorAction action) noexcept {
  switch (action) {
    case SupervisorAction::RestartWithBackoff:
      return "restart_with_backoff";
    case SupervisorAction::NoRestartCleanShutdown:
      return "no_restart_clean_shutdown";
    case SupervisorAction::NoRestartEscalate:
      return "no_restart_escalate";
  }
  return "unknown";
}

int backoff_for(int consecutive_crashes, const BackoffConfig& cfg) noexcept {
  // Floor the config so the maths can never go pathological: a non-positive base
  // becomes 1, and the cap is at least the base.
  const int base = cfg.base_seconds > 0 ? cfg.base_seconds : 1;
  const int cap = cfg.max_seconds >= base ? cfg.max_seconds : base;

  // The first crash (or a non-positive count) waits exactly the base.
  if (consecutive_crashes <= 1) {
    return base;
  }

  // OVERFLOW-SAFE capped doubling. We want base * 2^(consecutive_crashes-1) but
  // we must NEVER compute that shift directly: for a large exponent `base << k`
  // overflows `int` (signed-overflow UB). Instead we double in a loop and
  // early-return the cap the instant doubling WOULD reach it. The guard is on
  // `value > cap / 2` BEFORE the multiply: if value exceeds half the ceiling then
  // 2*value would reach/exceed the cap, so we return the cap without doubling.
  // This also makes the `value *= 2` provably overflow-free for ANY cap up to
  // INT_MAX — when we reach the multiply, value <= cap/2 <= INT_MAX/2, so value*2
  // cannot overflow int. (The earlier `value >= cap` form could still overflow
  // when cap itself was near INT_MAX; this form cannot.) We iterate at most
  // ~log2(cap/base) times no matter how huge consecutive_crashes is.
  int value = base;
  for (int i = 1; i < consecutive_crashes; ++i) {
    if (value > cap / 2) {
      return cap;  // doubling would reach/exceed the ceiling (and could overflow) — clamp now
    }
    value *= 2;  // safe: value <= cap/2 <= INT_MAX/2
    if (value >= cap) {
      return cap;
    }
  }
  return value;
}

SupervisorDecision decide(ExitReason reason, int consecutive_crashes, const BackoffConfig& cfg) {
  switch (reason) {
    case ExitReason::CleanShutdown:
      // An intended stop is not a failure: do nothing, no alarm.
      return SupervisorDecision{SupervisorAction::NoRestartCleanShutdown, 0, false,
                                "clean shutdown - intended stop, no restart"};

    case ExitReason::FailClosedNeedsHuman:
      // Terminal for ANY crash count: a restart would just re-hit the fail-closed
      // condition. Never restart; raise the absence alarm so a human steps in.
      return SupervisorDecision{
          SupervisorAction::NoRestartEscalate, 0, true,
          "fail-closed: human required - no auto-restart, absence alarm raised"};

    case ExitReason::Crash:
      // Crash-loop circuit-breaker: once consecutive crashes exceed the cap, stop
      // flapping (which would hammer the broker) and escalate to a human.
      if (consecutive_crashes > cfg.max_consecutive_restarts) {
        return SupervisorDecision{SupervisorAction::NoRestartEscalate, 0, true,
                                  "crash-loop circuit-breaker tripped after " +
                                      std::to_string(consecutive_crashes) +
                                      " consecutive crashes - escalated"};
      }
      // A transient crash: restart after a capped exponential backoff. No alarm —
      // the supervisor is handling it.
      {
        const int backoff = backoff_for(consecutive_crashes, cfg);
        return SupervisorDecision{
            SupervisorAction::RestartWithBackoff, backoff, false,
            "crash - restarting after " + std::to_string(backoff) + "s backoff"};
      }
  }

  // Unreachable for a valid enumerator (the switch is exhaustive; -Wswitch/WX
  // guards an added one). Fail-safe: escalate to a human rather than guess.
  return SupervisorDecision{SupervisorAction::NoRestartEscalate, 0, true,
                            "unknown exit reason - escalated"};
}

}  // namespace broker_exec::supervisor
