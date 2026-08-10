# Story 4.1: Structured logging, provenance & audit

Status: ready-for-dev

## Story

As an operator,
I want full per-order provenance in structured logs and an audit trail,
so that I can reconstruct any trade's decision path. (FR-27)

## Acceptance Criteria

1. **Given** spdlog JSON logging with the redaction formatter **When** an order flows through the system **Then**
   strategy/broker/account/all-timestamps/response/status-changes/risk-results/reconcile-result/P&L/error/override are recorded.
2. **And** a complete audit record reconstructs any executed order's decision path.
3. **And** the emitted event schema is versioned (field renames are breaking changes).

## Tasks / Subtasks

- [ ] Task 1: `observability` module (AC: all)
  - [ ] `include/broker_exec/observability/` + `src/observability/`; target `broker_exec_observability` (+ alias). Depends inward
        on `domain` (scrub + types), `ports` (ClockPort), `errors`, and links `spdlog` + `nlohmann_json` (spdlog is a NEW Conan
        dep — orchestrator pre-wired it). The redaction scrubber (`domain::scrub`, Story 2.2) binds so NO token reaches a log line.
- [ ] Task 2: Versioned event schema (AC: 1, 3)
  - [ ] `enum class EventType { OrderPlaced, OrderAcknowledged, OrderPartiallyFilled, OrderFilled, OrderRejected, OrderCancelled,
        OrderUnknown, RiskResult, ReconcileResult, Error, Override };` (+ `to_string` — STABLE names; renames are breaking [NFR-8]).
  - [ ] `struct AuditEvent { static constexpr int kSchemaVersion = 1; EventType type; std::chrono::system_clock::time_point ts{};
        std::string strategy, broker, account, client_ref, broker_order_id; nlohmann::json fields; };` — the typed columns
        (strategy/broker/account/client_ref/broker_order_id) plus a `fields` JSON object for the rest (status, response,
        risk_result, reconcile_result, pnl, error, override). `ts` defaults empty -> stamped from the ClockPort at log time.
  - [ ] `[[nodiscard]] std::string to_json_line(const AuditEvent&)`: ONE compact JSON object per line carrying
        `schema_version` (= kSchemaVersion), `ts` (ISO-8601 UTC), `type` (stable name), the typed columns, and the merged
        `fields` — snake_case keys. (This is the versioned public event contract.)
- [ ] Task 3: Redaction-bound structured logger (AC: 1)
  - [ ] `class StructuredLogger` (ctor `(std::shared_ptr<spdlog::logger> logger, const ports::ClockPort& clock)` OR
        `(spdlog::sink_ptr sink, ...)` — construct a logger from an INJECTED sink so tests capture lines without a file).
    - `void log(AuditEvent ev)`: if `ev.ts` is empty, stamp `ev.ts = clock.now_wall()`; build the JSON line via to_json_line;
      run the WHOLE rendered line through `domain::scrub` (the binding — no token-shaped string survives in ANY log output);
      emit via spdlog at a level derived from EventType (Error -> err, Override -> warn, else info). No throw.
  - [ ] The redaction binding is the load-bearing security property: a synthetic token in ANY field (typed or `fields`) is
        absent from the emitted line.
- [ ] Task 4: Audit trail / provenance reconstruction (AC: 2)
  - [ ] `class AuditTrail`: `void record(const AuditEvent&)` (append to an in-memory per-client_ref vector, preserving order);
        `[[nodiscard]] std::vector<AuditEvent> decision_path(std::string_view client_ref) const` returns every event for that
        order IN ORDER — placed -> risk -> ack -> (partial) fill / reject / unknown / reconcile — so an executed order's full
        decision path is reconstructable (AC-2). (The durable audit store is the ledger/intent-log; this is the in-memory +
        logged provenance view.) Optionally `record_and_log(ev, StructuredLogger&)`.
- [ ] Task 5: CMake (orchestrator pre-wired root add_subdirectory(src/observability) + the spdlog dep/find_package)
  - [ ] `src/observability/CMakeLists.txt`: links PUBLIC `broker_exec::domain` `broker_exec::errors` `nlohmann_json::nlohmann_json`;
        PRIVATE `spdlog::spdlog` `broker_exec::ports` warnings+sanitizers (spdlog is an impl detail of the logger .cpp).
        Test exe `broker_exec_observability_tests` ALSO links `broker_exec::clock` + `spdlog` (a capturing sink).
- [ ] Task 6: Tests (AC: 1, 2, 3) — `src/observability/observability_test.cpp` (a spdlog callback/ostream sink capturing lines; TestClock)
  - [ ] schema (AC-3): to_json_line emits `schema_version`==1, a stable `type` name, an ISO `ts`, and the typed columns +
        merged fields; parse it back with nlohmann and assert the keys/values.
  - [ ] provenance (AC-1): an AuditEvent carrying strategy/broker/account/client_ref/broker_order_id + fields (status,
        risk_result, reconcile_result, pnl, error, override) round-trips into the JSON line with all of them present.
  - [ ] REDACTION (AC-1 crux): an event with a synthetic Kite access_token in a typed field AND in `fields` -> the emitted line
        (captured from the sink) contains ZERO occurrences of the token / no token-shaped run (domain::scrub bound).
  - [ ] AUDIT TRAIL (AC-2): record OrderPlaced -> RiskResult -> OrderAcknowledged -> OrderFilled for one client_ref;
        decision_path(client_ref) returns the 4 events IN ORDER; a different client_ref's events don't bleed in.
  - [ ] ts stamping: an event with empty ts logged -> the captured line's ts equals the TestClock wall time.
  - [ ] level mapping: an Error event logs at error level; an OrderPlaced at info (assert via the sink's level if captured).

## Dev Notes

- **Redaction-on-logging** (the 2.2 follow-up): `domain::scrub` binds to the rendered line so no token-shaped string reaches a
  log/audit sink. [architecture.md#ID-2 logging, #SEC-3, #2.2 follow-up: provenance IDs through structured fields]
- **Versioned event schema** (NFR-8): `schema_version` on every line; `EventType`/field names are a public contract — renames
  are breaking. [architecture.md#NFR-8, ID-2]
- **Provenance reconstructs the decision path** (AC-2): the per-order ordered event list; the durable store is the ledger (4.4). [FR-27]
- **No float in money fields** — P&L/values are integer paise (serialize as the integer or a string, never a double). [docs/conventions.md]
- **Reuse:** `domain::scrub` (2.2), `domain` types, `ports::ClockPort`, `errors`; spdlog + nlohmann_json (deps).

### References
- [Source: epics.md#Story 4.1] [architecture.md#ID-2 structlog->spdlog, #NFR-8 schema, #FR-27, #SEC-3 redaction] [Source: include/broker_exec/domain/redaction.hpp]
- [Source: docs/conventions.md]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
