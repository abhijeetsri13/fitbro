#pragma once

// broker_exec::supervisor — the MULTI-ACCOUNT supervision plan (Story 6.5b,
// FR-33, AC-3).
//
// `SupervisorPolicy` (Story 6.5) answers ONE question: "this process exited with
// code N after M consecutive crashes — what now?". `SupervisorPlan` is the
// registry that makes that answer per-account: it owns the M for each account,
// applies the SAME policy, and reports a fleet-level summary.
//
// ── THE LOAD-BEARING PROPERTY: ISOLATION (AC-3) ─────────────────────────────
// Account A crash-looping all the way into the circuit-breaker MUST NOT change
// what the plan decides for account B. Not B's backoff, not B's crash count, not
// B's alarm. Accounts share a machine, not a fate: an expired token on A is not
// a reason to stop trading B. Every counter here is per-account state in a map
// keyed by account id, and nothing in `on_exit` reads another account's entry —
// the isolation test pins exactly that.
//
// ── WHAT THIS IS NOT ────────────────────────────────────────────────────────
// It spawns NOTHING. No fork, no CreateProcess, no exec, no signals, no sleeps,
// no clock of its own. That is deliberate and it IS the architecture (ID-1): the
// OS does supervision through the systemd template unit
// (deploy/systemd/broker-exec@.service), and this library only holds the DECISION
// the operator's supervisor — or a future in-process launcher — consults. Time is
// INJECTED as an explicit `now` argument (monotonic), so restart scheduling is
// deterministic and testable.
//
// Conventions: no-throw; no float (integer seconds only); no OS API; no
// `#ifdef`; redaction-safe detail strings (reason/action words + integers only).
// Depends on nothing but the C++20 standard library and supervisor_policy.hpp.
// NOT thread-safe: it lives on the supervisor's single loop.

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "broker_exec/supervisor/supervisor_policy.hpp"

namespace broker_exec::supervisor {

// Everything the plan tracks for ONE account. Public so an operator surface (CLI
// / health endpoint) can render it; all of it is redaction-safe.
struct AccountSupervision {
  // Consecutive CRASH exits with no intervening clean run. Drives the backoff
  // schedule and the crash-loop circuit-breaker. Reset by a clean shutdown or by
  // note_healthy(); NOT reset merely by starting (that would defeat the breaker,
  // since a crash loop restarts by definition).
  int consecutive_crashes = 0;

  // The account is not currently running. DEFAULTS TO TRUE: a freshly registered
  // account has not been reported started, and "we have never seen it run" must
  // not read as "it is healthy" — that is exactly how an account that failed to
  // launch at 09:00 stays invisible until someone notices no orders. Only
  // note_started() clears it; on_exit() sets it again.
  bool down = true;

  // A human is needed for THIS account (set by any decision that raises the
  // absence alarm). Sticky until clear_alarm()/note_healthy() — an alarm that
  // clears itself is not an alarm.
  bool absence_alarm = false;

  // The supervisor has stopped restarting this account (fail-closed exit or the
  // crash-loop breaker). Sticky until clear_alarm()/note_healthy().
  bool escalated = false;

  // The last exit observed, for operator display.
  int last_exit_code = 0;
  ExitReason last_reason = ExitReason::CleanShutdown;

  // When a restart may proceed (exit instant + backoff). Only meaningful after a
  // RestartWithBackoff decision; nullopt otherwise.
  std::optional<std::chrono::steady_clock::time_point> restart_due;
};

// Fleet-level rollup for the operator surface / dead-man's-switch.
struct PlanSummary {
  int accounts_total = 0;
  // Registered accounts not currently running. This COUNTS an account that was
  // registered and never started — "never launched" is a down account, not a
  // healthy one.
  int accounts_down = 0;
  int accounts_alarmed = 0;   // accounts with an unacknowledged absence alarm
  int accounts_escalated = 0; // accounts the supervisor has stopped restarting
  bool any_escalated = false; // "at least one account needs a human, now"
};

class SupervisorPlan {
 public:
  explicit SupervisorPlan(BackoffConfig backoff = {});

  // Pre-register an account so it appears in the summary before it ever exits.
  // It is registered as DOWN (see AccountSupervision::down) until note_started()
  // says otherwise. Idempotent; an empty id is ignored (fail-closed: an unnamed
  // account is not silently given a slot). Returns true when the account is now
  // registered.
  bool register_account(const std::string& account_id);

  // The decision for `account_id`'s process exiting with `exit_code` at `now`.
  //
  //   * Maps the code through `exit_reason_from_code` (unknown code ⇒ Crash).
  //   * Updates ONLY this account's state: a Crash increments its consecutive
  //     count, a CleanShutdown resets it, a fail-closed exit leaves it alone.
  //   * Delegates to `decide(...)` with THIS account's count and the shared
  //     BackoffConfig, then records alarm/escalation/restart_due.
  //
  // An unregistered account is registered on first use (a process the supervisor
  // did not know about still gets a decision — never a silent no-op). An EMPTY
  // account id is fail-closed: NoRestartEscalate + absence alarm, and nothing is
  // registered. Total function: no failure mode, nothing to throw.
  SupervisorDecision on_exit(const std::string& account_id, int exit_code,
                             std::chrono::steady_clock::time_point now);

  // The account's process has been (re)started. Clears `down` and the pending
  // restart_due. Does NOT reset the crash counter — see AccountSupervision.
  void note_started(const std::string& account_id, std::chrono::steady_clock::time_point now);

  // The account has been running healthily (long enough that its prior crashes
  // are no longer "consecutive"). Resets the crash counter and clears the alarm
  // and escalation. This is the ONLY automatic recovery path.
  void note_healthy(const std::string& account_id);

  // Operator acknowledgement: clear the alarm + escalation for one account
  // WITHOUT pretending it ran healthily (the crash counter is left intact).
  void clear_alarm(const std::string& account_id);

  // May a restart proceed for this account at `now`? False when the account is
  // unknown, running, escalated, or still inside its backoff window.
  [[nodiscard]] bool may_restart(const std::string& account_id,
                                 std::chrono::steady_clock::time_point now) const;

  // Per-account state; nullopt for an unregistered account.
  [[nodiscard]] std::optional<AccountSupervision> state_of(const std::string& account_id) const;

  // Registered account ids, sorted (the map's order — stable for tests/logs).
  [[nodiscard]] std::vector<std::string> accounts() const;

  [[nodiscard]] PlanSummary summary() const;

  [[nodiscard]] const BackoffConfig& backoff_config() const noexcept { return backoff_; }

 private:
  BackoffConfig backoff_;
  std::map<std::string, AccountSupervision> accounts_;
};

}  // namespace broker_exec::supervisor
