#include "broker_exec/supervisor/supervisor_policy.hpp"

#include <catch2/catch_test_macros.hpp>

#include <climits>
#include <string>

using broker_exec::supervisor::backoff_for;
using broker_exec::supervisor::BackoffConfig;
using broker_exec::supervisor::decide;
using broker_exec::supervisor::exit_reason_from_code;
using broker_exec::supervisor::ExitReason;
using broker_exec::supervisor::kExitClean;
using broker_exec::supervisor::kExitFailClosedNeedsHuman;
using broker_exec::supervisor::SupervisorAction;
using broker_exec::supervisor::SupervisorDecision;
using broker_exec::supervisor::to_string;

namespace {

// The default contract: base 1s, cap 300s, circuit-breaker at 10 restarts.
constexpr BackoffConfig kDefaultCfg{};

}  // namespace

TEST_CASE("exit_reason_from_code maps the canonical codes and fails safe on the rest",
          "[supervisor]") {
  // The two KNOWN codes.
  CHECK(exit_reason_from_code(kExitClean) == ExitReason::CleanShutdown);
  CHECK(exit_reason_from_code(0) == ExitReason::CleanShutdown);
  CHECK(exit_reason_from_code(kExitFailClosedNeedsHuman) == ExitReason::FailClosedNeedsHuman);
  CHECK(exit_reason_from_code(70) == ExitReason::FailClosedNeedsHuman);

  // Everything else ⇒ Crash (fail-safe: unknown is never clean).
  CHECK(exit_reason_from_code(1) == ExitReason::Crash);     // generic failure
  CHECK(exit_reason_from_code(139) == ExitReason::Crash);   // 128 + SIGSEGV
  CHECK(exit_reason_from_code(137) == ExitReason::Crash);   // 128 + SIGKILL
  CHECK(exit_reason_from_code(-1) == ExitReason::Crash);    // negative
  CHECK(exit_reason_from_code(255) == ExitReason::Crash);   // arbitrary nonzero
}

TEST_CASE("crash restart-with-backoff: capped exponential schedule (AC-3)", "[supervisor]") {
  // First crash ⇒ restart after the base backoff, no alarm.
  const SupervisorDecision d1 = decide(ExitReason::Crash, 1, kDefaultCfg);
  CHECK(d1.action == SupervisorAction::RestartWithBackoff);
  CHECK(d1.backoff_seconds == kDefaultCfg.base_seconds);  // 1
  CHECK(d1.backoff_seconds == 1);
  CHECK_FALSE(d1.raise_absence_alarm);

  // Third crash ⇒ base * 2^2 == 4, still under the cap.
  const SupervisorDecision d3 = decide(ExitReason::Crash, 3, kDefaultCfg);
  CHECK(d3.action == SupervisorAction::RestartWithBackoff);
  CHECK(d3.backoff_seconds == 4);
  CHECK_FALSE(d3.raise_absence_alarm);
}

TEST_CASE("crash with a large count under a high breaker hits the backoff cap exactly",
          "[supervisor]") {
  // A cfg whose circuit-breaker tolerates many restarts so we can exercise the
  // PURE cap path of the backoff schedule (rather than the breaker).
  const BackoffConfig high_breaker{1, 300, 100000};
  const SupervisorDecision d = decide(ExitReason::Crash, 1000, high_breaker);
  CHECK(d.action == SupervisorAction::RestartWithBackoff);
  CHECK(d.backoff_seconds == 300);  // capped at max_seconds, no overflow
  CHECK_FALSE(d.raise_absence_alarm);
}

TEST_CASE("fail-closed never restarts and always raises the absence alarm (AC-3)",
          "[supervisor]") {
  for (const int n : {0, 1, 2, 5, 50, 1000}) {
    const SupervisorDecision d = decide(ExitReason::FailClosedNeedsHuman, n, kDefaultCfg);
    CHECK(d.action == SupervisorAction::NoRestartEscalate);
    CHECK(d.raise_absence_alarm);
    CHECK(d.backoff_seconds == 0);  // never restarts
  }
}

TEST_CASE("clean shutdown does nothing and raises no alarm", "[supervisor]") {
  const SupervisorDecision d = decide(ExitReason::CleanShutdown, 0, kDefaultCfg);
  CHECK(d.action == SupervisorAction::NoRestartCleanShutdown);
  CHECK_FALSE(d.raise_absence_alarm);
  CHECK(d.backoff_seconds == 0);
}

TEST_CASE("crash-loop circuit-breaker trips and escalates (AC-3)", "[supervisor]") {
  // Exactly at the cap is still a restart...
  const SupervisorDecision at_cap =
      decide(ExitReason::Crash, kDefaultCfg.max_consecutive_restarts, kDefaultCfg);
  CHECK(at_cap.action == SupervisorAction::RestartWithBackoff);
  CHECK_FALSE(at_cap.raise_absence_alarm);

  // ...one past the cap trips the breaker: stop flapping, escalate + alarm.
  const SupervisorDecision tripped =
      decide(ExitReason::Crash, kDefaultCfg.max_consecutive_restarts + 1, kDefaultCfg);
  CHECK(tripped.action == SupervisorAction::NoRestartEscalate);
  CHECK(tripped.raise_absence_alarm);
  CHECK(tripped.backoff_seconds == 0);

  // A very large count is also the breaker, not a restart.
  const SupervisorDecision big = decide(ExitReason::Crash, 100, kDefaultCfg);
  CHECK(big.action == SupervisorAction::NoRestartEscalate);
  CHECK(big.raise_absence_alarm);
}

TEST_CASE("backoff_for: base floor, capped, monotonic, overflow-safe", "[supervisor]") {
  const BackoffConfig cfg{1, 300, 10};

  // 0 or 1 consecutive crashes ⇒ the base (the floor).
  CHECK(backoff_for(0, cfg) == cfg.base_seconds);
  CHECK(backoff_for(1, cfg) == cfg.base_seconds);
  CHECK(backoff_for(1, cfg) == 1);

  // Exact doubling values up to the cap: 1, 2, 4, 8, 16, 32, 64, 128, 256, 300.
  CHECK(backoff_for(2, cfg) == 2);
  CHECK(backoff_for(3, cfg) == 4);
  CHECK(backoff_for(4, cfg) == 8);
  CHECK(backoff_for(5, cfg) == 16);
  CHECK(backoff_for(9, cfg) == 256);
  CHECK(backoff_for(10, cfg) == 300);  // 512 would exceed the cap ⇒ clamped

  // A HUGE count returns exactly the cap with no overflow / UB on the shift.
  CHECK(backoff_for(1000, cfg) == 300);
  CHECK(backoff_for(1000000, cfg) == 300);

  // Monotonic non-decreasing, all the way up to and past the cap.
  int previous = 0;
  for (int n = 0; n <= 64; ++n) {
    const int b = backoff_for(n, cfg);
    CHECK(b >= previous);
    CHECK(b >= cfg.base_seconds);
    CHECK(b <= cfg.max_seconds);
    previous = b;
  }
}

TEST_CASE("backoff_for honors a non-default base and cap", "[supervisor]") {
  const BackoffConfig cfg{5, 100, 10};
  CHECK(backoff_for(1, cfg) == 5);    // base
  CHECK(backoff_for(2, cfg) == 10);   // 5 * 2
  CHECK(backoff_for(3, cfg) == 20);   // 5 * 4
  CHECK(backoff_for(4, cfg) == 40);   // 5 * 8
  CHECK(backoff_for(5, cfg) == 80);   // 5 * 16
  CHECK(backoff_for(6, cfg) == 100);  // 160 exceeds cap ⇒ clamped
  CHECK(backoff_for(100, cfg) == 100);
}

TEST_CASE("backoff_for floors a degenerate config to stay safe", "[supervisor]") {
  // Non-positive base is floored to 1; a cap below base is floored to base.
  const BackoffConfig degenerate{0, -5, 10};
  CHECK(backoff_for(1, degenerate) == 1);
  CHECK(backoff_for(5, degenerate) == 1);  // cap floored to base (1)
  CHECK(backoff_for(1000, degenerate) == 1);
}

TEST_CASE("decide detail strings are redaction-safe and informative", "[supervisor]") {
  const SupervisorDecision crash = decide(ExitReason::Crash, 1, kDefaultCfg);
  CHECK(crash.detail.find("restarting") != std::string::npos);

  const SupervisorDecision clean = decide(ExitReason::CleanShutdown, 0, kDefaultCfg);
  CHECK(clean.detail.find("clean shutdown") != std::string::npos);

  const SupervisorDecision failclosed = decide(ExitReason::FailClosedNeedsHuman, 1, kDefaultCfg);
  CHECK(failclosed.detail.find("human required") != std::string::npos);

  const SupervisorDecision tripped = decide(ExitReason::Crash, 100, kDefaultCfg);
  CHECK(tripped.detail.find("circuit-breaker") != std::string::npos);
}

TEST_CASE("backoff_for is overflow-safe with a near-INT_MAX cap (no UB, bounded positive)",
          "[supervisor]") {
  // A pathological operator cap near INT_MAX must NOT overflow the doubling into a
  // negative / instant-restart backoff. The result must stay positive and == cap.
  const BackoffConfig huge_cap{1, INT_MAX, 1000000};
  const int b = backoff_for(40, huge_cap);  // 2^39 would overflow int if shifted
  CHECK(b > 0);
  CHECK(b == INT_MAX);
  // A moderate cap below INT_MAX/2 still doubles correctly.
  CHECK(backoff_for(40, BackoffConfig{1, 1000, 1000000}) == 1000);
}

TEST_CASE("degenerate circuit-breaker (max_consecutive_restarts <= 0) fails safe to escalate",
          "[supervisor]") {
  const BackoffConfig no_restarts{1, 300, 0};
  const SupervisorDecision d = decide(ExitReason::Crash, 1, no_restarts);
  // 1 > 0 => the breaker trips immediately rather than infinite-restarting.
  CHECK(d.action == SupervisorAction::NoRestartEscalate);
  CHECK(d.raise_absence_alarm);
}

TEST_CASE("to_string names are the stable observability contract", "[supervisor]") {
  CHECK(to_string(ExitReason::CleanShutdown) == "clean_shutdown");
  CHECK(to_string(ExitReason::Crash) == "crash");
  CHECK(to_string(ExitReason::FailClosedNeedsHuman) == "fail_closed_needs_human");

  CHECK(to_string(SupervisorAction::RestartWithBackoff) == "restart_with_backoff");
  CHECK(to_string(SupervisorAction::NoRestartCleanShutdown) == "no_restart_clean_shutdown");
  CHECK(to_string(SupervisorAction::NoRestartEscalate) == "no_restart_escalate");
}
