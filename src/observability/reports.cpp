#include "broker_exec/observability/reports.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

// The EXACT int64 paise carried by a `fields["pnl"]`, or nullopt when the value
// is not one and must be ignored. Two ways a money read here can lie, both of
// them silent before this helper existed:
//   * nlohmann's is_number_integer() is TRUE for value_t::number_unsigned as
//     well as number_integer, and get<std::int64_t>() on an unsigned above
//     INT64_MAX is a bare static_cast in from_json.hpp — no throw, no
//     diagnostic. A broker field of 18446744073709551615 read back as -1. So an
//     unsigned is RANGE-CHECKED IN ITS OWN TYPE before it is ever narrowed.
//   * a float/string/bool pnl is not money we can add exactly, so it is refused
//     rather than coerced (money is never a float).
// This is the rule domain::decimal_paise already applies to a 20-digit broker
// field: a value that cannot be represented in int64 paise is a REFUSAL, not a
// wraparound.
[[nodiscard]] std::optional<std::int64_t> pnl_paise(const json& value) {
  if (!value.is_number_integer()) {
    return std::nullopt;
  }
  if (value.is_number_unsigned()) {
    const auto raw = value.get<std::uint64_t>();
    if (raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return std::nullopt;
    }
    return static_cast<std::int64_t>(raw);
  }
  return value.get<std::int64_t>();
}

// `total += addend`, but ONLY if the exact sum is representable; returns false
// and leaves `total` untouched otherwise. Signed overflow is undefined
// behaviour, so the test is made on the OPERANDS BEFORE the add — never by
// inspecting an already-wrapped result. Plain arithmetic, no compiler builtin,
// so MSVC and gcc/clang take the identical path.
[[nodiscard]] bool checked_add(std::int64_t& total, std::int64_t addend) {
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
  if (addend > 0 && total > kMax - addend) {
    return false;
  }
  if (addend < 0 && total < kMin - addend) {
    return false;
  }
  total += addend;
  return true;
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
        // Keyed on the RAW strategy on purpose — redaction of this column happens
        // at OUTPUT (DailyReport::to_json), so two distinct anomalous names cannot
        // merge into one row here. Same grouping discipline as reconciliation().
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

    // Sum P&L as integer paise ONLY, and only where the value can be added
    // EXACTLY: a non-integer (float/string/...) pnl is IGNORED — never coerced
    // to a double — an integer outside int64 is IGNORED rather than narrowed
    // through a silent uint64->int64 wrap, and an addend that would overflow the
    // running total is refused rather than wrapped. Every dropped value is
    // COUNTED: `realized_pnl_paise` is documented as the EXACT sum, so a total
    // that is missing a figure has to say so instead of handing the operator a
    // short (or sign-flipped) number to reconcile against the broker statement.
    //
    // BOUNDARY, stated so it is not rediscovered as a bug: WHICH addend a
    // checked accumulator refuses depends on the running total, so a set of
    // events whose PARTIAL sums leave int64 can drop a different one under a
    // different arrival order. That needs a total near 9.2e16 rupees — input
    // already outside any real money — and the counter flags it either way. The
    // AC-3 promise (the SAME event vector always renders byte-identically) is
    // untouched; only the old wrapping `+=` pretended such input was fine.
    if (ev.fields.is_object()) {
      const auto it = ev.fields.find("pnl");
      // A `null` pnl is ABSENT, not a dropped figure — the same reading the
      // Kite adapter's numeric_text() gives a null money field. Counting it
      // would make the ignored counter cry wolf on every event whose host tags a
      // pnl key it has no value for, and a counter that cries wolf gets ignored.
      if (it != ev.fields.end() && !it->is_null()) {
        const std::optional<std::int64_t> paise = pnl_paise(*it);
        if (!paise.has_value() || !checked_add(report.realized_pnl_paise, *paise)) {
          ++report.pnl_values_ignored;
        }
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
    // PROVENANCE COLUMN (IMP-15): the client_ref is the whole point of an error
    // row — it is what joins the row to the store, the log and the intent log —
    // so a well-formed ref is carried VERBATIM. It used to be copied raw, which
    // meant a caller who stuffed a credential into the column leaked it into this
    // persisted report; `scrub_provenance_column` closes that fail-closed without
    // touching the ids. (The bare-run rule never reached this field, so nothing
    // that was legible before becomes redacted now.)
    entry.client_ref = domain::scrub_provenance_column(ev.client_ref);
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
    // PROVENANCE COLUMN (IMP-15), sanitized at OUTPUT ONLY: the grouping above
    // keys on the RAW ref, so two distinct anomalous refs can never collapse into
    // one row. A well-formed ref (the normal case) is carried verbatim into both
    // the row and its discrepancy lines — a reconciliation statement that cannot
    // name the order it is complaining about is useless to an operator.
    const std::string safe_ref = domain::scrub_provenance_column(ref);
    order.client_ref = safe_ref;
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
      report.discrepancies.push_back("client_ref " + safe_ref + ": intended but not confirmed");
    }
    // Fill-specific: only an actual FILL changes position and must be reconciled
    // against the broker. An ack/reject/cancel without a fill needs no reconcile,
    // so a placed+rejected order produces NO discrepancy; a filled-but-never-
    // reconciled order DOES.
    const bool filled = had_fill.find(ref) != had_fill.end();
    if (filled && !order.reconciled) {
      report.discrepancies.push_back("client_ref " + safe_ref + ": filled but not reconciled");
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
  // Non-zero means at least one `fields["pnl"]` could NOT be summed exactly (a
  // float/string, an integer outside int64, or an addend that would overflow)
  // and was dropped, so the total above is a PARTIAL sum. The key is ALWAYS
  // rendered: a dropped money figure that is invisible to the operator is the
  // whole defect, and an absent key would read as "nothing was dropped".
  out["pnl_values_ignored"] = pnl_values_ignored;

  // PROVENANCE COLUMN (IMP-15), sanitized at OUTPUT ONLY — the same shape the
  // reconciliation report uses, for the same reason. `strategy` is a typed column
  // that a caller fills, so a persisted report used to emit whatever was in it
  // (a pasted `access_token=...` went out verbatim AS A JSON KEY). Grouping in
  // daily() still keys on the RAW strategy, so two distinct anomalous names can
  // never collapse before they are counted; the sanitize happens here, and two
  // names that sanitize to the SAME key have their counts SUMMED rather than one
  // silently overwriting the other (which would under-report placed orders).
  //
  // std::map iterates sorted by key and nlohmann's default object is itself
  // sorted, so the rendered object stays deterministic under this remapping.
  //
  // KNOWN INTERACTION, PRE-EXISTING AND RECORDED SO IT IS NOT REDISCOVERED AS A
  // MYSTERY (IMP-19 noted it; IMP-19 did not introduce it). `token`, `secret` and
  // `apikey` are all-letter runs, so they are VALID strategy names by
  // domain::is_valid_strategy_name and survive scrub_provenance_column verbatim —
  // and config's kSecretNeedles denylist matches config KEYS, not
  // `strategies.names` VALUES, so it does not stop one either. Here the strategy
  // becomes a JSON KEY, so such a name renders as `"token":3`. That JSON is
  // correct; but running domain::scrub() over the ALREADY-RENDERED report would
  // match try_key_value on `"token":` and replace the value with the marker
  // UNQUOTED, producing `"token":***REDACTED***` — invalid JSON. Report text is
  // therefore rendered scrubbed-by-column (here) and must not be scrubbed AGAIN as
  // free-form text downstream.
  json per_strategy = json::object();
  for (const auto& [strategy, count] : orders_per_strategy) {
    const std::string safe = domain::scrub_provenance_column(strategy);
    per_strategy[safe] = per_strategy.value(safe, 0) + count;
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
