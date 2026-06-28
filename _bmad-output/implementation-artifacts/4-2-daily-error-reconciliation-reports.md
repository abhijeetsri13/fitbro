# Story 4.2: Daily/error/reconciliation reports

Status: ready-for-dev

## Story

As an operator,
I want end-of-session reports,
so that I get a verdict, not just a stream of events. (FR-27)

## Acceptance Criteria

1. **Given** a trading session **When** it ends **Then** daily, error, and reconciliation reports are generated.
2. **And** the reconciliation report states intended/sent/confirmed/reconciled vs broker.
3. **And** reports are reproducible from the audit data.

## Tasks / Subtasks

- [ ] Task 1: Add report generation to the EXISTING `observability` module (AC: all)
  - [ ] `include/broker_exec/observability/reports.hpp` + `src/observability/reports.cpp`; add to `broker_exec_observability`.
        Reuses Story-4.1 `AuditEvent`/`EventType`/`AuditTrail`; reports are PURE functions of a `std::vector<AuditEvent>` (no
        clock, no I/O) so the same audit data always yields the same report (AC-3). No new dep; nlohmann for the JSON render.
- [ ] Task 2: Daily report (AC: 1)
  - [ ] `struct DailyReport { int orders_placed=0, filled=0, rejected=0, cancelled=0, unknown=0; std::int64_t realized_pnl_paise=0;
        std::map<std::string,int> orders_per_strategy; std::string to_json() const; };`
  - [ ] Built by counting EventType occurrences (OrderPlaced/Filled/Rejected/Cancelled/OrderUnknown) and summing a `pnl` integer
        from the events' `fields` (integer paise ONLY — never a double; if a `fields["pnl"]` value is non-integer it is ignored
        and noted, not coerced to float). Per-strategy order counts from `ev.strategy`.
- [ ] Task 3: Error report (AC: 1)
  - [ ] `struct ErrorReportEntry { std::string ts, client_ref, detail; };` `struct ErrorReport { std::vector<ErrorReportEntry> errors;
        std::string to_json() const; };` — every `EventType::Error` event in input order, carrying its ts/client_ref and a
        redaction-safe detail (reuse the event's already-scrubbed fields; do NOT re-introduce a secret).
- [ ] Task 4: Reconciliation report (AC: 2)
  - [ ] `struct OrderReconciliation { std::string client_ref; bool intended=false, sent=false, confirmed=false, reconciled=false;
        std::string final_state; };`
  - [ ] `struct ReconciliationReport { std::vector<OrderReconciliation> orders; int intended=0, sent=0, confirmed=0, reconciled=0;
        std::vector<std::string> discrepancies; std::string to_json() const; };`
  - [ ] Per client_ref, derive from the event types (documented mapping):
        `intended` = an OrderPlaced exists; `sent` = OrderPlaced exists (placing IS the send); `confirmed` = ANY broker verdict
        exists — OrderAcknowledged OR OrderPartiallyFilled OR OrderFilled OR OrderRejected OR OrderCancelled (a broker reject/cancel
        IS the broker confirming the outcome, not a silent gap); `reconciled` = a ReconcileResult exists; `final_state` = the last
        terminal/known state (Filled/Rejected/Cancelled/Unknown) seen for the order. Totals = counts across orders. A DISCREPANCY
        (AC-2) is recorded (naming the client_ref) when intended && !confirmed (placed but NO broker verdict at all — a possible
        UNKNOWN), or fill-specific had_fill && !reconciled ("filled but not reconciled" — only an actual fill changes position and
        must be reconciled; an ack/reject/cancel without a fill produces no discrepancy). This is the intended-vs-sent-vs-confirmed-
        vs-reconciled-vs-broker statement.
- [ ] Task 5: Generator + reproducibility (AC: 3)
  - [ ] `class ReportGenerator { public: static DailyReport daily(const std::vector<AuditEvent>&); static ErrorReport errors(const std::vector<AuditEvent>&);
        static ReconciliationReport reconciliation(const std::vector<AuditEvent>&); };` — pure/static; deterministic ordering
        (iterate input in order; sort the per-order list by client_ref so the output is stable regardless of map iteration).
- [ ] Task 6: CMake — extend `src/observability/CMakeLists.txt`
  - [ ] Add `reports.cpp` to `broker_exec_observability`; add `reports_test.cpp` to `broker_exec_observability_tests`. No new deps.
- [ ] Task 7: Tests (AC: 1, 2, 3) — `src/observability/reports_test.cpp`
  - [ ] daily: a hand-built event vector (placed×3, filled×2, rejected×1, two strategies, pnl fields) -> correct counts,
        realized_pnl_paise == the exact integer sum, orders_per_strategy correct; a non-integer pnl field is ignored (no float).
  - [ ] error: only EventType::Error events appear, in input order, with ts/client_ref; a non-error event is excluded.
  - [ ] reconciliation (AC-2): an order with placed+ack+filled+reconcile -> intended/sent/confirmed/reconciled all true, final_state
        "filled", no discrepancy; an order placed but NEVER confirmed -> intended/sent true, confirmed false -> a discrepancy
        naming it; an order confirmed but not reconciled -> a discrepancy. Totals correct.
  - [ ] reproducible (AC-3): generate each report TWICE from the same input -> byte-identical to_json() (stable ordering, no clock).
  - [ ] to_json: each report's JSON parses (nlohmann allow_exceptions=false) and carries the expected keys/values.

## Dev Notes

- **Reports are pure functions of the audit data** (AC-3) — no clock, no I/O; deterministic ordering (sort by client_ref) so the
  same events reproduce the same report. [architecture.md#FR-27 reports]
- **Reconciliation statement** (AC-2) — intended/sent/confirmed/reconciled per order + discrepancies, derived from the event
  types (OrderPlaced -> intended/sent; ack/partial/filled -> confirmed; ReconcileResult -> reconciled). [architecture.md#FR-27]
- **No float in money** — P&L summed as integer paise; a non-integer pnl field is ignored, never coerced. [docs/conventions.md]
- **Redaction-safe** — reuse the events' already-scrubbed fields; do not re-introduce a secret into a report. [Story 4.1]
- **Reuse:** `observability::AuditEvent`/`EventType`/`AuditTrail` (4.1), nlohmann, domain.

### References
- [Source: epics.md#Story 4.2] [architecture.md#FR-27 reports] [Source: include/broker_exec/observability/audit_event.hpp (4.1)]
- [Source: docs/conventions.md]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
