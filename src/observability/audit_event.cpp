#include "broker_exec/observability/audit_event.hpp"

#include <chrono>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace broker_exec::observability {

std::string_view to_string(EventType type) noexcept {
  switch (type) {
    case EventType::OrderPlaced:
      return "order.placed";
    case EventType::OrderAcknowledged:
      return "order.acknowledged";
    case EventType::OrderPartiallyFilled:
      return "order.partially_filled";
    case EventType::OrderFilled:
      return "order.filled";
    case EventType::OrderRejected:
      return "order.rejected";
    case EventType::OrderCancelled:
      return "order.cancelled";
    case EventType::OrderUnknown:
      return "order.unknown";
    case EventType::RiskResult:
      return "risk.result";
    case EventType::ReconcileResult:
      return "reconcile.result";
    case EventType::Error:
      return "error";
    case EventType::Override:
      return "override";
  }
  // Unreachable for a valid enumerator; a stable fallback keeps the contract
  // total without throwing across the logging boundary.
  return "unknown";
}

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

// Append a typed provenance column only when it carries a value, so empty
// columns never bloat the line nor imply a known-blank value.
void put_if_set(json& out, std::string_view key, const std::string& value) {
  if (!value.empty()) {
    out[std::string(key)] = value;
  }
}

}  // namespace

std::string to_json_line(const AuditEvent& ev) {
  json out = json::object();
  out["schema_version"] = AuditEvent::kSchemaVersion;
  out["ts"] = to_iso8601_utc(ev.ts);
  out["type"] = std::string(to_string(ev.type));

  put_if_set(out, "strategy", ev.strategy);
  put_if_set(out, "broker", ev.broker);
  put_if_set(out, "account", ev.account);
  put_if_set(out, "client_ref", ev.client_ref);
  put_if_set(out, "broker_order_id", ev.broker_order_id);

  // Namespace the caller's free-form columns under "fields" so a `fields` key
  // can never collide with / overwrite a reserved top-level key. Only emit the
  // object when it actually carries values.
  if (ev.fields.is_object() && !ev.fields.empty()) {
    out["fields"] = ev.fields;
  }

  // Compact, single-line JSON (indent -1 -> no newlines). Render with the
  // non-throwing UTF-8 error handler: arbitrary broker/caller bytes (e.g. a
  // Latin-1 reject reason) would otherwise make dump() throw json::type_error.316
  // on invalid UTF-8; `replace` substitutes the U+FFFD replacement char instead,
  // so only std::bad_alloc can escape across the logging boundary.
  return out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace broker_exec::observability
