# Story 3.4: Crash recovery (reconcile-before-resume)

Status: ready-for-dev

## Story

As an operator,
I want safe recovery after a crash,
so that the bot never trades immediately on restart. (FR-14)

## Acceptance Criteria

1. **Given** an in-flight order and a process kill **When** the bot restarts **Then** it loads state, checks session,
   fetches broker order book/trades/positions, resolves unknowns, and resumes only when safe.
2. **And** zero duplicate orders result.
3. **And** a double-fault (UNKNOWN + broker unreachable) lands in MANUAL_INTERVENTION_REQUIRED with escalation, no
   auto-square-off.

## Tasks / Subtasks

- [ ] Task 1: Add the recovery coordinator to the EXISTING `reconcile` module (AC: all)
  - [ ] `include/broker_exec/reconcile/recovery.hpp` + `src/reconcile/recovery.cpp`; add to `broker_exec_reconcile`. Composes
        the 3.1 `Reconciler`/`ReconcileApplier` + the lifecycle FSM + injected seams for load-state (intent-log replay, 1.5),
        session check, and the safe-start gate (2.13). No new dep. Issues NO broker mutations — recovery only reads + reconciles.
- [ ] Task 2: The recovery state machine (AC: 1)
  - [ ] `enum class RecoveryStatus { ResumedSafe, Blocked, ManualInterventionRequired };`
  - [ ] `struct RecoveryOutcome { RecoveryStatus status = RecoveryStatus::Blocked; std::vector<domain::Order> orders;
        int unknowns_resolved = 0; int unknowns_unresolved = 0; bool escalated = false; std::string detail; };`
  - [ ] `class RecoveryCoordinator` — ctor injects: `ports::BrokerPort& broker`, `ports::AlertSink& alerts`,
        `const ports::ClockPort& clock`, `lifecycle::LifecycleEngine& engine`, and three seams:
        `std::function<Result<std::vector<domain::Order>>()> load_state` (intent-log replay -> the orders that MIGHT have been
        sent, incl. any Unknown/Sent), `std::function<Result<ports::Ok>()> check_session`,
        `std::function<Result<ports::Ok>()> safe_start_check`.
  - [ ] `RecoveryOutcome recover()` runs, in order (a fail short-circuits to the right status — fail-closed):
        1. **LoadState**: `load_state()`; on Error -> Blocked + alert (cannot recover yet). Else take the orders.
        2. **CheckSession**: `check_session()`; on Error -> Blocked (re-establish needed) — never resume on a dead session.
        3. **FetchBroker**: `Reconciler(clock).fetch(broker, seq)`; on Error (broker UNREACHABLE):
           - if ANY loaded order is `OrderState::Unknown` -> **DOUBLE FAULT**: set each Unknown order to
             `OrderState::ManualInterventionRequired`, `alerts.send(AlertLevel::Critical, ...)` (escalation), status =
             `ManualInterventionRequired`, escalated = true. **NO auto-square-off / no mutation.** Return (terminal).
           - else -> Blocked (cannot reconcile yet; retry later — not terminal).
        4. **ResolveUnknowns**: `ReconcileApplier(engine, alerts).apply(result, orders)` converges local orders to broker
           truth via the FSM (forward-progressing). After apply, count orders still `Unknown` (unresolvable against broker
           truth -> they stay Unknown + were alerted by the applier — fail-closed, NOT auto-squared-off). unknowns_resolved /
           unknowns_unresolved are recorded.
        5. **SafeOrBlocked**: `safe_start_check()`; on Error -> Blocked. Else if `unknowns_unresolved > 0` -> Blocked (do not
           resume with open unknowns). Else -> **ResumedSafe**.
- [ ] Task 3: Guarantees (AC: 2, 3)
  - [ ] **Zero duplicates (AC-2)**: `recover()` issues NO `place/modify/cancel/square_off` — it only reads (fetch) + applies
        FSM views to local state. A test asserts the broker's request/mutation count is unchanged across recovery (no re-send).
  - [ ] **Double-fault (AC-3)**: UNKNOWN + broker unreachable -> `ManualInterventionRequired` (terminal), a Critical escalation
        alert, and NO square-off (the coordinator never calls broker.square_off). Document: a human must intervene.
- [ ] Task 4: CMake — extend `src/reconcile/CMakeLists.txt`
  - [ ] Add `recovery.cpp` to `broker_exec_reconcile`; add `recovery_test.cpp` to `broker_exec_reconcile_tests` (already links
        lifecycle, clock, fake_broker).
- [ ] Task 5: Tests (AC: 1, 2, 3) — `src/reconcile/recovery_test.cpp` (FakeBroker, CountingAlertSink, TestClock, a fresh LifecycleEngine)
  - [ ] clean recovery: loaded orders converge to broker truth, no Unknowns, session ok, safe-start ok -> ResumedSafe; assert
        the broker received NO new mutation (request count unchanged) — zero duplicates (AC-2).
  - [ ] unknown resolved: a loaded Unknown order that the broker truth matches -> applier advances it -> unknowns_unresolved == 0
        -> ResumedSafe.
  - [ ] unknown unresolved (broker reachable, no match): stays Unknown + alerted -> unknowns_unresolved > 0 -> Blocked (NOT resumed, NOT squared off).
  - [ ] DOUBLE FAULT (AC-3): a loaded Unknown order + a broker whose fetch FAILS (unreachable) -> status ManualInterventionRequired,
        the order's state == ManualInterventionRequired, a Critical alert sent, escalated == true, and NO square_off was issued
        (assert the broker's square-off/mutation count is 0).
  - [ ] session bad: check_session -> Error -> Blocked (never resume).
  - [ ] safe-start fail: all else ok but safe_start_check -> Error -> Blocked.
  - [ ] load-state fail: load_state -> Error -> Blocked + alert.

## Dev Notes

- **Reconcile-before-resume** — load (replay) -> session -> fetch broker truth -> resolve unknowns -> resume ONLY when safe.
  Recovery is read+reconcile only; it NEVER re-sends an order (zero duplicates, AC-2). [architecture.md#FR-14, #NFR-2 double-fault]
- **Double-fault = MANUAL_INTERVENTION_REQUIRED** (UNKNOWN + broker unreachable): terminal, escalate (Critical alert), NEVER
  auto-square-off a phantom. [architecture.md#NFR-2 double-fault, FR-14]
- **Composes**: `reconcile::Reconciler`/`ReconcileApplier` (3.1), `lifecycle::LifecycleEngine`, injected intent-log replay (1.5),
  session check (2.4), safe-start gate (2.13). Seams keep it decoupled + deterministically testable.
- **No float / no-throw / fail-closed.** [docs/conventions.md]
- **Reuse:** `ports::BrokerPort`/`AlertSink`/`ClockPort`, `lifecycle::LifecycleEngine`, `reconcile::Reconciler`/`ReconcileApplier`,
  `domain::Order`/`OrderState{Unknown,ManualInterventionRequired}`, `adapters::fake::FakeBroker` + `clock::TestClock` (tests).

### References
- [Source: epics.md#Story 3.4] [architecture.md#FR-14 crash recovery, #NFR-2 double-fault] [Source: src/reconcile/reconciler.* (3.1), include/broker_exec/intentlog/intent_log.hpp]
- [Source: docs/conventions.md] [Source: include/broker_exec/ports/broker_port.hpp, domain/enums.hpp]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
