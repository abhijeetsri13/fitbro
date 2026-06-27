# Story 2.13: Safe-start cold-boot gate

Status: ready-for-dev

## Story

As an operator,
I want trading blocked until the world is verified fresh,
so that a restart never trades into a stale or unauthenticated state. (FR-22)

## Acceptance Criteria

1. **Given** a fresh boot **When** the bot starts **Then** it verifies session establishment, reconciliation
   completeness, clock sanity, config integrity, fresh instrument master + calendar, egress-IP allowlist, and
   crypto-key presence — trading only after all pass.
2. **And** any failed check blocks trading loudly (fail closed).
3. **And** a forced egress-IP mismatch or stale master blocks the start.

## Tasks / Subtasks

- [ ] Task 1: Add `SafeStartGate` to the EXISTING `session` module (AC: all)
  - [ ] `include/broker_exec/session/safe_start.hpp` + `src/session/safe_start.cpp`; add to `broker_exec_session`. Depends
        inward on `ports` (Ok) + `errors` only — it composes the world via INJECTED `std::function<Result<Ok>()>` checks so
        it stays decoupled and uniformly testable (the composition root binds each to the real module:
        session::validate, reconciler, clock skew/stall, config, refdata::InstrumentMaster::require_fresh,
        TradingCalendar::require_fresh, egress-IP, crypto-keys).
- [ ] Task 2: The all-required, fail-closed gate (AC: 1, 2)
  - [ ] `struct SafeStartContext` with one `std::function<Result<ports::Ok>()>` per check (all REQUIRED):
        `config_check`, `crypto_keys_check`, `clock_check`, `session_check`, `egress_ip_check`,
        `instrument_master_check`, `calendar_check`, `reconciliation_check`.
  - [ ] `class SafeStartGate { Result<ports::Ok> verify(const SafeStartContext&) const; };` — runs the checks in a fixed,
        documented order (foundational/cheap first; reconciliation last): config -> crypto-keys -> clock -> session ->
        egress-IP -> instrument-master -> calendar -> reconciliation.
  - [ ] **Fail-closed default**: unlike the validation gate, an UNSET (empty) check is a HARD FAILURE — "safe-start: <check>
        not configured" (a safety gate must never pass a world it cannot verify). The FIRST failing/empty check returns a
        named Error; the message prefix is `"safe-start: <check> check failed: ..."` (mirrors the validation-gate naming).
        On a wrapped inner Error, preserve its category/action (e.g. a DataStale from require_fresh, a SessionExpired from
        session) while prepending the check name.
  - [ ] All pass -> `ok()` (trading allowed). The gate is the single cold-boot authority; no partial/skip variant.
- [ ] Task 3: Convenience for the session check (AC: 1, 3)
  - [ ] A small helper to turn a `session::SessionState` into a `Result<Ok>` (Healthy -> ok; NeedsReauth/Failed ->
        a SessionExpired/ReEstablishSession Error) so the composition root can wire `KiteSessionEstablisher::validate()`
        as `session_check`. (Reuse `KiteSessionEstablisher::needs_reauth_error()` where apt.)
- [ ] Task 4: CMake — extend `src/session/CMakeLists.txt`
  - [ ] Add `safe_start.cpp` to `broker_exec_session`; add `safe_start_test.cpp` to `broker_exec_session_tests`. The test may
        ALSO link `broker_exec_refdata` + `broker_exec_clock` to prove a REAL stale `InstrumentMaster`/`TradingCalendar`
        blocks the start (AC-3); if so add those to the test link.
- [ ] Task 5: Tests (AC: 1, 2, 3) — `src/session/safe_start_test.cpp`
  - [ ] all eight checks pass -> verify() ok (trading allowed).
  - [ ] each check fails in isolation (others pass) -> a named Error for THAT check; the wrapped inner category is preserved
        (e.g. a DataStale instrument-master failure stays DataStale).
  - [ ] AC-2 fail-closed: an UNSET (empty std::function) check -> verify() blocks with "not configured" naming it.
  - [ ] ordering: set TWO checks to fail (e.g. config AND reconciliation) -> the FIRST in the order (config) is named.
  - [ ] AC-3 with REAL modules: wire `instrument_master_check = [&]{ return im.require_fresh(); }` with a never-refreshed /
        next-day-stale `InstrumentMaster` -> verify() blocks (DataStale); and an `egress_ip_check` returning a mismatch Error
        -> verify() blocks. (Build the real InstrumentMaster via a fake CSV fetcher + TestClock as in Story 2.6.)
  - [ ] session: Healthy session_check -> contributes pass; NeedsReauth/Failed -> blocks (SessionExpired).

## Dev Notes

- **Fail-closed cold boot** — every check REQUIRED; unset/failed blocks loudly. The safe-start gate is the single authority
  that flips "may trade" true only when the whole world is verified. [architecture.md#FR-22 safe_start, #SE-5 crypto keys in gate]
- **Composition (decoupled):** injected `std::function` per check; the composition root binds the real modules
  (session/validate, reconciler [Epic 3], clock skew/stall, config, refdata require_fresh ×2, egress-IP, crypto-keys).
  [2-13 note in loop-state, architecture.md#Safe-start gate]
- **Named, ordered, fail-closed** — mirrors the validation gate (Story 2.8) but with all-required semantics. Preserve a
  wrapped inner Error's category/action. [docs/conventions.md#Errors]
- **Reuse:** `ports::Ok`, `errors::Error`/taxonomy, `session::SessionState`/`KiteSessionEstablisher::needs_reauth_error`,
  (tests) `refdata::InstrumentMaster`/`TradingCalendar` + `clock::TestClock`.

### References
- [Source: epics.md#Story 2.13] [architecture.md#FR-22 safe-start gate, #SE-5]
- [Source: docs/conventions.md] [Source: src/risk/validation_gate.* (ordered-named-check pattern), src/session/kite_session_establisher.*]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
