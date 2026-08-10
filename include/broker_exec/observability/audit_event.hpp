#pragma once

// broker_exec::observability::AuditEvent — the versioned, structured event that
// carries one step of an order's decision path (Story 4.1, FR-27, NFR-8).
//
// Every event renders to ONE compact JSON line (`to_json_line`) carrying a
// `schema_version`, an ISO-8601 UTC timestamp, a STABLE `type` name, the typed
// provenance columns (strategy/broker/account/client_ref/broker_order_id) and a
// free-form `fields` object for the rest (status, risk/reconcile result, P&L,
// error, override...). The event names and field names are a PUBLIC contract:
// renaming `EventType` strings or reserved keys is a BREAKING change (NFR-8).
//
// MONEY: never a float. Callers put paise as integers or strings inside
// `fields` (never a double); this module does not coerce money.
//
// This header stays pure (domain/std + nlohmann only) — no spdlog. The
// redaction binding and emission live in StructuredLogger.
//
// Cross-platform: C++20 standard library + nlohmann only. No OS APIs, no
// `#ifdef`, no localtime/strftime — UTC is formatted via std::chrono.

#include <chrono>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace broker_exec::observability {

// The kind of decision-path step. The to_string() names below are the stable
// NFR-8 observability contract; renaming a returned string is a breaking change.
enum class EventType {
  OrderPlaced,
  OrderAcknowledged,
  OrderPartiallyFilled,
  OrderFilled,
  OrderRejected,
  OrderCancelled,
  OrderUnknown,
  RiskResult,
  ReconcileResult,
  Error,
  Override
};

// Stable, dotted snake-case-ish names (e.g. "order.placed", "risk.result").
// STABLE: a rename is a breaking schema change. Never throws.
[[nodiscard]] std::string_view to_string(EventType type) noexcept;

// One step of an order's provenance. `ts` defaults empty (epoch) and is stamped
// from the injected ClockPort at log time when left unset.
struct AuditEvent {
  // Bump on any breaking change to the rendered line shape or reserved keys.
  static constexpr int kSchemaVersion = 1;

  EventType type = EventType::OrderPlaced;
  std::chrono::system_clock::time_point ts{};

  // Typed provenance columns. Emitted only when non-empty.
  std::string strategy;
  std::string broker;
  std::string account;
  std::string client_ref;
  std::string broker_order_id;

  // Everything else (status, risk_result, reconcile_result, pnl, error,
  // override...). Money values are integers or strings here — never a double.
  nlohmann::json fields = nlohmann::json::object();
};

// Render the event to ONE compact JSON line: `schema_version`, ISO-8601 UTC
// `ts`, stable `type`, the non-empty typed columns, and the merged `fields`
// (namespaced under a "fields" key so a caller key can never overwrite a
// reserved key). Compact (one line) via nlohmann's dump(). Renders with a
// non-throwing UTF-8 error handler (invalid bytes -> U+FFFD), because typed
// columns and `fields` can carry arbitrary broker/caller bytes that would
// otherwise make a strict dump() throw on invalid UTF-8; so only std::bad_alloc
// could escape (as with any std::string op). That is why StructuredLogger::log
// renders inside its try.
[[nodiscard]] std::string to_json_line(const AuditEvent& ev);

}  // namespace broker_exec::observability
