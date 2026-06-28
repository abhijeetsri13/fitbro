#pragma once

// broker_exec::supervisor — the supervisor DECISION CORE (Story 6.5, FR-33,
// AC-3).
//
// A supervised account process signals its FATE through its exit code, and the
// supervisor maps that code to one decision: restart-with-backoff, do nothing
// (an intended stop), or stop and escalate to a human. This module is that
// mapping — a PURE, no-I/O, no-clock, no-process-API decision brain the real
// supervisor process consults. The OS-level wiring (systemd template,
// process-per-account spawn, the per-(broker,date) shared cache under a
// cross-process lock, SIGKILL-restart-from-intent-log) is the DEFERRED follow-up;
// none of it lives here.
//
// The three load-bearing rules (AC-3):
//   * CRASH (a transient failure) ⇒ RESTART with capped exponential backoff —
//     bounded by the crash-loop circuit-breaker so a flapping process can never
//     hammer the broker forever.
//   * FAIL_CLOSED_NEEDS_HUMAN (a terminal stop) ⇒ NEVER auto-restart (a restart
//     would just re-hit the same wall) and RAISE an absence alarm so a human
//     steps in.
//   * CLEAN SHUTDOWN (an intended stop) ⇒ no restart, no alarm — not a failure.
//
// Two fail-safes worth stating loudly:
//   * UNKNOWN EXIT CODE ⇒ CRASH: an unrecognized code is suspicious; it is
//     treated as a crash (restart-with-backoff, then the circuit-breaker bounds
//     it), NEVER as a clean exit. An unrecognized stop is never assumed benign.
//   * CRASH-LOOP CIRCUIT-BREAKER: after `max_consecutive_restarts` consecutive
//     crashes, stop flapping and escalate (alarm) instead of restarting again.
//
// Conventions: no-throw, NO double/float (integer seconds only), no OS API / no
// `#ifdef`, redaction-safe `detail` (reason/action words + integers only, never
// secrets). Depends on nothing but the C++20 standard library.

#include <string>
#include <string_view>

namespace broker_exec::supervisor {

// How a supervised account process ended, as derived from its exit code. The
// underlying values carry no ordering meaning; the supervisor distinguishes them
// by policy. Renaming a returned `to_string` name is a breaking observability
// change (NFR-8).
enum class ExitReason {
  CleanShutdown,        // intended stop — no restart, no alarm
  Crash,                // transient/unknown failure — restart-with-backoff
  FailClosedNeedsHuman  // terminal stop — no restart, raise absence alarm
};

// ── The exit-code contract (a supervised process MUST honor these) ──────────
// Canonical, documented exit codes. Anything NOT listed here is a Crash
// (fail-safe: an unknown code is suspicious, never clean).
//   kExitClean                — a deliberate, intended clean stop.
//   kExitFailClosedNeedsHuman — a deliberate "stop, a human is required" code
//                               (70 == EX_SOFTWARE in sysexits.h, reused here as
//                               the agreed fail-closed signal).
inline constexpr int kExitClean = 0;
inline constexpr int kExitFailClosedNeedsHuman = 70;

// Map a raw process exit code to an ExitReason (the contract):
//   0  ⇒ CleanShutdown
//   70 ⇒ FailClosedNeedsHuman
//   ANY OTHER code (negative, 1, 137, 139, signal-derived 128+n, …) ⇒ Crash.
// Fail-safe: an unrecognized code is treated as a Crash (restart-with-backoff,
// bounded by the circuit-breaker), never as a clean exit.
[[nodiscard]] ExitReason exit_reason_from_code(int exit_code) noexcept;

// Capped-exponential restart-backoff schedule plus the crash-loop circuit-breaker
// bound. All integer seconds (no float).
//   base_seconds            — the first/minimum backoff (the floor).
//   max_seconds             — the backoff cap (the ceiling).
//   max_consecutive_restarts— the crash-loop circuit-breaker: after this many
//                             consecutive crashes, stop restarting and escalate.
struct BackoffConfig {
  int base_seconds = 1;
  int max_seconds = 300;
  int max_consecutive_restarts = 10;
};

// The supervisor's possible decisions.
enum class SupervisorAction {
  RestartWithBackoff,      // transient crash — restart after a backoff delay
  NoRestartCleanShutdown,  // intended stop — do nothing
  NoRestartEscalate        // terminal/looping — stop and alert a human
};

// The full decision the supervisor acts on.
//   action             — what to do.
//   backoff_seconds    — how long to wait before restart (0 when not restarting).
//   raise_absence_alarm— alert a human that this account is now absent.
//   detail             — a redaction-safe, human-readable reason (reason/action
//                        words + integers only; NEVER any secret).
struct SupervisorDecision {
  SupervisorAction action;
  int backoff_seconds;
  bool raise_absence_alarm;
  std::string detail;
};

// Stable, log/serialization-friendly names (NFR-8 observability contract).
[[nodiscard]] std::string_view to_string(ExitReason reason) noexcept;
[[nodiscard]] std::string_view to_string(SupervisorAction action) noexcept;

// Capped exponential backoff for the Nth consecutive crash:
//   min(base * 2^(consecutive_crashes-1), max_seconds), clamped ≥ base_seconds.
//   consecutive_crashes <= 1 ⇒ base_seconds.
// OVERFLOW-SAFE: the shift is NEVER performed past the cap. The implementation
// multiplies by 2 in a loop and early-returns max_seconds the instant the
// running value reaches/exceeds the cap, so `base << shift` can never overflow
// `int` (no UB) however large `consecutive_crashes` is. A non-positive base is
// floored to 1; max_seconds is floored to base_seconds.
[[nodiscard]] int backoff_for(int consecutive_crashes, const BackoffConfig& cfg) noexcept;

// The supervisor's exit-code → decision contract (AC-3):
//   * CleanShutdown        ⇒ NoRestartCleanShutdown, backoff 0, no alarm.
//   * FailClosedNeedsHuman ⇒ NoRestartEscalate, backoff 0, raise_absence_alarm =
//                            true — for ANY consecutive_crashes (a restart would
//                            re-hit the fail-closed wall; a human is required).
//   * Crash ⇒ if consecutive_crashes > cfg.max_consecutive_restarts ⇒
//             NoRestartEscalate, backoff 0, raise_absence_alarm = true (crash-loop
//             circuit-breaker: stop flapping, escalate). OTHERWISE ⇒
//             RestartWithBackoff, backoff_for(consecutive_crashes, cfg), no alarm.
// No throw. `detail` is redaction-safe.
[[nodiscard]] SupervisorDecision decide(ExitReason reason, int consecutive_crashes,
                                        const BackoffConfig& cfg);

}  // namespace broker_exec::supervisor
