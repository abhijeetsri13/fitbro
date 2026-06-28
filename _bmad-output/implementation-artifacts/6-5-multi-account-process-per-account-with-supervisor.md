# Story 6.5: Multi-account process-per-account with supervisor

Status: ready-for-dev

> **SCOPE NOTE (autonomous loop, 2026-06-28):** this pass implements the **supervisor DECISION CORE** — the exit-code
> contract + restart-with-backoff schedule + absence-alarm policy (the AC-3 "supervisor honors the exit-code contract"
> clause) as a pure, unit-testable module. The PROCESS wiring (systemd template, process-per-account spawn, the
> per-(broker,date) shared cache under a cross-process lock, and SIGKILL-restart-from-intent-log) is OS-level orchestration
> deferred to a follow-up. Story stays **in-progress** until that wiring lands; this is the testable safety brain the real
> supervisor process drives.

## Story

As an operator,
I want each account in its own process under a supervisor,
so that a failure in one account cannot touch another. (FR-33)

## Acceptance Criteria (this pass: AC-3 decision core)

3. **And** the supervisor honors the exit-code contract: **CRASH ⇒ restart-with-backoff**; **FAIL_CLOSED_NEEDS_HUMAN ⇒ no
   restart + absence alarm**. (Clean shutdown ⇒ no restart, no alarm — an intended stop.)

(Deferred to the wiring follow-up: AC-1 process-per-account ownership of intent log/store/token/kill-switch + shared
per-(broker,date) cache under a cross-process lock; AC-2 SIGKILL of one process leaves others trading and the supervisor
restarts the dead one from its intent log.)

## Tasks / Subtasks

- [ ] Task 1: New module `src/supervisor` + `include/broker_exec/supervisor` (AC: 3)
  - [ ] Target `broker_exec_supervisor`; pure decision logic. Depends inward on `errors` only (or none). **No new Conan dep.**
        Mirror `src/modes/CMakeLists.txt`. No I/O, no clock, no process APIs (that is the deferred wiring's job).
- [ ] Task 2: Exit-code contract (AC: 3) — `include/broker_exec/supervisor/supervisor_policy.hpp` / `.cpp`
  - [ ] `enum class ExitReason { CleanShutdown, Crash, FailClosedNeedsHuman };` + stable `to_string`.
  - [ ] Canonical exit codes (the contract a supervised process MUST honor — documented constants):
        `kExitClean = 0`, `kExitFailClosedNeedsHuman = 70` (a deliberate "stop, a human is required" code), and ANY OTHER
        non-zero code (including signal-derived 128+n) ⇒ `Crash`.
  - [ ] `[[nodiscard]] ExitReason exit_reason_from_code(int exit_code) noexcept;` — 0 ⇒ CleanShutdown; 70 ⇒
        FailClosedNeedsHuman; everything else ⇒ Crash (fail-safe: an UNKNOWN code is treated as a Crash, never as a clean
        exit — an unrecognized stop is suspicious, restart-with-backoff is the safe default, but see Task 3's crash-loop cap).
- [ ] Task 3: Restart/backoff + absence-alarm policy (AC: 3) — `supervisor_policy.hpp`
  - [ ] `struct BackoffConfig { int base_seconds = 1; int max_seconds = 300; int max_consecutive_restarts = 10; };` (capped
        exponential; the cap on consecutive restarts is the crash-loop circuit-breaker).
  - [ ] `enum class SupervisorAction { RestartWithBackoff, NoRestartCleanShutdown, NoRestartEscalate };` + `to_string`.
  - [ ] `struct SupervisorDecision { SupervisorAction action; int backoff_seconds; bool raise_absence_alarm;`
        `std::string detail; };`
  - [ ] `[[nodiscard]] int backoff_for(int consecutive_crashes, const BackoffConfig&) noexcept;` — capped exponential:
        `min(base * 2^(consecutive_crashes-1), max_seconds)`, clamped ≥ base; `consecutive_crashes <= 1` ⇒ `base_seconds`.
        No overflow (clamp the shift before it explodes).
  - [ ] `[[nodiscard]] SupervisorDecision decide(ExitReason reason, int consecutive_crashes, const BackoffConfig&);` —
        the contract:
        - `CleanShutdown` ⇒ `NoRestartCleanShutdown`, backoff 0, NO alarm (an intended stop is not a failure).
        - `FailClosedNeedsHuman` ⇒ `NoRestartEscalate`, backoff 0, **raise_absence_alarm = true** (auto-restart would just
          re-hit the fail-closed condition; a human is required — alert, do NOT restart).
        - `Crash` ⇒ if `consecutive_crashes > max_consecutive_restarts` ⇒ `NoRestartEscalate`, **raise_absence_alarm = true**
          (CRASH-LOOP circuit-breaker: stop flapping, escalate to a human). Otherwise ⇒ `RestartWithBackoff`,
          `backoff_seconds = backoff_for(consecutive_crashes, cfg)`, NO alarm.
  - [ ] `detail` redaction-safe (reason/action names + the backoff integer only; no secrets).
- [ ] Task 4: CMake — `src/supervisor/CMakeLists.txt` (mirror modes); wire `add_subdirectory(src/supervisor)` into root
      CMakeLists after `src/isolation`. `broker_exec_supervisor_tests` (Catch2). No new dep.
- [ ] Task 5: Tests — `src/supervisor/supervisor_policy_test.cpp`
  - [ ] exit_reason_from_code: 0⇒CleanShutdown; 70⇒FailClosedNeedsHuman; 1, 139, 137 (128+SIGKILL), -1 ⇒ Crash
        (unknown ⇒ Crash, fail-safe).
  - [ ] AC-3 crash restart-with-backoff: `decide(Crash, 1)` ⇒ RestartWithBackoff, backoff==base, no alarm;
        `decide(Crash, 3)` ⇒ backoff==base*4 (capped at max); a large consecutive count ⇒ backoff==max_seconds (cap holds).
  - [ ] AC-3 fail-closed: `decide(FailClosedNeedsHuman, n)` ⇒ NoRestartEscalate, raise_absence_alarm==true, backoff 0 — for
        any n (NEVER restarts).
  - [ ] clean shutdown: `decide(CleanShutdown, 0)` ⇒ NoRestartCleanShutdown, no alarm, backoff 0.
  - [ ] crash-loop circuit-breaker: `decide(Crash, max_consecutive_restarts + 1)` ⇒ NoRestartEscalate +
        raise_absence_alarm==true (stop flapping).
  - [ ] backoff_for monotonic + capped + no overflow: a huge consecutive_crashes (e.g. 1000) returns exactly max_seconds,
        does not overflow/UB; consecutive_crashes 0 or 1 ⇒ base_seconds.
  - [ ] to_string stability for ExitReason + SupervisorAction.

## Dev Notes

- **Exit-code contract is the supervisor's brain** (FR-33, AC-3): a supervised account process signals its fate through its
  exit code, and the supervisor maps that to restart / no-restart-escalate. A CRASH is transient ⇒ restart with capped
  exponential backoff; a FAIL_CLOSED_NEEDS_HUMAN exit is terminal ⇒ never auto-restart (it would re-hit the same wall),
  raise an absence alarm so a human steps in; a clean shutdown is an intended stop. [architecture.md#FR-33 supervisor]
- **Crash-loop circuit-breaker** — unbounded restart-on-crash is its own failure mode (a flapping process hammering the
  broker); after `max_consecutive_restarts` consecutive crashes, stop and escalate. Fail-safe, not in the bare AC text but
  required to make "restart-with-backoff" safe.
- **Unknown exit code ⇒ Crash** (fail-safe): an unrecognized code is suspicious; treat as a crash (restart-with-backoff,
  then the circuit-breaker bounds it) rather than a clean exit.
- **Pure, no-throw, no float, redaction-safe.** Integer backoff seconds. The process spawn / systemd / cross-process cache
  lock / restart-from-intent-log is the DEFERRED wiring; this module is the decision core it consults.

### References
- [Source: epics.md#Story 6.5, FR-33] [architecture.md#FR-33 process-per-account + supervisor exit-code contract]
- [Source: src/modes/* (sibling pure-decision module style)] [docs/conventions.md]

## Dev Agent Record
### Agent Model Used
Opus 4.8 (1M context) — claude-opus-4-8[1m]

### Completion Notes List
- Implemented the AC-3 **supervisor decision core ONLY** (Tasks 1-5). The OS-level
  wiring (systemd template, process-per-account spawn, per-(broker,date) shared
  cache under a cross-process lock, SIGKILL-restart-from-intent-log) remains the
  DEFERRED follow-up per the scope note; story stays in-progress until that lands.
- New module `broker_exec::supervisor` is fully standalone: it depends on nothing
  but the C++20 standard library (no domain/ports/errors, no Result/Error, no new
  Conan dep). It returns plain value structs, never throws, uses integer seconds
  only (no float), and contains no OS API / `#ifdef`.
- **Exit-code contract:** `kExitClean = 0` ⇒ CleanShutdown; `kExitFailClosedNeedsHuman
  = 70` ⇒ FailClosedNeedsHuman; every other code (negative, 1, 137, 139, 128+n, …)
  ⇒ Crash (fail-safe: unknown is never clean).
- **Decision contract** matches the spec exactly: CleanShutdown ⇒
  NoRestartCleanShutdown (no alarm); FailClosedNeedsHuman ⇒ NoRestartEscalate +
  alarm for ANY crash count; Crash ⇒ RestartWithBackoff (no alarm) unless
  `consecutive_crashes > max_consecutive_restarts`, where the crash-loop
  circuit-breaker trips to NoRestartEscalate + alarm. `detail` strings are
  redaction-safe (reason/action words + integers only).
- **backoff_for overflow-safety:** the shift `base << (n-1)` is NEVER computed
  directly (it would be signed-overflow UB for large n). Instead the function
  doubles in a loop and early-returns the cap the instant the running value
  reaches/exceeds `max_seconds`. Because it stops AT the cap, `value` is always
  ≤ cap before each doubling, so `value * 2` can at most reach 2*cap and never
  overflows `int`, and it iterates at most ~log2(cap/base) times regardless of how
  huge `consecutive_crashes` is. A non-positive base is floored to 1 and
  `max_seconds` is floored to base, so a degenerate config stays safe.
- Tests cover every Task-5 bullet with asserted integer backoff values, including
  `backoff_for(1000)` / `backoff_for(1000000)` returning exactly the cap with no
  overflow, monotonicity up to and past the cap, the circuit-breaker at
  `max_consecutive_restarts + 1`, fail-closed for several n, and to_string
  stability for both enums.
- NOT built locally (orchestrator builds). One orchestrator edit only:
  `add_subdirectory(src/supervisor)` added after `src/isolation` in root
  CMakeLists.txt.

### File List
- include/broker_exec/supervisor/supervisor_policy.hpp (new)
- src/supervisor/supervisor_policy.cpp (new)
- src/supervisor/supervisor_policy_test.cpp (new)
- src/supervisor/CMakeLists.txt (new)
- CMakeLists.txt (edited — added `add_subdirectory(src/supervisor)` after src/isolation)
