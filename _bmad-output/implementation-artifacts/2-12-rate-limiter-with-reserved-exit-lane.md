# Story 2.12: Rate limiter with reserved exit lane

Status: ready-for-dev

## Story

As an operator,
I want broker requests throttled with guaranteed exit capability,
so that throttling never blocks a square-off. (FR-24)

## Acceptance Criteria

1. **Given** the token bucket + admission queue **When** the bucket is drained by entries **Then** square-off/cancel
   still dispatch from a reserved allocation entries cannot consume.
2. **And** a per-send timeout bounds head-of-line blocking (timeout ⇒ UNKNOWN, free the slot).
3. **And** under induced pressure the library slows+alerts instead of letting the broker reject.

## Tasks / Subtasks

- [ ] Task 1: `ratelimit` module (AC: all)
  - [ ] `include/broker_exec/ratelimit/` + `src/ratelimit/`; target `broker_exec_ratelimit` (+ alias). Depends inward on
        `ports` (ClockPort), `errors` only. No new Conan dep. Deterministic via the injected clock (TestClock in tests).
- [ ] Task 2: Token bucket with steady refill (AC: 3) — integer math, NO float
  - [ ] Ctor: `(const ports::ClockPort& clock, int capacity, std::chrono::milliseconds refill_period, int reserved_exit)`.
        `refill_period` = the time to accrue ONE token; `capacity` = max tokens; `reserved_exit` = tokens entries may never consume.
  - [ ] State: `int tokens_` (starts full at capacity), `std::chrono::steady_clock::time_point last_refill_` (from clock.now_steady()),
        and a carry of leftover sub-period time so refill is exact over many calls (no token drift).
  - [ ] `void refill_()` (called at the top of each acquire): `elapsed = now_steady - last_refill_`; `added = elapsed / refill_period`
        (integer division); if added > 0: `tokens_ = min(capacity, tokens_ + added)`; advance `last_refill_ += added * refill_period`
        (keep the remainder so fractional periods accumulate). All `std::chrono` integer arithmetic.
- [ ] Task 3: Reserved exit lane (AC: 1) — the headline
  - [ ] `bool try_acquire(bool is_exit)`: refill_(); the consumable FLOOR for an ENTRY is `reserved_exit` (an entry may consume
        only while `tokens_ > reserved_exit`, leaving the reserved pool intact); for an EXIT the floor is 0 (an exit may consume
        while `tokens_ >= 1`, drawing from the reserved pool if needed). On success decrement tokens_ and return true; else false.
  - [ ] `Result<ports::Ok> acquire(bool is_exit)`: try_acquire ? ok() : a `RateLimited` Error (SuggestedAction::RetrySafe) so the
        runtime SLOWS/queues + alerts rather than hammering the broker (AC-3). The Error names that the limiter is throttling
        (entries vs exits).
  - [ ] `int available(bool is_exit) const`: tokens minus the applicable floor, clamped >= 0 (diagnostics / admission decisions).
- [ ] Task 4: Per-send timeout note (AC: 2)
  - [ ] AC-2 (per-send timeout ⇒ UNKNOWN, free the slot) is realized by the dispatch chokepoint (Story 1.9) which already marks
        a timed-out dangerous op UNKNOWN and reconciles; the rate limiter's contribution is ADMISSION (which request enters
        dispatch and when a token is free) — it never owns the broker socket. Document the boundary (admission-only, per the
        architecture CC-1/CC-5 resolution); no dispatch change here. (A token consumed for a request that then times out is
        NOT returned — the request was really sent; the bucket is a send-rate limiter, not a concurrency pool.)
- [ ] Task 5: CMake (orchestrator pre-wires root add_subdirectory(src/ratelimit); NO new Conan dep)
  - [ ] `src/ratelimit/CMakeLists.txt`: links PUBLIC `broker_exec::ports` `broker_exec::errors`; PRIVATE warnings+sanitizers.
        Test exe `broker_exec_ratelimit_tests` ALSO links `broker_exec::clock` (TestClock).
- [ ] Task 6: Tests (AC: 1, 2, 3) — `src/ratelimit/rate_limiter_test.cpp` (TestClock for deterministic steady time)
  - [ ] HEADLINE (AC-1): capacity 10, reserved_exit 3. Drain with ENTRIES: the first 7 entry acquires succeed, the 8th entry is
        DENIED (RateLimited) — but an EXIT acquire STILL succeeds (draws from the reserved 3). Then exits can drain the rest.
  - [ ] exit can drain to zero (an exit acquire succeeds while tokens >= 1, incl. the reserved pool); a further exit at 0 is denied.
  - [ ] entries cannot touch the reserved pool: with tokens at exactly reserved_exit, an entry is denied while an exit succeeds.
  - [ ] refill (AC-3): drain fully, advance the TestClock by N refill_periods -> exactly N tokens accrue (capped at capacity);
        an entry then succeeds again. Advancing less than one period grants 0 (and the carry accumulates so two half-periods = 1 token).
  - [ ] acquire() returns a RateLimited/RetrySafe Error when denied (the slow+alert signal), ok() when granted.
  - [ ] deterministic: identical clock advances produce identical grant/deny sequences.

## Dev Notes

- **Reserved exit lane** = entries floor at `reserved_exit`, exits floor at 0 (exits may consume the reserved pool). Conformance:
  drain with entries, an exit still dispatches. [architecture.md#CC-5 reserved exit lane, FR-24]
- **Admission only** — the limiter decides which order enters dispatch and when a token is free; it never owns the socket or the
  Store. The per-send timeout ⇒ UNKNOWN is the dispatcher (Story 1.9). [architecture.md#CC-1, #CC-5]
- **Steady clock, integer math** — refill measured on the monotonic clock with a carry so there is no token drift; NO float.
  [docs/conventions.md — steady for durations, no float]
- **Errors:** `RateLimited` + `RetrySafe` on a denied acquire (runtime slows/queues + alerts, never lets the broker reject). [FR-24]
- **Reuse:** `ports::ClockPort`, `errors`, `clock::TestClock` (tests).

### References
- [Source: epics.md#Story 2.12] [architecture.md#CC-1/CC-5 reserved exit lane, FR-24]
- [Source: docs/conventions.md] [Source: include/broker_exec/ports/clock_port.hpp]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
