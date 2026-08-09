#pragma once

// broker_exec::observability::ReportGenerator — end-of-session reports
// (Story 4.2, FR-27, AC-1/2/3).
//
// Three reports are derived as PURE functions of the recorded audit data (a
// std::vector<AuditEvent> from Story 4.1): a DailyReport (counts + integer
// P&L + per-strategy order counts), an ErrorReport (every Error event, in
// input order, with a redaction-safe detail), and a ReconciliationReport (the
// intended/sent/confirmed/reconciled-vs-broker statement plus discrepancies).
//
// PURE & DETERMINISTIC (AC-3): no clock, no I/O; the same events always yield a
// byte-identical to_json(). Per-order output is sorted by client_ref so the
// result never depends on map/iteration order.
//
// MONEY: never a float. P&L is summed as integer paise (int64); a non-integer
// `fields["pnl"]` value is IGNORED, never coerced to a double.
//
// REDACTION: the in-memory `AuditEvent.fields` are stored RAW — domain::scrub
// runs only on the rendered log line in StructuredLogger (Story 4.1), NOT on the
// in-memory event. So any report that renders free-form `fields`-derived text
// (only the ErrorReport detail does) must scrub that text itself before it
// reaches an observable/persisted sink; ErrorReport does so via domain::scrub.
//
// PROVENANCE COLUMNS (IMP-15): every rendered `client_ref` — the error rows, the
// reconciliation rows and the discrepancy strings — goes through
// `domain::scrub_provenance_column`, so a well-formed ref is carried VERBATIM
// (a report that cannot name the order it is about is useless) while an
// anomalous value in that column is scrubbed (fail closed; the column used to be
// copied raw). The daily report's `orders_per_strategy` KEYS go through the same
// helper. Every one of these GROUPS ON THE RAW VALUE and sanitizes only at
// output, so two distinct anomalous values can never merge into one row before
// they are counted; where two of them do sanitize to the same rendered key, the
// counts are SUMMED, never overwritten.
//
// Cross-platform: C++20 standard library + nlohmann only. No OS APIs, no
// `#ifdef`, no localtime/strftime — UTC is formatted via std::chrono.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "broker_exec/observability/audit_event.hpp"

namespace broker_exec::observability {

// End-of-session tally (AC-1). Counts are per EventType occurrence;
// `realized_pnl_paise` is the exact integer sum of every integer `fields["pnl"]`
// (non-integer pnl values are ignored). `orders_per_strategy` counts placed
// orders per strategy (counted on OrderPlaced to avoid double counting a single
// order across its lifecycle).
struct DailyReport {
  int orders_placed = 0;
  int filled = 0;
  int rejected = 0;
  int cancelled = 0;
  int unknown = 0;
  std::int64_t realized_pnl_paise = 0;
  std::map<std::string, int> orders_per_strategy;

  [[nodiscard]] std::string to_json() const;
};

// One error row: an ISO-8601 UTC timestamp, the order's client_ref, and a
// detail drawn from the event's RAW `fields` and then run through domain::scrub
// (the in-memory fields are not pre-scrubbed), so no token reaches this report.
struct ErrorReportEntry {
  std::string ts;
  std::string client_ref;
  std::string detail;
};

// Every EventType::Error event, in input order (AC-1).
struct ErrorReport {
  std::vector<ErrorReportEntry> errors;

  [[nodiscard]] std::string to_json() const;
};

// Per-order reconciliation flags (AC-2). Mapping: `intended`/`sent` = an
// OrderPlaced exists (placing IS the send); `confirmed` = ANY broker verdict
// exists (Acknowledged | PartiallyFilled | Filled | OrderRejected |
// OrderCancelled) — a reject/cancel is the broker confirming the outcome, not a
// silent gap; `reconciled` = a ReconcileResult exists. `final_state` is the
// stable name of the last terminal/known state event seen for the order
// ("order.filled", "order.rejected", "order.cancelled", "order.unknown"), or
// "open" if none.
struct OrderReconciliation {
  std::string client_ref;
  bool intended = false;
  bool sent = false;
  bool confirmed = false;
  bool reconciled = false;
  std::string final_state;
};

// The intended-vs-sent-vs-confirmed-vs-reconciled-vs-broker statement (AC-2).
// `orders` is sorted by client_ref; totals are sums across orders;
// `discrepancies` names the client_ref of each gap, ordered by client_ref.
struct ReconciliationReport {
  std::vector<OrderReconciliation> orders;
  int intended = 0;
  int sent = 0;
  int confirmed = 0;
  int reconciled = 0;
  std::vector<std::string> discrepancies;

  [[nodiscard]] std::string to_json() const;
};

// Pure, static report builders over recorded AuditEvents (Story 4.1).
// Deterministic and non-throwing across the boundary.
class ReportGenerator {
 public:
  [[nodiscard]] static DailyReport daily(const std::vector<AuditEvent>& events);
  [[nodiscard]] static ErrorReport errors(const std::vector<AuditEvent>& events);
  [[nodiscard]] static ReconciliationReport reconciliation(const std::vector<AuditEvent>& events);
};

}  // namespace broker_exec::observability
