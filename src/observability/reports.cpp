#include "broker_exec/observability/reports.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "broker_exec/domain/redaction.hpp"
#include "broker_exec/observability/audit_event.hpp"

namespace broker_exec::observability {

namespace {

using json = nlohmann::json;

// Left-pad a non-negative integer to `width` with '0' (ISO timestamp parts).
// Takes long long so chrono's `rep` (64-bit) never narrows under MSVC /W4 /WX.
[[nodiscard]] std::string pad(long long value, std::size_t width) {
  std::string digits = std::to_string(value);
  while (digits.size() < width) {
    digits.insert(digits.begin(), '0');
  }
  return digits;
}

// Format a wall-clock instant as ISO-8601 UTC: "YYYY-MM-DDTHH:MM:SSZ". UTC is
// derived purely from std::chrono — NO localtime/strftime, NO `#ifdef`, no
// timezone database — so the rendered timestamp is identical on every platform.
// (Mirrors audit_event.cpp's internal formatter; kept local to avoid widening
// that file's API for one helper.)
[[nodiscard]] std::string to_iso8601_utc(std::chrono::system_clock::time_point tp) {
  using namespace std::chrono;
  const sys_days day = floor<days>(tp);
  const year_month_day ymd{day};
  const hh_mm_ss<seconds> tod{floor<seconds>(tp - day)};

  const long long year = static_cast<long long>(static_cast<int>(ymd.year()));
  const long long month = static_cast<long long>(static_cast<unsigned>(ymd.month()));
  const long long dom = static_cast<long long>(static_cast<unsigned>(ymd.day()));

  return pad(year, 4) + '-' + pad(month, 2) + '-' + pad(dom, 2) + 'T' +
         pad(static_cast<long long>(tod.hours().count()), 2) + ':' +
         pad(static_cast<long long>(tod.minutes().count()), 2) + ':' +
         pad(static_cast<long long>(tod.seconds().count()), 2) + 'Z';
}

// Compact, single-line JSON with the non-throwing UTF-8 error handler: typed
// columns and scrubbed `fields` can carry arbitrary broker/caller bytes that
// would otherwise make a strict dump() throw on invalid UTF-8; `replace`
// substitutes U+FFFD instead, so only std::bad_alloc can escape.
[[nodiscard]] std::string compact_dump(const json& value) {
  return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace

DailyReport ReportGenerator::daily(const std::vector<AuditEvent>& events) {
  DailyReport report;
  for (const AuditEvent& ev : events) {
    switch (ev.type) {
      case EventType::OrderPlaced:
        ++report.orders_placed;
        // Per-strategy counts are taken on OrderPlaced ONLY: an order emits many
        // lifecycle events (ack/fill/...), so counting on placement gives one
        // count per order instead of double counting the same order's strategy.
        if (!ev.strategy.empty()) {
          ++report.orders_per_strategy[ev.strategy];
        }
        break;
      case EventType::OrderFilled:
        ++report.filled;
        break;
      case EventType::OrderRejected:
        ++report.rejected;
        break;
      case EventType::OrderCancelled:
        ++report.cancelled;
        break;
      case EventType::OrderUnknown:
        ++report.unknown;
        break;
      default:
        break;
    }

    // Sum P&L as integer paise ONLY. A non-integer (float/string/...) pnl is
    // IGNORED — never coerced to a double (money is never a float).
    if (ev.fields.is_object()) {
      const auto it = ev.fields.find("pnl");
      if (it != ev.fields.end() && it->is_number_integer()) {
        report.realized_pnl_paise += it->get<std::int64_t>();
      }
    }
  }
  return report;
}

ErrorReport ReportGenerator::errors(const std::vector<AuditEvent>& events) {
  ErrorReport report;
  for (const AuditEvent& ev : events) {
    if (ev.type != EventType::Error) {
      continue;
    }
    ErrorReportEntry entry;
    entry.ts = to_iso8601_utc(ev.ts);
    entry.client_ref = ev.client_ref;
    // The in-memory `fields` are stored RAW — domain::scrub runs only on the
    // rendered log line in StructuredLogger (Story 4.1), NOT on the in-memory
    // event. So a "message"/fields value can carry a token-shaped secret; the
    // rendered detail is scrubbed HERE via domain::scrub before it reaches this
    // report's to_json() (an observable/persisted sink). Prefer a "message"
    // string when present, else the compact dump of fields.
    if (ev.fields.is_object()) {
      const auto it = ev.fields.find("message");
      if (it != ev.fields.end() && it->is_string()) {
        entry.detail = domain::scrub(it->get<std::string>());
      } else if (!ev.fields.empty()) {
        entry.detail = domain::scrub(compact_dump(ev.fields));
      }
    }
    report.errors.push_back(std::move(entry));
  }
  return report;
}

ReconciliationReport ReportGenerator::reconciliation(const std::vector<AuditEvent>& events) {
  // Accumulate per client_ref in a std::map so iteration (and thus output) is
  // sorted by client_ref — deterministic regardless of input arrival order.
  std::map<std::string, OrderReconciliation> by_ref;
  // Tracks whether the order ever actually filled (partial or full). A fill
  // changes position and so MUST be reconciled; an ack/reject/cancel without a
  // fill needs no reconcile. Kept beside by_ref (not on the struct, whose public
  // fields are fixed) so the fill-specific discrepancy rule can consult it.
  std::map<std::string, bool> had_fill;

  for (const AuditEvent& ev : events) {
    if (ev.client_ref.empty()) {
      continue;  // un-keyed events cannot be reconciled to an order.
    }
    OrderReconciliation& order = by_ref[ev.client_ref];
    order.client_ref = ev.client_ref;

    switch (ev.type) {
      case EventType::OrderPlaced:
        order.intended = true;
        order.sent = true;  // placing IS the send.
        break;
      case EventType::OrderAcknowledged:
      case EventType::OrderPartiallyFilled:
      case EventType::OrderFilled:
      case EventType::OrderRejected:
      case EventType::OrderCancelled:
        // ANY broker verdict CONFIRMS the order's outcome: an ack, a fill, but
        // also a reject or a cancel (the broker has definitively spoken). Only an
        // OrderPlaced with NO verdict at all is a silent/UNKNOWN gap.
        order.confirmed = true;
        break;
      case EventType::ReconcileResult:
        order.reconciled = true;
        break;
      default:
        break;
    }

    // A fill (partial or full) moved position and must be reconciled.
    if (ev.type == EventType::OrderPartiallyFilled || ev.type == EventType::OrderFilled) {
      had_fill[ev.client_ref] = true;
    }

    // final_state tracks the LAST terminal/known state event seen for the order.
    switch (ev.type) {
      case EventType::OrderFilled:
      case EventType::OrderRejected:
      case EventType::OrderCancelled:
      case EventType::OrderUnknown:
        order.final_state = std::string(to_string(ev.type));
        break;
      default:
        break;
    }
  }

  ReconciliationReport report;
  for (auto& [ref, order] : by_ref) {
    if (order.final_state.empty()) {
      order.final_state = "open";
    }
    report.intended += order.intended ? 1 : 0;
    report.sent += order.sent ? 1 : 0;
    report.confirmed += order.confirmed ? 1 : 0;
    report.reconciled += order.reconciled ? 1 : 0;

    // Discrepancies (AC-2), emitted in client_ref order (the map is sorted):
    // "intended but not confirmed" now means a TRULY SILENT order — placed, but
    // the broker never returned ANY verdict (ack/fill/reject/cancel) — the
    // dangerous possible-UNKNOWN gap. A clean reject/cancel is confirmed, so it
    // is NOT flagged here.
    if (order.intended && !order.confirmed) {
      report.discrepancies.push_back("client_ref " + ref + ": intended but not confirmed");
    }
    // Fill-specific: only an actual FILL changes position and must be reconciled
    // against the broker. An ack/reject/cancel without a fill needs no reconcile,
    // so a placed+rejected order produces NO discrepancy; a filled-but-never-
    // reconciled order DOES.
    const bool filled = had_fill.find(ref) != had_fill.end();
    if (filled && !order.reconciled) {
      report.discrepancies.push_back("client_ref " + ref + ": filled but not reconciled");
    }

    report.orders.push_back(order);
  }
  return report;
}

std::string DailyReport::to_json() const {
  json out = json::object();
  out["orders_placed"] = orders_placed;
  out["filled"] = filled;
  out["rejected"] = rejected;
  out["cancelled"] = cancelled;
  out["unknown"] = unknown;
  out["realized_pnl_paise"] = realized_pnl_paise;  // int64 paise — never a float.

  // std::map iterates sorted by key, so the rendered object is deterministic.
  json per_strategy = json::object();
  for (const auto& [strategy, count] : orders_per_strategy) {
    per_strategy[strategy] = count;
  }
  out["orders_per_strategy"] = std::move(per_strategy);

  return compact_dump(out);
}

std::string ErrorReport::to_json() const {
  json out = json::object();
  json arr = json::array();
  for (const ErrorReportEntry& entry : errors) {
    json row = json::object();
    row["ts"] = entry.ts;
    row["client_ref"] = entry.client_ref;
    row["detail"] = entry.detail;
    arr.push_back(std::move(row));
  }
  out["errors"] = std::move(arr);
  return compact_dump(out);
}

std::string ReconciliationReport::to_json() const {
  json out = json::object();

  json arr = json::array();
  for (const OrderReconciliation& order : orders) {
    json row = json::object();
    row["client_ref"] = order.client_ref;
    row["intended"] = order.intended;
    row["sent"] = order.sent;
    row["confirmed"] = order.confirmed;
    row["reconciled"] = order.reconciled;
    row["final_state"] = order.final_state;
    arr.push_back(std::move(row));
  }
  out["orders"] = std::move(arr);

  out["intended"] = intended;
  out["sent"] = sent;
  out["confirmed"] = confirmed;
  out["reconciled"] = reconciled;

  json disc = json::array();
  for (const std::string& d : discrepancies) {
    disc.push_back(d);
  }
  out["discrepancies"] = std::move(disc);

  return compact_dump(out);
}

}  // namespace broker_exec::observability
