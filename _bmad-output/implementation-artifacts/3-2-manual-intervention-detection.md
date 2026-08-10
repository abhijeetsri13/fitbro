# Story 3.2: Manual-intervention detection

Status: ready-for-dev

## Story

As an operator,
I want manual broker-app changes detected,
so that the bot never sends a duplicate exit. (FR-12)

## Acceptance Criteria

1. **Given** a position the bot believes open **When** the user closes it manually in the broker app **Then**
   reconciliation detects the change and updates internal state.
2. **And** no duplicate exit order is sent.
3. **And** the event is alerted and audited.

## Tasks / Subtasks

- [ ] Task 1: Add manual-intervention detection to the EXISTING `reconcile` module (AC: all)
  - [ ] `include/broker_exec/reconcile/manual_intervention.hpp` + `src/reconcile/manual_intervention.cpp`; add to
        `broker_exec_reconcile`. Reuses `ReconcileResult` (3.1), `ports::AlertSink`, domain types. No new dep.
- [ ] Task 2: Classification (AC: 1, 3)
  - [ ] `struct ManualInterventionEvent { enum class Kind { PositionClosedManually, PositionReducedManually,
        OrderCancelledManually }; Kind kind; std::string symbol; std::string client_ref; std::int64_t believed_qty;
        std::int64_t broker_qty; std::string detail; };`
  - [ ] `class ManualInterventionDetector` (ctor `(ports::AlertSink&)`):
    `std::vector<ManualInterventionEvent> detect(const std::vector<domain::Position>& believed_positions,
       const std::vector<domain::Order>& local_orders, const ReconcileResult& truth) const`:
    - For each BELIEVED-open position (net_qty != 0): find the broker position for that symbol in `truth.positions`.
      * broker shows FLAT (net_qty == 0 or symbol absent) AND the bot did NOT itself initiate the close — i.e. there is NO
        local non-terminal "exit" order for that symbol that would explain the flat — => `PositionClosedManually`.
      * broker net_qty is non-zero but SMALLER magnitude / opposite-reduced vs believed, again unexplained by a bot exit =>
        `PositionReducedManually`.
      A close/reduce that IS explained by a bot exit order (a local order for that symbol on the closing side) is NORMAL
      (a legit fill), NOT a manual intervention — do not flag it.
    - For each LOCAL order the bot believes live (non-terminal AND broker-acked, i.e. non-empty broker_order_id) that the
      broker snapshot shows CANCELLED or absent, and the bot did not request the cancel => `OrderCancelledManually`.
    - Every detected event -> `alerts.send(AlertLevel::Warning, <redaction-safe message: kind + symbol/client_ref + qtys>)`
      (client_ref/symbol/qty are NOT secrets). A failing send is swallowed (no throw) but the event still returns (for audit).
- [ ] Task 3: Update internal state + the no-duplicate-exit guarantee (AC: 1, 2)
  - [ ] `void reconcile_positions(const ReconcileResult& truth, std::vector<domain::Position>& local_positions) const`:
        set each local position to the broker's truth (a manually-closed position becomes net_qty == 0 locally; a reduced one
        takes the broker magnitude; a symbol the broker no longer reports becomes flat/removed). This is the state update (AC-1)
        that makes the no-duplicate-exit guarantee structural (AC-2): once the local position reads flat, any exit/square-off
        decision sees nothing to close.
  - [ ] A pure decision helper proving AC-2: `[[nodiscard]] bool needs_exit(const domain::Position& local_position)` (or
        reuse an existing notion) returns false for a flat position — so AFTER reconcile_positions a manually-closed position
        yields NO exit. The test must drive: believe-open -> detect manual close -> reconcile_positions -> needs_exit == false
        (i.e. the bot would NOT generate a second/duplicate exit).
- [ ] Task 4: Audit hook (AC: 3)
  - [ ] The returned `ManualInterventionEvent` list IS the audit record the main loop persists (the audit store is Epic 4); the
        detector's contract is "classify + alert + return for audit". Document that the caller audits the events.
- [ ] Task 5: CMake — extend `src/reconcile/CMakeLists.txt`
  - [ ] Add `manual_intervention.cpp` to `broker_exec_reconcile`; add `manual_intervention_test.cpp` to
        `broker_exec_reconcile_tests`. No new deps.
- [ ] Task 6: Tests (AC: 1, 2, 3) — `src/reconcile/manual_intervention_test.cpp` (CountingAlertSink)
  - [ ] AC-1+AC-3: believed long 50 in "X"; broker truth shows X FLAT; no local exit order -> one `PositionClosedManually`
        event, alert sent (count>0), event names the symbol + qtys.
  - [ ] AC-2 (the crux): after detect, `reconcile_positions` sets local X to flat; `needs_exit(local X) == false` -> NO duplicate
        exit. Contrast: WITHOUT the manual close (broker still shows long 50), needs_exit reflects the real open position.
  - [ ] NOT a manual intervention: the position went flat BUT there is a local bot exit order for X (a legit fill) -> NO event
        (don't false-flag a normal close).
  - [ ] partial reduce: believed long 100, broker long 40, no bot exit -> `PositionReducedManually` (believed_qty 100, broker 40);
        reconcile_positions sets local to 40.
  - [ ] order manual-cancel: a local broker-acked non-terminal order absent/CANCELLED at broker, no bot cancel -> `OrderCancelledManually` + alert.
  - [ ] idempotence/no-throw: a failing AlertSink still returns the events; detecting twice on the same truth is stable.

## Dev Notes

- **Builds on 3.1**: reuse `reconcile::ReconcileResult`; the 3.1 `ReconcileApplier` flags a vanished order as a generic mismatch,
  3.2 CLASSIFIES the POSITION-level manual close/reduce + the order manual-cancel and guarantees no duplicate exit. [architecture.md#FR-12]
- **No duplicate exit (AC-2)** is made STRUCTURAL by reconciling the local position to broker truth (flat) before any exit
  decision — a flat position needs no exit. [architecture.md#FR-12, #manual-intervention]
- **Distinguish manual vs legit fill**: a close EXPLAINED by a local bot exit order is normal; an UNEXPLAINED close is manual. [FR-12]
- **Alert + audit**: AlertSink warning + return the events for the main loop to persist (audit store = Epic 4). Redaction-safe messages.
- **No float / no-throw.** Quantities integer. [docs/conventions.md]
- **Reuse:** `reconcile::ReconcileResult`, `ports::AlertSink`, `domain::Position`/`Order`/`Quantity`, `adapters::fake` + CountingAlertSink (tests).

### References
- [Source: epics.md#Story 3.2] [architecture.md#FR-12 manual-intervention detection] [Source: src/reconcile/reconciler.* (3.1 sibling)]
- [Source: docs/conventions.md] [Source: include/broker_exec/ports/alert_sink.hpp, domain/types.hpp]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
