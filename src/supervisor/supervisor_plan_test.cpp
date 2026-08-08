#include "broker_exec/supervisor/supervisor_plan.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

using broker_exec::supervisor::AccountSupervision;
using broker_exec::supervisor::BackoffConfig;
using broker_exec::supervisor::ExitReason;
using broker_exec::supervisor::kExitClean;
using broker_exec::supervisor::kExitFailClosedNeedsHuman;
using broker_exec::supervisor::PlanSummary;
using broker_exec::supervisor::SupervisorAction;
using broker_exec::supervisor::SupervisorDecision;
using broker_exec::supervisor::SupervisorPlan;

namespace {

// Injected monotonic time: a fixed origin plus explicit offsets. No wall clock,
// no sleeping, fully deterministic.
constexpr std::chrono::steady_clock::time_point kT0{};

[[nodiscard]] std::chrono::steady_clock::time_point at(int seconds) {
  return kT0 + std::chrono::seconds{seconds};
}

// base 1s, cap 300s, breaker at 3 consecutive crashes — small enough to drive a
// full crash-loop in a few lines.
constexpr BackoffConfig kCfg{1, 300, 3};

}  // namespace

TEST_CASE("a crash produces a per-account restart-with-backoff and schedules the restart",
          "[supervisor][plan]") {
  SupervisorPlan plan(kCfg);

  const SupervisorDecision d = plan.on_exit("alpha", 1, at(100));
  CHECK(d.action == SupervisorAction::RestartWithBackoff);
  CHECK(d.backoff_seconds == 1);
  CHECK_FALSE(d.raise_absence_alarm);

  const auto state = plan.state_of("alpha");
  REQUIRE(state.has_value());
  CHECK(state->consecutive_crashes == 1);
  CHECK(state->down);
  CHECK_FALSE(state->absence_alarm);
  CHECK_FALSE(state->escalated);
  CHECK(state->last_exit_code == 1);
  CHECK(state->last_reason == ExitReason::Crash);
  REQUIRE(state->restart_due.has_value());
  CHECK(*state->restart_due == at(101));  // exit instant + 1s backoff

  // The restart window is enforced against the INJECTED clock.
  CHECK_FALSE(plan.may_restart("alpha", at(100)));
  CHECK(plan.may_restart("alpha", at(101)));
  CHECK(plan.may_restart("alpha", at(500)));
}

TEST_CASE("ISOLATION: account A crash-looping to escalation never changes B's decision",
          "[supervisor][plan]") {
  SupervisorPlan plan(kCfg);
  plan.register_account("alpha");
  plan.register_account("beta");

  // Drive A all the way through the crash-loop circuit-breaker.
  CHECK(plan.on_exit("alpha", 1, at(0)).action == SupervisorAction::RestartWithBackoff);
  CHECK(plan.on_exit("alpha", 1, at(10)).action == SupervisorAction::RestartWithBackoff);
  CHECK(plan.on_exit("alpha", 139, at(20)).action == SupervisorAction::RestartWithBackoff);
  const SupervisorDecision tripped = plan.on_exit("alpha", 1, at(30));
  REQUIRE(tripped.action == SupervisorAction::NoRestartEscalate);
  CHECK(tripped.raise_absence_alarm);

  const auto a_state = plan.state_of("alpha");
  REQUIRE(a_state.has_value());
  CHECK(a_state->consecutive_crashes == 4);
  CHECK(a_state->escalated);
  CHECK(a_state->absence_alarm);

  // B has been untouched by ALL of that. Its FIRST crash is still its first.
  const auto b_before = plan.state_of("beta");
  REQUIRE(b_before.has_value());
  CHECK(b_before->consecutive_crashes == 0);
  CHECK(b_before->down);  // registered but never started — see the "never started" case
  CHECK_FALSE(b_before->absence_alarm);
  CHECK_FALSE(b_before->escalated);

  const SupervisorDecision b_first = plan.on_exit("beta", 1, at(40));
  CHECK(b_first.action == SupervisorAction::RestartWithBackoff);
  CHECK(b_first.backoff_seconds == kCfg.base_seconds);  // the BASE, not A's escalation
  CHECK_FALSE(b_first.raise_absence_alarm);

  const auto b_after = plan.state_of("beta");
  REQUIRE(b_after.has_value());
  CHECK(b_after->consecutive_crashes == 1);
  CHECK_FALSE(b_after->escalated);
  CHECK_FALSE(b_after->absence_alarm);

  // ...and B's activity does not resurrect A either.
  CHECK(plan.state_of("alpha")->escalated);
  CHECK(plan.state_of("alpha")->consecutive_crashes == 4);
}

TEST_CASE("ISOLATION: a fail-closed exit on one account leaves every other account alone",
          "[supervisor][plan]") {
  SupervisorPlan plan(kCfg);
  plan.register_account("gamma");

  const SupervisorDecision alpha = plan.on_exit("alpha", kExitFailClosedNeedsHuman, at(5));
  CHECK(alpha.action == SupervisorAction::NoRestartEscalate);
  CHECK(alpha.raise_absence_alarm);
  CHECK(alpha.backoff_seconds == 0);

  const auto gamma = plan.state_of("gamma");
  REQUIRE(gamma.has_value());
  CHECK_FALSE(gamma->absence_alarm);
  CHECK_FALSE(gamma->escalated);
  CHECK(gamma->consecutive_crashes == 0);
  CHECK(plan.on_exit("gamma", 1, at(6)).action == SupervisorAction::RestartWithBackoff);
}

TEST_CASE("per-account backoff schedules advance independently", "[supervisor][plan]") {
  SupervisorPlan plan(BackoffConfig{1, 300, 100});

  // A crashes three times; B crashes once. Their backoffs must not share state.
  CHECK(plan.on_exit("alpha", 1, at(0)).backoff_seconds == 1);
  CHECK(plan.on_exit("alpha", 1, at(1)).backoff_seconds == 2);
  CHECK(plan.on_exit("alpha", 1, at(2)).backoff_seconds == 4);
  CHECK(plan.on_exit("beta", 1, at(3)).backoff_seconds == 1);   // B's FIRST crash
  CHECK(plan.on_exit("alpha", 1, at(4)).backoff_seconds == 8);  // A continues its own ladder
  CHECK(plan.on_exit("beta", 1, at(5)).backoff_seconds == 2);
}

TEST_CASE("a clean shutdown resets only that account's crash streak", "[supervisor][plan]") {
  SupervisorPlan plan(BackoffConfig{1, 300, 100});

  CHECK(plan.on_exit("alpha", 1, at(0)).backoff_seconds == 1);
  CHECK(plan.on_exit("alpha", 1, at(1)).backoff_seconds == 2);
  CHECK(plan.on_exit("beta", 1, at(2)).backoff_seconds == 1);

  const SupervisorDecision clean = plan.on_exit("alpha", kExitClean, at(3));
  CHECK(clean.action == SupervisorAction::NoRestartCleanShutdown);
  CHECK_FALSE(clean.raise_absence_alarm);
  CHECK(plan.state_of("alpha")->consecutive_crashes == 0);
  CHECK_FALSE(plan.state_of("alpha")->restart_due.has_value());
  CHECK(plan.state_of("beta")->consecutive_crashes == 1);  // untouched

  // The next crash on A starts the ladder over at the base.
  CHECK(plan.on_exit("alpha", 1, at(4)).backoff_seconds == 1);
}

TEST_CASE("note_started clears down but NOT the crash streak (a loop must still trip)",
          "[supervisor][plan]") {
  SupervisorPlan plan(kCfg);

  plan.on_exit("alpha", 1, at(0));
  CHECK(plan.state_of("alpha")->down);
  plan.note_started("alpha", at(1));
  CHECK_FALSE(plan.state_of("alpha")->down);
  CHECK(plan.state_of("alpha")->consecutive_crashes == 1);  // a restart is not a recovery
  CHECK_FALSE(plan.may_restart("alpha", at(500)));          // it is running

  // Restarting repeatedly still reaches the circuit-breaker.
  plan.on_exit("alpha", 1, at(2));
  plan.note_started("alpha", at(3));
  plan.on_exit("alpha", 1, at(4));
  plan.note_started("alpha", at(5));
  CHECK(plan.on_exit("alpha", 1, at(6)).action == SupervisorAction::NoRestartEscalate);
}

TEST_CASE("note_healthy is the only automatic recovery: it resets the streak and the alarm",
          "[supervisor][plan]") {
  SupervisorPlan plan(kCfg);

  plan.on_exit("alpha", 1, at(0));
  plan.on_exit("alpha", 1, at(1));
  plan.on_exit("alpha", 1, at(2));
  REQUIRE(plan.on_exit("alpha", 1, at(3)).action == SupervisorAction::NoRestartEscalate);
  REQUIRE(plan.state_of("alpha")->absence_alarm);

  plan.note_healthy("alpha");
  const auto state = plan.state_of("alpha");
  REQUIRE(state.has_value());
  CHECK(state->consecutive_crashes == 0);
  CHECK_FALSE(state->absence_alarm);
  CHECK_FALSE(state->escalated);

  // And the next crash is a first crash again.
  CHECK(plan.on_exit("alpha", 1, at(4)).action == SupervisorAction::RestartWithBackoff);
}

TEST_CASE("clear_alarm acknowledges without pretending the account ran healthily",
          "[supervisor][plan]") {
  SupervisorPlan plan(kCfg);

  plan.on_exit("alpha", kExitFailClosedNeedsHuman, at(0));
  REQUIRE(plan.state_of("alpha")->absence_alarm);
  REQUIRE(plan.state_of("alpha")->escalated);

  plan.clear_alarm("alpha");
  CHECK_FALSE(plan.state_of("alpha")->absence_alarm);
  CHECK_FALSE(plan.state_of("alpha")->escalated);
  // The evidence is preserved: acknowledging is not the same as recovering.
  CHECK(plan.state_of("alpha")->last_exit_code == kExitFailClosedNeedsHuman);
  CHECK(plan.state_of("alpha")->last_reason == ExitReason::FailClosedNeedsHuman);
}

TEST_CASE("an escalated account is never eligible for restart", "[supervisor][plan]") {
  SupervisorPlan plan(kCfg);
  plan.on_exit("alpha", kExitFailClosedNeedsHuman, at(0));
  CHECK_FALSE(plan.may_restart("alpha", at(0)));
  CHECK_FALSE(plan.may_restart("alpha", at(100000)));
}

TEST_CASE("may_restart is false for an unknown or cleanly stopped account",
          "[supervisor][plan]") {
  SupervisorPlan plan(kCfg);
  CHECK_FALSE(plan.may_restart("never-seen", at(10)));

  plan.on_exit("alpha", kExitClean, at(0));
  CHECK_FALSE(plan.may_restart("alpha", at(10000)));  // an intended stop stays stopped
}

TEST_CASE("a registered but NEVER STARTED account counts as DOWN, not healthy",
          "[supervisor][plan]") {
  // The failure this pins: an account that never came up at 09:00 must not read
  // as healthy just because it has not exited. "We have never seen it run" is a
  // down account — otherwise a launch failure is invisible until someone notices
  // there are no orders.
  SupervisorPlan plan(kCfg);
  plan.register_account("alpha");

  const auto state = plan.state_of("alpha");
  REQUIRE(state.has_value());
  CHECK(state->down);
  CHECK(state->consecutive_crashes == 0);
  CHECK_FALSE(state->absence_alarm);
  CHECK_FALSE(state->escalated);
  CHECK(plan.summary().accounts_down == 1);

  // ...and it is not a restart candidate either (nothing has been scheduled).
  CHECK_FALSE(plan.may_restart("alpha", at(10000)));

  // Only an actual start clears it.
  plan.note_started("alpha", at(1));
  CHECK_FALSE(plan.state_of("alpha")->down);
  CHECK(plan.summary().accounts_down == 0);
}

TEST_CASE("the plan summary rolls the fleet up for the operator", "[supervisor][plan]") {
  SupervisorPlan plan(kCfg);
  plan.register_account("alpha");
  plan.register_account("beta");
  plan.register_account("gamma");

  // Registered but not yet started: all three are DOWN until they report a start.
  PlanSummary summary = plan.summary();
  CHECK(summary.accounts_total == 3);
  CHECK(summary.accounts_down == 3);
  CHECK(summary.accounts_alarmed == 0);
  CHECK(summary.accounts_escalated == 0);
  CHECK_FALSE(summary.any_escalated);

  // Bring the fleet up.
  plan.note_started("alpha", at(0));
  plan.note_started("beta", at(0));
  plan.note_started("gamma", at(0));
  summary = plan.summary();
  CHECK(summary.accounts_down == 0);

  plan.on_exit("alpha", 1, at(1));                          // down, restarting
  plan.on_exit("beta", kExitFailClosedNeedsHuman, at(2));   // down, escalated + alarmed

  summary = plan.summary();
  CHECK(summary.accounts_total == 3);
  CHECK(summary.accounts_down == 2);
  CHECK(summary.accounts_alarmed == 1);
  CHECK(summary.accounts_escalated == 1);
  CHECK(summary.any_escalated);

  // gamma is untouched throughout — still running, no alarm.
  CHECK_FALSE(plan.state_of("gamma")->down);
  CHECK_FALSE(plan.state_of("gamma")->absence_alarm);

  plan.note_started("alpha", at(3));
  summary = plan.summary();
  CHECK(summary.accounts_down == 1);
  CHECK(summary.any_escalated);  // beta still needs a human

  plan.clear_alarm("beta");
  plan.note_started("beta", at(4));
  summary = plan.summary();
  CHECK(summary.accounts_down == 0);
  CHECK(summary.accounts_alarmed == 0);
  CHECK_FALSE(summary.any_escalated);
}

TEST_CASE("an unregistered account that exits is adopted, never silently dropped",
          "[supervisor][plan]") {
  SupervisorPlan plan(kCfg);
  CHECK_FALSE(plan.state_of("surprise").has_value());

  const SupervisorDecision d = plan.on_exit("surprise", 1, at(0));
  CHECK(d.action == SupervisorAction::RestartWithBackoff);
  REQUIRE(plan.state_of("surprise").has_value());
  CHECK(plan.summary().accounts_total == 1);
}

TEST_CASE("an EMPTY account id fails closed and creates no registry entry",
          "[supervisor][plan]") {
  SupervisorPlan plan(kCfg);

  const SupervisorDecision d = plan.on_exit("", 1, at(0));
  CHECK(d.action == SupervisorAction::NoRestartEscalate);
  CHECK(d.raise_absence_alarm);
  CHECK(d.backoff_seconds == 0);

  CHECK_FALSE(plan.register_account(""));
  CHECK(plan.summary().accounts_total == 0);
  CHECK(plan.accounts().empty());

  // The no-op paths must also not invent an entry.
  plan.note_healthy("");
  plan.clear_alarm("");
  plan.note_started("", at(1));
  CHECK(plan.summary().accounts_total == 0);
}

TEST_CASE("an UNKNOWN exit code is a crash for the account that produced it",
          "[supervisor][plan]") {
  SupervisorPlan plan(kCfg);
  for (const int code : {137, 139, 255, -1}) {
    SupervisorPlan fresh(kCfg);
    const SupervisorDecision d = fresh.on_exit("alpha", code, at(0));
    CHECK(d.action == SupervisorAction::RestartWithBackoff);
    CHECK(fresh.state_of("alpha")->last_reason == ExitReason::Crash);
  }
  CHECK(plan.summary().accounts_total == 0);
}

TEST_CASE("register_account is idempotent and accounts() is sorted", "[supervisor][plan]") {
  SupervisorPlan plan(kCfg);
  CHECK(plan.register_account("zulu"));
  CHECK(plan.register_account("alpha"));
  CHECK(plan.register_account("alpha"));  // idempotent
  CHECK(plan.summary().accounts_total == 2);

  const std::vector<std::string> ids = plan.accounts();
  REQUIRE(ids.size() == 2);
  CHECK(ids[0] == "alpha");
  CHECK(ids[1] == "zulu");

  // Registering does not disturb an existing account's state.
  plan.on_exit("alpha", 1, at(0));
  CHECK(plan.register_account("alpha"));
  CHECK(plan.state_of("alpha")->consecutive_crashes == 1);
}
