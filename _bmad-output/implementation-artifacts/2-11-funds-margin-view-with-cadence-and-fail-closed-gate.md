# Story 2.11: Funds/margin view with cadence and fail-closed gate

Status: ready-for-dev

## Story

As an operator,
I want margin checked against a fresh funds view,
so that an over-leveraged order is never let through on stale data. (FR-13)

## Acceptance Criteria

1. **Given** a funds view with a `fetched_at` stamp **When** a margin-sensitive order is gated **Then** the view is
   refreshed if older than the configured cadence; on unrefreshable-stale the check fails closed (block + alert).
2. **And** the scheduler refreshes funds on cadence and after fills.
3. **And** the check never runs against a view older than the cadence.

## Tasks / Subtasks

- [ ] Task 1: Add `FundsView` to the EXISTING `risk` module (AC: all)
  - [ ] `include/broker_exec/risk/funds_view.hpp` + `src/risk/funds_view.cpp`; add to `broker_exec_risk`. Reuse
        `ports::FundsSnapshot` (available_margin / used_margin) and `ports::ClockPort`; no new deps.
- [ ] Task 2: The cadence'd view (AC: 1, 3)
  - [ ] Ctor: `(const ports::ClockPort& clock, std::function<Result<ports::FundsSnapshot>()> fetch, std::chrono::seconds cadence)`.
  - [ ] Holds the last `FundsSnapshot` + `fetched_at_` (a steady-clock stamp from `clock.now_steady()`), a `has_snapshot_`
        flag, and the cadence.
  - [ ] `Result<ports::Ok> refresh()`: call fetch(); on success store snapshot + `fetched_at_ = clock.now_steady()`,
        set has_snapshot_; on failure return the Error and DO NOT update the stamp (the view stays stale — never advance
        freshness on a failed fetch).
  - [ ] `bool is_fresh() const`: `has_snapshot_ && (now_steady - fetched_at_) <= cadence` (age strictly within cadence).
  - [ ] `void invalidate()`: force the next access to refetch (after a fill — AC-2). (e.g. clear has_snapshot_ or mark dirty.)
  - [ ] `Result<ports::FundsSnapshot> ensure_fresh()`: if `!is_fresh()` call refresh(); if STILL not fresh -> a
        `DataStale` Error (SuggestedAction::BlockStrategy) — fail closed; else return the snapshot. **The snapshot value is
        NEVER returned while stale** (AC-3).
- [ ] Task 3: Margin check + gate adapter (AC: 1)
  - [ ] `Result<ports::Ok> check_margin(std::int64_t required_margin_paise)`: `ensure_fresh()` first (auto-refresh-if-stale,
        fail-closed if unrefreshable); then ok() iff `available_margin >= required_margin_paise`, else an
        `InsufficientFunds` Error (do-not-retry) naming the shortfall. A required of 0 still requires a FRESH view (the gate
        must never pass margin on stale data even for a zero requirement — fail-closed on stale dominates).
  - [ ] `std::function<Result<ports::Ok>()> make_funds_check(std::int64_t required_margin_paise)`: binds check_margin so it
        drops into `GateContext::funds_check` (Story 2.8). Captures the view by reference (document the lifetime: the view
        outlives the gate call within the main loop) OR by a pointer; intent is a per-call closure used inside one gate pass.
- [ ] Task 4: CMake — extend `src/risk/CMakeLists.txt`
  - [ ] Add `funds_view.cpp` to `broker_exec_risk`; add `funds_view_test.cpp` to `broker_exec_risk_tests` (clock already linked).
        Add `broker_exec_ports` to the test link if needed for FundsSnapshot (already linked transitively via risk).
- [ ] Task 5: Tests (AC: 1, 2, 3) — `src/risk/funds_view_test.cpp` (TestClock for deterministic time)
  - [ ] fresh: refresh() then check_margin(required <= available) -> ok; required > available -> InsufficientFunds.
  - [ ] cadence refresh: a fetch counter; advance the TestClock past the cadence -> the next check_margin triggers exactly
        ONE refresh (fetch count increments); within cadence -> no extra fetch.
  - [ ] fail-closed on unrefreshable-stale (AC-1, AC-3 CRUX): first fetch returns a healthy snapshot (available high); advance
        past cadence; make the fetch now return an Error -> check_margin returns DataStale (NOT the old healthy available),
        proving the stale snapshot is never used. The order is blocked.
  - [ ] never-stale (AC-3): after advancing past cadence with a failing fetch, even check_margin(0) fails closed.
  - [ ] after-fill (AC-2): invalidate() within the cadence window forces a refetch on the next check (fetch count increments).
  - [ ] gate adapter: make_funds_check(req)() returns the same verdict as check_margin(req), including the stale->DataStale path.

## Dev Notes

- **fetched_at via ClockPort.now_steady()** (monotonic; cadence is an elapsed-duration test, immune to wall jumps).
  [FR-13, docs/conventions.md — steady for durations]
- **Fail-closed dominates**: a stale, unrefreshable view ALWAYS blocks a margin check — the old value is never reused, even
  for a zero requirement. [architecture.md#FR-13 funds_view fail-closed]
- **Gate integration**: make_funds_check is the gate's injected `funds_check` (Story 2.8). [2-8 loop-state follow-up: runtime
  must wire funds_check for entries]
- **No float**: margin compared in integer paise (Money/ FundsSnapshot). [docs/conventions.md#Money]
- **Errors**: DataStale + BlockStrategy on stale; InsufficientFunds + DoNotRetry on shortfall. Reuse taxonomy.
- **Reuse:** `ports::FundsSnapshot`/`ClockPort`, `domain::Money`, `errors`, `clock::TestClock` (tests).

### References
- [Source: epics.md#Story 2.11] [architecture.md#FR-13 risk/funds_view] [Source: include/broker_exec/ports/broker_port.hpp (FundsSnapshot), ports/clock_port.hpp]
- [Source: docs/conventions.md] [Source: src/risk/validation_gate.* (sibling module)]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
