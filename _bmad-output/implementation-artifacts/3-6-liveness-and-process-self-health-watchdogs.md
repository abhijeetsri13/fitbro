# Story 3.6: Liveness and process self-health watchdogs

Status: ready-for-dev

## Story

As an operator,
I want connected-but-mute feeds and resource exhaustion detected,
so that the bot degrades before it silently fails. (FR-20)

## Acceptance Criteria

1. **Given** a still-connected WebSocket delivering no ticks past threshold **When** the watchdog runs **Then** it
   forces reconnect-or-degrade.
2. **And** disk (WAL/audit growth)/memory/handle limits trigger safe degradation + alert before corrupting logging.
3. **And** clock-skew/stall feeds the same degrade-to-exit-only path.

## Tasks / Subtasks

- [ ] Task 1: `health` module (AC: all)
  - [ ] `include/broker_exec/health/` + `src/health/`; target `broker_exec_health` (+ alias). Depends inward on `ports`
        (ClockPort, AlertSink), `errors`, and `marketdata` (for the mute-feed helper). NO new Conan dep. The watchdog DETECTS +
        signals degrade — the posture mapping is Story 3.7; here a breach => degrade-to-exit-only, fail-closed.
- [ ] Task 2: Health signals + verdict (AC: all)
  - [ ] `enum class HealthSignal { MuteFeed, DiskPressure, MemoryPressure, HandlePressure, ClockSkew, ClockStall };` (+ to_string).
  - [ ] `struct ResourceUsage { std::int64_t disk_bytes = 0; std::int64_t mem_bytes = 0; int handle_count = 0; };`
  - [ ] `struct ResourceLimits { std::int64_t max_disk_bytes = 0; std::int64_t max_mem_bytes = 0; int max_handles = 0; };`
        (0 = no limit. These are HEADROOM limits set BELOW true capacity so the alert fires BEFORE logging/WAL corruption — AC-2.)
  - [ ] `struct WatchdogInputs { ResourceUsage usage; bool feed_connected_but_mute = false; bool clock_skew = false; bool clock_stall = false; };`
  - [ ] `struct WatchdogVerdict { std::vector<HealthSignal> breaches; std::string detail;
        [[nodiscard]] bool healthy() const { return breaches.empty(); }
        [[nodiscard]] bool degrade_to_exit_only() const { return !breaches.empty(); } };`
- [ ] Task 3: `Watchdog` (AC: 1, 2, 3)
  - [ ] Ctor `(ResourceLimits limits, ports::AlertSink& alerts)`.
  - [ ] `[[nodiscard]] WatchdogVerdict check(const WatchdogInputs&) const`:
    - `feed_connected_but_mute` -> `MuteFeed` (AC-1: force reconnect-or-degrade — the verdict signals degrade; the transport
      owns the actual reconnect attempt).
    - `usage.disk_bytes > max_disk_bytes` (when max>0) -> `DiskPressure`; same for mem/handles (AC-2).
    - `clock_skew` -> `ClockSkew`; `clock_stall` -> `ClockStall` (AC-3: same degrade-to-exit-only path).
    - For EACH breach: `alerts.send(<level>, <redaction-safe message naming the signal + the breached value/limit>)`
      (DiskPressure/MemoryPressure/HandlePressure = Error; MuteFeed/ClockSkew/ClockStall = Error/Critical — pick a sane level;
      a swallowed send Result, no throw). Resource breaches alert BEFORE the resource is actually exhausted (the headroom limit).
    - `healthy()` iff no breach; ANY breach => `degrade_to_exit_only()` true. Fail-closed: a breach is never ignored.
- [ ] Task 4: Mute-feed helper reusing Story 3.5 (AC: 1)
  - [ ] `[[nodiscard]] bool feed_connected_but_mute(const marketdata::MarketDataView& view, std::string_view symbol)`:
        true iff the view is NOT Disconnected (the socket is up) AND `view.state_for(symbol)` is Stale (connected but no fresh
        tick past threshold). (A Disconnected feed is a different signal — the transport reconnect path; a Live/Unknown feed is
        not mute.) Document the mapping; the caller passes the result as `WatchdogInputs::feed_connected_but_mute`.
- [ ] Task 5: CMake (orchestrator pre-wires root add_subdirectory(src/health); NO new Conan dep)
  - [ ] `src/health/CMakeLists.txt`: links PUBLIC `broker_exec::ports` `broker_exec::errors`; PRIVATE `broker_exec::marketdata`
        warnings+sanitizers. Test exe `broker_exec_health_tests` ALSO links `broker_exec::marketdata` + `broker_exec::clock` (TestClock for the view).
- [ ] Task 6: Tests (AC: 1, 2, 3) — `src/health/watchdog_test.cpp` (CountingAlertSink; a MarketDataView for the helper)
  - [ ] all healthy (usage under limits, no mute, clock ok) -> healthy(), no breach, no alert, NOT degrade.
  - [ ] AC-1 mute feed: feed_connected_but_mute=true -> MuteFeed breach, degrade_to_exit_only true, alert sent.
  - [ ] AC-2 disk over headroom limit -> DiskPressure + alert; mem over -> MemoryPressure; handles over -> HandlePressure;
        assert the breach fires when usage > limit (the limit is below capacity, so the alert precedes real exhaustion).
  - [ ] AC-3 clock_skew -> ClockSkew + degrade; clock_stall -> ClockStall + degrade.
  - [ ] multiple breaches at once -> all reported in `breaches`, degrade true, an alert per breach.
  - [ ] 0 limit = no limit: a huge usage with max_*=0 -> no resource breach.
  - [ ] mute helper: a MarketDataView connected + a stale symbol -> feed_connected_but_mute true; a Live symbol -> false; a
        Disconnected view -> false (mute is "connected but mute", not disconnected).
  - [ ] boundary: usage exactly == limit is NOT a breach (strictly greater breaches).

## Dev Notes

- **Detect + signal only** — the watchdog DETECTS liveness/health breaches and signals degrade-to-exit-only; the posture
  coordinator (Story 3.7) maps each signal to a concrete posture; the gate (2.8) enforces it. [architecture.md#FR-20, #FR-26]
- **Alert BEFORE corruption** (AC-2) — resource limits are headroom thresholds set below true capacity so degradation + alert
  happen before WAL/audit/logging is corrupted. [architecture.md#FR-20]
- **Mute feed reuses 3.5** — connected-but-Stale via `marketdata::MarketDataView`; clock-skew/stall is an injected detector
  signal (the real one is the Story-1.3 SkewStallDetector) feeding the same degrade path. [architecture.md#FR-20, FR-23]
- **Fail-closed / no float / no-throw.** Integer byte/handle counts; AlertSink Result swallowed. [docs/conventions.md]
- **Reuse:** `ports::AlertSink`/`ClockPort`, `marketdata::MarketDataView` (3.5), `errors`, `clock::TestClock` (tests).

### References
- [Source: epics.md#Story 3.6] [architecture.md#FR-20 watchdogs, #FR-26 posture] [Source: src/marketdata/* (3.5)]
- [Source: docs/conventions.md] [Source: include/broker_exec/ports/alert_sink.hpp]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
