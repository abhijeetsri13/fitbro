# Story 3.1: Continuous reconciliation (fetch-off-loop, apply-on-loop)

Status: ready-for-dev

## Story

As an operator,
I want state continuously reconciled against the broker on every trigger,
so that local state always converges to broker truth. (FR-11)

## Acceptance Criteria

1. **Given** the reconciler on the scheduler thread **When** a trigger fires (startup/login/reconnect/post-unknown/
   around square-off/periodic/WS-disconnect/manual-intervention-suspected) **Then** it performs broker reads only and
   enqueues an immutable result; the main loop applies all diffs (sole writer).
2. **And** cadence is adaptive — tight (~1–2s) while SENT/UNKNOWN or a position is open, loose (~15–30s) when flat.
3. **And** a mismatch raises an alert and optionally blocks new orders.

## Tasks / Subtasks

- [ ] Task 1: `reconcile` module (AC: all)
  - [ ] `include/broker_exec/reconcile/` + `src/reconcile/`; target `broker_exec_reconcile` (+ alias). Depends inward on
        `domain`, `ports`, `lifecycle`, `errors`. The FETCH side touches the broker only (reads); the APPLY side is the only
        writer (reuses the lifecycle FSM). No new Conan dep.
- [ ] Task 2: `ReconcileResult` — the immutable snapshot (AC: 1)
  - [ ] An immutable value type carrying the broker reads: `std::vector<domain::Order> orders`, `std::vector<domain::Trade>
        trades`, `std::vector<domain::Position> positions`, `ports::FundsSnapshot funds`, `std::int64_t ordering_key`
        (a monotonic snapshot sequence — the apply-ordering discriminator; a full reconciler snapshot is authoritative over a
        single WS push, so this key is set high per the architecture), and a `fetched_at` wall stamp (ClockPort). Const-only access.
- [ ] Task 3: `Reconciler::fetch` — broker READS ONLY, off-loop (AC: 1)
  - [ ] Ctor: `(const ports::ClockPort& clock)`. `Result<ReconcileResult> fetch(ports::BrokerPort& broker, std::int64_t snapshot_seq)`:
        calls `fetch_orders()/fetch_trades()/fetch_positions()/fetch_funds()` — NOTHING else. A read failure -> a typed Error
        (the trigger handler decides; e.g. reconcile-first/retry-safe). It holds NO Store / LifecycleEngine reference — the
        fetch side STRUCTURALLY cannot write (this is the fetch-off-loop guarantee; document it).
- [ ] Task 4: Apply-on-loop — the SOLE writer (AC: 1, 3)
  - [ ] `class ReconcileApplier` (constructed with `lifecycle::LifecycleEngine&` + `ports::AlertSink&`). 
        `ReconcileOutcome apply(const ReconcileResult& result, std::vector<domain::Order>& local_orders) const`:
        - For each broker order, find the local order by `client_ref` (fallback `broker_order_id`); build a
          `lifecycle::BrokerView{ client_ref, observed_state = broker order's state, ordering_key = result.ordering_key }` and
          call `engine.apply(local_order, view)` — forward-progressing + terminal-absorbing (the FSM drops a stale/older view).
          Count each `ApplyOutcome` (advanced / dropped-stale / etc.).
        - MISMATCH detection (AC-3): a broker order with NO local match (a phantom / manually-placed order the bot did not
          create) is a mismatch; a local order the bot believes live that the broker's snapshot does NOT contain is a mismatch
          (a candidate manual intervention — full handling is Story 3.2, but flag it here). On ANY mismatch: `alerts.send(
          AlertLevel::Warning|Error, <redaction-safe message naming the kind + ref>)` and set `outcome.block_new_orders = true`.
        - Returns `ReconcileOutcome { int applied; int advanced; int dropped_stale; int mismatches; bool block_new_orders; }`.
        - The applier is the ONLY thing that mutates `local_orders`/the FSM — fetch never does (sole-writer, AC-1).
- [ ] Task 5: Adaptive cadence (AC: 2)
  - [ ] `struct ReconcileState { bool any_inflight; bool any_open_position; };` (any_inflight = any local order in
        Sent/Unknown-ish non-terminal-uncertain state). `std::chrono::milliseconds next_cadence(const ReconcileState&, std::chrono::milliseconds tight, std::chrono::milliseconds loose)`:
        returns `tight` when `any_inflight || any_open_position`, else `loose`. Provide a helper to derive ReconcileState from a
        `std::vector<domain::Order>` + positions (any non-flat). Defaults tight≈1500ms, loose≈20000ms (documented; integer ms, no float).
- [ ] Task 6: CMake (orchestrator pre-wires root add_subdirectory(src/reconcile); NO new Conan dep)
  - [ ] `src/reconcile/CMakeLists.txt`: links PUBLIC `broker_exec::domain` `broker_exec::ports` `broker_exec::errors`;
        PRIVATE `broker_exec::lifecycle` warnings+sanitizers. Test exe `broker_exec_reconcile_tests` ALSO links
        `broker_exec::lifecycle` + `broker_exec::clock` + `broker_exec::fake_broker` (drive fetch via the FakeBroker).
- [ ] Task 7: Tests (AC: 1, 2, 3) — `src/reconcile/reconcile_test.cpp`
  - [ ] fetch (AC-1): drive a FakeBroker; `fetch()` returns a ReconcileResult populated from all four reads; a broker read
        failure -> Error. (Optionally assert the Reconciler type has no Store/FSM member — at least document the reads-only contract.)
  - [ ] apply forward-progress (AC-1): a local order in a non-terminal state + a broker view advancing it -> the FSM advances it;
        a STALE/older `ordering_key` (lower than last-applied) -> dropped, NOT regressed (reuse the FSM's apply-ordering — assert via ApplyOutcome counts).
  - [ ] terminal-absorbing: a local order already FILLED/REJECTED/CANCELLED is not moved by a contradicting older view.
  - [ ] mismatch (AC-3): a broker snapshot containing an order with NO local match -> outcome.mismatches>0, an alert was sent
        (CountingAlertSink), and block_new_orders==true. A local order absent from the broker snapshot -> also flagged.
  - [ ] cadence (AC-2): any_inflight -> tight; any_open_position -> tight; flat -> loose. Derivation helper from orders/positions.
  - [ ] sole-writer (AC-1): fetch() does not mutate any local state (it only returns the immutable result); apply() is the only
        mutator — demonstrate by applying the SAME result twice is idempotent (same ordering_key -> second apply drops as same-observation).

## Dev Notes

- **Fetch-off-loop / apply-on-loop** — the reconciler READS (no writes); the main loop APPLIES all diffs as the sole writer.
  Structurally: the Reconciler holds no Store/FSM; the ReconcileApplier is the only mutator. [architecture.md#COH-1, #CC-4, FR-11]
- **Apply-ordering** — reuse `lifecycle::LifecycleEngine::apply` (forward-progressing, terminal-absorbing); a full reconciler
  snapshot's `ordering_key` is authoritative over a single WS push (set it high). [architecture.md#CC-7]
- **Adaptive cadence** — tight while SENT/UNKNOWN or any open position; loose when flat. [architecture.md#IBR-5 cadence, FR-11]
- **Mismatch -> alert + optionally block** — a phantom/unmatched order or a vanished local order raises an alert (AlertSink) and
  sets block_new_orders; full manual-intervention classification is Story 3.2. [architecture.md#FR-11, FR-12]
- **No float / no-throw / immutable result.** Integer ms cadence; Result<T> on fetch. [docs/conventions.md]
- **Reuse:** `ports::BrokerPort`/`FundsSnapshot`/`AlertSink`/`ClockPort`, `lifecycle::LifecycleEngine`/`BrokerView`/`ApplyOutcome`,
  `domain` types, `adapters::fake::FakeBroker` + `clock::TestClock` (tests).

### References
- [Source: epics.md#Story 3.1] [architecture.md#COH-1/CC-4 fetch-off/apply-on, #CC-7 apply-ordering, #IBR-5 cadence, FR-11]
- [Source: include/broker_exec/lifecycle/lifecycle.hpp (BrokerView/apply), ports/broker_port.hpp, ports/alert_sink.hpp]
- [Source: docs/conventions.md]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
