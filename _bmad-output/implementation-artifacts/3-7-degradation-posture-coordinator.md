# Story 3.7: Degradation-posture coordinator

Status: ready-for-dev

## Story

As an operator,
I want one authority mapping each failure to a defined posture,
so that degradation is coherent, never contradictory. (FR-26)

## Acceptance Criteria

1. **Given** detector signals (broker-down, stale-data, UNKNOWN, mismatch, risk-breach, session-expiry, clock-stall,
   mute-feed) **When** any fires **Then** the coordinator selects a posture from the existing vocabulary
   (block-entries/exit-only/soft-kill/panic) and the gate enforces it as the single chokepoint.
2. **And** detectors feed the coordinator; they do not each decide a posture.
3. **And** each failure scenario produces its specified behavior without duplicate orders or silent failure.

## Tasks / Subtasks

- [ ] Task 1: `modes` module (AC: all)
  - [ ] `include/broker_exec/modes/` + `src/modes/`; target `broker_exec_modes` (+ alias). Depends inward on `ports`
        (AlertSink), `errors`, and `health` (to map `health::HealthSignal`). NO new Conan dep. The coordinator is the SINGLE
        posture authority (AC-2): detectors only FEED it `DetectorSignal`s; they never pick a posture themselves.
- [ ] Task 2: Posture + signal vocabulary (AC: 1)
  - [ ] `enum class Posture { Normal, BlockEntries, ExitOnly, SoftKill, Panic };` — a TOTAL ORDER by severity
        (Normal < BlockEntries < ExitOnly < SoftKill < Panic); the underlying enum values must be ascending so `std::max` works.
  - [ ] `enum class DetectorSignal { StaleData, MuteFeed, Unknown, Mismatch, SessionExpiry, BrokerDown, ClockSkew, ClockStall,
        ResourcePressure, RiskBreach };` (+ to_string for both).
- [ ] Task 3: The coordinator (AC: 1, 2)
  - [ ] `[[nodiscard]] Posture posture_for(DetectorSignal)` — each signal's MINIMUM posture (documented mapping):
        StaleData/MuteFeed/Unknown/Mismatch/SessionExpiry -> BlockEntries (price-sensitive/new-order block; resolve/reconcile/
        re-establish proceeds); BrokerDown/ClockSkew/ClockStall/ResourcePressure -> ExitOnly (degrade-to-exit-only); RiskBreach
        -> SoftKill (stop the strategy/account, allow exits). (Panic is reached only via the operator kill switch — Story 3.8 —
        folded in below; no detector maps to Panic.)
  - [ ] `class PostureCoordinator`:
    - `[[nodiscard]] Posture evaluate(const std::vector<DetectorSignal>& active, Posture operator_floor = Posture::Normal) const`:
      return the SEVEREST (max) of `operator_floor` and `posture_for(s)` over all active signals; `Normal` when none active and
      no operator floor. This is the single coherent decision (AC-1/AC-2) — a contradictory mix resolves to the most severe.
    - `[[nodiscard]] static bool allows_entries(Posture p)` -> `p == Posture::Normal` (any degraded posture blocks new entries).
    - `[[nodiscard]] static bool allows_risk_reducing_exits(Posture p)` -> true for Normal/BlockEntries/ExitOnly/SoftKill;
      FALSE for Panic (under Panic the emergency engine drives the square-off out-of-band; the normal gate blocks everything).
    - `[[nodiscard]] static Result<ports::Ok> require_entry_allowed(Posture p)`: ok() iff Normal; else a typed
      `RiskRejected` (action BlockStrategy) Error naming the posture — the gate's posture chokepoint (composes with Story 2.8).
    - `[[nodiscard]] static DetectorSignal from_health(health::HealthSignal)`: map the watchdog signals into the coordinator's
      vocabulary (MuteFeed->MuteFeed; ClockSkew->ClockSkew; ClockStall->ClockStall; Disk/Memory/Handle Pressure->ResourcePressure).
    - Optional `Posture evaluate_and_alert(const std::vector<DetectorSignal>&, Posture operator_floor, ports::AlertSink&) const`:
      compute the posture and, when it is not Normal, send one redaction-safe alert (level by severity); a swallowed Result, no throw.
- [ ] Task 4: Gate integration note (AC: 1, 3)
  - [ ] Document the wiring: the runtime computes the posture each loop tick and sets the Story-2.8 gate's `kill_entry_block`
        (= `!allows_entries(posture)`) and treats exits per `allows_risk_reducing_exits` — so the gate ENFORCES the posture as
        the single chokepoint, no duplicate orders, no silent failure (every degraded posture blocks new entries loudly). No
        gate code change required here; the coordinator provides the decision.
- [ ] Task 5: CMake (orchestrator pre-wires root add_subdirectory(src/modes); NO new Conan dep)
  - [ ] `src/modes/CMakeLists.txt`: links PUBLIC `broker_exec::ports` `broker_exec::errors`; PRIVATE `broker_exec::health`
        warnings+sanitizers. Test exe `broker_exec_modes_tests` ALSO links `broker_exec::health` (for HealthSignal mapping test).
- [ ] Task 6: Tests (AC: 1, 2, 3) — `src/modes/posture_test.cpp` (CountingAlertSink)
  - [ ] no active signals -> Normal; allows_entries true; require_entry_allowed ok.
  - [ ] each DetectorSignal in isolation -> its documented posture (StaleData->BlockEntries, BrokerDown->ExitOnly, RiskBreach->SoftKill, ...).
  - [ ] severest wins (AC-1): {StaleData, BrokerDown} -> ExitOnly; {StaleData, RiskBreach} -> SoftKill; {MuteFeed} + operator_floor Panic -> Panic.
  - [ ] total order: Normal < BlockEntries < ExitOnly < SoftKill < Panic (assert the enum ordering used by max).
  - [ ] allows_entries: only Normal true; every degraded posture blocks entries. allows_risk_reducing_exits: true except Panic.
  - [ ] require_entry_allowed: Normal ok; each degraded posture -> RiskRejected Error naming the posture.
  - [ ] from_health: MuteFeed->MuteFeed, ClockStall->ClockStall, DiskPressure->ResourcePressure (etc.); fed through evaluate yields the right posture.
  - [ ] evaluate_and_alert: Normal -> no alert; a degraded posture -> exactly one alert (redaction-safe).

## Dev Notes

- **Single authority** (AC-2) — detectors feed `DetectorSignal`s; the coordinator alone selects the posture (severest over
  active signals + the operator floor). No detector decides a posture. [architecture.md#FR-26, #RCT-2 modes/posture]
- **Severest-wins** total order resolves a contradictory mix coherently; `std::max` over ascending enum values. [architecture.md#FR-26]
- **Gate enforces** (AC-1/AC-3) — the runtime maps the posture onto the Story-2.8 gate (`kill_entry_block`/exit policy); every
  degraded posture blocks new entries loudly (no silent failure, no duplicate orders). [architecture.md#FR-26, FR-6]
- **Panic via the kill switch** (Story 3.8) — folded in as `operator_floor`; no detector escalates to Panic on its own.
- **No float / no-throw.** AlertSink Result swallowed. [docs/conventions.md]
- **Reuse:** `health::HealthSignal` (3.6), `ports::AlertSink`, `errors` (RiskRejected).

### References
- [Source: epics.md#Story 3.7] [architecture.md#FR-26 modes/posture coordinator, #RCT-2] [Source: src/health/* (3.6), src/risk/validation_gate.* (2.8)]
- [Source: docs/conventions.md]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
