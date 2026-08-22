#pragma once

// broker_exec::adapters::kite::KiteRestClient — the Kite Connect REST protocol
// over the HttpClient transport seam (Story 2.3, AC-1/AC-2).
//
// This is the TRANSPORT beneath a broker adapter, not a `ports::BrokerPort`
// impl: it speaks Kite Connect v3 (auth headers, the `{status,data}` envelope,
// rate-limit headers) and returns the parsed `data` payload (`nlohmann::json`)
// or a typed, secret-scrubbed `errors::Error`. The adapter above translates that
// into domain types for the core.
//
// CREDENTIALS: the api_key and the daily access_token are fetched lazily by name
// through `ports::SecretProvider`; they are placed ONLY into the outgoing
// `Authorization: token <api_key>:<access_token>` header and are NEVER stored on
// this object, logged, or copied into any Error.
//
// NO-THROW POLICY: every method returns `Result<T>`; nothing throws across the
// boundary. `nlohmann::json` appears only as a forward declaration here (the
// full type is needed only by the .cpp and by callers that consume the result).
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "broker_exec/adapters/kite/http_client.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/secret_provider.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::adapters::kite {

// The rate-limit signal lifted from a response's headers so the rate limiter
// (Story 2.12) can throttle. Kite REST is non-paginated; where it sends
// X-RateLimit-*/Retry-After we surface them, otherwise the fields stay empty.
struct RateLimitInfo {
  bool present = false;                     // any rate-limit/Retry-After header seen
  std::optional<long> limit;                // X-RateLimit-Limit
  std::optional<long> remaining;            // X-RateLimit-Remaining
  std::optional<long> retry_after_seconds;  // Retry-After (notably on a 429)
};

// Map an HTTP/Kite error response to the typed taxonomy (the single Kite
// error-parsing point). The Kite `message` is run through `domain::scrub` before
// it is placed on the Error; the Authorization header/token is never touched.
//   401/403 or TokenException/PermissionException -> SessionExpired / ReEstablishSession
//   429                                           -> RateLimited   / RetrySafe (+Retry-After)
//   5xx                                           -> Network       / ReconcileFirst
//   400/422 or NetworkException                   -> Validation    / DoNotRetry
[[nodiscard]] errors::Error map_http_error(const HttpResponse& response);

// The Kite Connect v3 REST client. Construct with the transport, the secret
// provider, and the logical secret names for the api_key and daily access_token.
class KiteRestClient {
 public:
  KiteRestClient(const HttpClient& http, const ports::SecretProvider& secrets,
                 std::string api_key_secret_name, std::string access_token_secret_name);

  // ── Orders (mutations) ──
  // `params` is a Kite order field object (form-encoded for the wire). Returns
  // the parsed `data` payload (e.g. `{ "order_id": "..." }`).
  [[nodiscard]] Result<nlohmann::json> place_order(const nlohmann::json& params);
  [[nodiscard]] Result<nlohmann::json> modify_order(const std::string& order_id,
                                                    const nlohmann::json& params);
  [[nodiscard]] Result<nlohmann::json> cancel_order(const std::string& order_id,
                                                    const nlohmann::json& params);

  // ── Reads (idempotent) ──
  [[nodiscard]] Result<nlohmann::json> orders();  // orderbook
  [[nodiscard]] Result<nlohmann::json> trades();  // tradebook
  [[nodiscard]] Result<nlohmann::json> positions();
  [[nodiscard]] Result<nlohmann::json> holdings();
  [[nodiscard]] Result<nlohmann::json> margins(const std::string& segment);

  // The instruments dump is a CSV body (not a JSON envelope) — returned as text.
  [[nodiscard]] Result<std::string> instruments();

  // The rate-limit signal from the most recent response (empty before any call).
  [[nodiscard]] const RateLimitInfo& rate_limit() const noexcept { return last_rate_limit_; }

 private:
  using Headers = std::vector<std::pair<std::string, std::string>>;

  // Build the Kite auth headers, fetching both secrets lazily. A missing secret
  // is surfaced as the provider's (secret-free) Error.
  [[nodiscard]] Result<Headers> auth_headers() const;

  // Issue one request through the transport (adds auth + content-type headers).
  [[nodiscard]] Result<HttpResponse> issue(HttpRequest::Method method, const std::string& path,
                                           const std::string& body);

  // JSON-envelope path: non-2xx / `status:"error"` / malformed -> typed Error;
  // success -> the `data` payload. Updates the rate-limit signal.
  [[nodiscard]] Result<nlohmann::json> request_json(HttpRequest::Method method,
                                                    const std::string& path, std::string body);

  // Text path (instruments CSV): non-2xx -> typed Error; success -> raw body.
  [[nodiscard]] Result<std::string> request_text(HttpRequest::Method method,
                                                 const std::string& path);

  const HttpClient& http_;
  const ports::SecretProvider& secrets_;
  std::string api_key_secret_name_;
  std::string access_token_secret_name_;
  RateLimitInfo last_rate_limit_;
};

}  // namespace broker_exec::adapters::kite
