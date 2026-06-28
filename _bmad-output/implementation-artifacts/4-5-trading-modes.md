# Story 4.5: Trading modes

Status: ready-for-dev

## Story

As an operator,
I want selectable run modes,
so that I can develop and operate safely. (FR-30)

## Acceptance Criteria

1. **Given** a configured mode **When** the bot runs **Then** live/paper/dry-run/replay/monitor-only/exit-only/emergency
   each exhibit their defined behavior.
2. **And** replay binds the Clock to the recorded timeline (deterministic decision-path).
3. **And** dry-run validates but never executes; emergency allows only cancel/square-off.

## Tasks / Subtasks

- [ ] Task 1: Add trading modes to the EXISTING `modes` module (AC: all)
  - [ ] `include/broker_exec/modes/trading_mode.hpp` + `src/modes/trading_mode.cpp`; add to `broker_exec_modes`. Pure logic;
        depends only on `ports`/`errors`. No new dep.
- [ ] Task 2: Mode vocabulary + policy (AC: 1, 3)
  - [ ] `enum class TradingMode { Live, Paper, DryRun, Replay, MonitorOnly, ExitOnly, Emergency };` (+ to_string — stable names).
  - [ ] `struct ModePolicy { bool allows_entry; bool allows_exit; bool live_execution; bool read_only; bool replay_clock; };`
        where `live_execution` = sends to the REAL broker; `read_only` = no orders at all; `replay_clock` = the Clock is bound to
        the recorded timeline.
  - [ ] `[[nodiscard]] ModePolicy policy_for(TradingMode)` — the documented per-mode table:
        - Live:        entry T, exit T, live T, ro F, replay F.
        - Paper:       entry T, exit T, live F (simulated, no real send), ro F, replay F.
        - DryRun:      entry T (validated), exit T (validated), **live F** (validates but never executes — AC-3), ro F, replay F.
        - Replay:      entry F, exit F, live F, ro T (replays the recorded decision-path), **replay T** (Clock bound to recorded timeline — AC-2).
        - MonitorOnly: entry F, exit F, live F, ro T (read-only, no orders).
        - ExitOnly:    entry F, exit T, live T, ro F, replay F (risk-reducing exits only).
        - Emergency:   entry F, exit T (only cancel/square-off), live T, ro F, replay F (AC-3).
- [ ] Task 3: Order/operation gates (AC: 1, 3)
  - [ ] `[[nodiscard]] bool can_place_entry(TradingMode)` / `can_place_exit(TradingMode)` (from the policy).
  - [ ] `[[nodiscard]] bool will_execute_live(TradingMode)` — true ONLY for modes that hit the real broker (Live/ExitOnly/
        Emergency); Paper/DryRun/Replay/MonitorOnly are non-live (AC-3 dry-run validates-but-no-live-execute).
  - [ ] `enum class OrderOp { Entry, Exit, Cancel, SquareOff };` and
        `[[nodiscard]] Result<ports::Ok> require_op_allowed(TradingMode, OrderOp)`: ok()/Error per the mode —
        MonitorOnly/Replay block ALL ops; DryRun allows the op to be VALIDATED but `will_execute_live`==false (document: the gate
        runs, the send is suppressed); ExitOnly allows Exit/Cancel/SquareOff, blocks Entry; **Emergency allows ONLY Cancel and
        SquareOff** (blocks Entry AND a generic Exit that isn't a cancel/square-off — AC-3); Live/Paper allow all (Paper non-live).
        A blocked op -> a typed Error (NotSupported/RiskRejected) naming the mode + op.
  - [ ] `[[nodiscard]] bool uses_recorded_clock(TradingMode)` -> replay_clock (AC-2): the composition root binds a replay Clock
        to the recorded timeline when true (the actual Clock binding is the runtime's; this flags it).
- [ ] Task 4: CMake — extend `src/modes/CMakeLists.txt`
  - [ ] Add `trading_mode.cpp` to `broker_exec_modes`; add `trading_mode_test.cpp` to `broker_exec_modes_tests`. No new deps.
- [ ] Task 5: Tests (AC: 1, 2, 3) — `src/modes/trading_mode_test.cpp`
  - [ ] policy_for each of the 7 modes -> the exact documented flags (a table-driven assertion).
  - [ ] AC-3 dry-run: can_place_entry(DryRun)==true (validated) BUT will_execute_live(DryRun)==false (never executes).
  - [ ] AC-3 emergency: require_op_allowed(Emergency, Cancel)==ok, (Emergency, SquareOff)==ok, (Emergency, Entry)==Error,
        (Emergency, Exit)==Error (only cancel/square-off).
  - [ ] AC-2 replay: uses_recorded_clock(Replay)==true; every other mode false; Replay is read-only (no ops).
  - [ ] monitor-only: every OrderOp blocked; read_only true.
  - [ ] exit-only: Entry blocked, Exit/Cancel/SquareOff allowed; live_execution true.
  - [ ] live vs paper: both allow all ops; live_execution true for Live, false for Paper (paper does not hit the real broker).

## Dev Notes

- **Per-mode behavior table** is the single source of truth (FR-30); the runtime/gate reads it. [architecture.md#FR-30 modes]
- **Dry-run validates, never executes** (AC-3) — the op is allowed to be VALIDATED (the gate runs) but `will_execute_live` is
  false (no real send). **Emergency = only cancel/square-off.** [architecture.md#FR-30]
- **Replay binds the Clock to the recorded timeline** (AC-2) — `uses_recorded_clock(Replay)` flags it; the runtime injects a
  replay Clock. [architecture.md#TO-4 replay determinism, FR-30]
- **No float / no-throw.** Reuse the modes module's style (Story 3.7 PostureCoordinator sibling). [docs/conventions.md]
- **Reuse:** `ports::Ok`, `errors` (NotSupported/RiskRejected).

### References
- [Source: epics.md#Story 4.5] [architecture.md#FR-30 modes, #TO-4 replay] [Source: src/modes/posture.* (3.7 sibling)]
- [Source: docs/conventions.md]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
