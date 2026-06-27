#pragma once

// broker_exec::errors — the typed error taxonomy (Story 1.4, FR-25, CAP-13/14).
//
// Every failure in the library is normalized into a stable, broker-neutral
// `ErrorCategory` carrying a `SuggestedAction`. Strategy code switches on the
// category/action, never on raw broker error text. The single place that
// parses raw broker/transport responses is the `classify_*` mapping below — no
// other code path inspects broker strings.
//
// NO-THROW POLICY: fallible internal calls return `Result<T>` (see
// `broker_exec/result.hpp`), i.e. `expected<T, Error>`. Errors are values, not
// exceptions; nothing throws across the strategy-facing boundary.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <string>
#include <string_view>

namespace broker_exec::errors {

// Stable, broker-neutral failure categories. These names are part of the
// observability / cross-broker contract (CAP-13: the same condition on Kite and
// Kotak Neo must resolve to the same category). Renames are breaking changes.
enum class ErrorCategory {
  Transient,          // momentary glitch, safe to retry a read
  RateLimited,        // broker throttled us (HTTP 429 / broker rate-limit code)
  Network,            // connection/transport failure with no broker verdict
  Timeout,            // no/late response — dangerous ops must reconcile first
  Auth,               // bad/expired credentials at login
  SessionExpired,     // token no longer valid; re-establish session
  Validation,         // generic invalid request (symbol/qty/price/product/...)
  InsufficientFunds,  // margin shortfall
  RiskRejected,       // broker RMS / exchange refused on risk grounds
  NotSupported,       // capability the broker lacks (reject-only posture)
  DuplicateOrder,     // broker reports an already-seen client/order reference
  OrderNotFound,      // modify/cancel on an unknown/already-terminal order
  BrokerRejected,     // broker/exchange refused for another stated reason
  MarketClosed,       // outside session window (unless AMO configured)
  DataStale,          // a freshness gate failed (funds/instrument/calendar)
  Internal,           // a bug/invariant violation inside the library
  Unknown             // unparseable/ambiguous — mark order UNKNOWN, reconcile
};

// The fixed vocabulary of normalized actions. The runtime and strategies switch
// on this deterministically; it is the only contract they need from an error.
enum class SuggestedAction {
  RetrySafe,           // safe to retry (idempotent read) with backoff
  DoNotRetry,          // fix the input / do not repeat the request
  ReconcileFirst,      // resolve against broker truth before any decision
  BlockStrategy,       // halt this strategy; config/data/funds problem
  ReEstablishSession,  // re-login (daily establishment where no headless refresh)
  Cancel,              // cancel open orders
  SquareOff,           // flatten positions (emergency exit)
  RaiseAlert           // escalate to the operator (dead-man's-switch path)
};

// A normalized, log-safe error value. Returned (never thrown) via Result<T>.
struct Error {
  ErrorCategory category = ErrorCategory::Unknown;
  SuggestedAction action = SuggestedAction::ReconcileFirst;
  // Human-readable summary. MUST be safe to log: redaction-safe, never echoes a
  // raw response body or anything token-shaped. Keep it minimal and stable.
  std::string message;
  // Optional raw broker/transport code (e.g. an HTTP status string or a broker
  // error code) for diagnostics. Empty when there is none. This is a short
  // code, never a free-form body that could carry secrets.
  std::string broker_code;
};

// Stable, log/serialization-friendly names (NFR-8 observability contract).
// Renaming a returned string is a breaking change.
[[nodiscard]] std::string_view to_string(ErrorCategory category) noexcept;
[[nodiscard]] std::string_view to_string(SuggestedAction action) noexcept;

// The default suggested action for a category. The `classify_*` entry points
// may override this where context (e.g. timeout on a read vs a dangerous op)
// warrants, but this is the safe, broker-neutral baseline.
[[nodiscard]] SuggestedAction default_action_for(ErrorCategory category) noexcept;

// Build an Error for a category using its default action. `message` must be
// redaction-safe; `broker_code` is an optional short raw code (empty if none).
[[nodiscard]] Error make_error(ErrorCategory category, std::string message,
                               std::string broker_code = {});

// ── The single broker-text parsing point ──────────────────────────────────
// Strategy/runtime code never parses raw broker responses. These two entry
// points are the ONLY place that maps a raw transport/broker error into the
// typed taxonomy. Keep them redaction-safe: do not copy whole response bodies
// (which may carry tokens) into the Error — derive a minimal, fixed message.

// Classify a transport-layer outcome from an HTTP status + (optional) body.
// `body` is inspected for broker hints only; it is NOT stored on the Error.
[[nodiscard]] Error classify_http(int status, std::string_view body);

// Classify a broker-level error from its raw code and message. `broker_code` is
// preserved on the Error (it is a short code, not a body); `message` is used
// only to refine the category and is not echoed verbatim.
[[nodiscard]] Error classify(std::string_view broker_code, std::string_view message);

}  // namespace broker_exec::errors
