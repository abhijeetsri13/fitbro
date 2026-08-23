#pragma once

// broker_exec::adapters::kotak::KotakRestClient — the Kotak Neo REST protocol
// over the HttpClient transport seam (Story 6.1, AC-1/AC-2, FR-25).
//
// This is the TRANSPORT beneath a broker adapter, not a `ports::BrokerPort`
// impl: it speaks the Kotak Neo wire format (multi-header auth, the
// `jData=` form body, the `{"stat":...}` envelope, rate-limit headers) and
// returns the parsed payload (`nlohmann::json`) or a typed, secret-scrubbed
// `errors::Error`. The surface deliberately MIRRORS `KiteRestClient` so the
// adapter above can be written against one shape for both brokers (CAP-13).
//
// CREDENTIALS. The client holds NO session material. It takes a
// `BundleProvider` — a callable that yields the current `KotakSessionBundle`
// (normally `[&est]{ return est.load(); }`, i.e. straight from the encrypted
// TokenStore) — and invokes it PER CALL. The bundle's fields are materialized
// into the outgoing headers at the call site and discarded when the request
// returns; nothing is cached on this object, logged, or copied into an Error.
//
// KOTAK ENVELOPE, THE TRAP: Kotak commonly answers a REJECTED order with
// **HTTP 200 and `{"stat":"Not_Ok"}`**. Every response therefore goes through
// `is_kotak_success` (envelope first, status second) before its payload is
// trusted — see kotak_errors.hpp for the mapping contract.
//
// PAYLOAD SHAPE: reads answer `{"stat":"Ok","data":[...]}`; mutations inline
// their result (`{"stat":"Ok","nOrdNo":"..."}`). The client returns `data` when
// present and otherwise the whole envelope object, so a caller always receives
// the broker's payload without re-parsing the envelope.
//
// MUTATIONS NEVER COME BACK "SAFE TO RETRY": place/modify/cancel are tagged as
// mutations, and any RetrySafe verdict from the error mapper (notably a 429) is
// downgraded to ReconcileFirst on them. The mapper sees only text and status; it
// cannot know the call was a write, and re-sending a write that may already have
// reached the exchange is the duplicate-order hazard this library exists to
// prevent.
//
// NO-THROW: every method returns `Result<T>`; nothing throws across the
// boundary. `nlohmann::json` appears only as a forward declaration here.
//
// NO FLOAT: this layer never converts a price/quantity — JSON values are passed
// through verbatim to the adapter, which owns the paise-integer conversion.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "broker_exec/adapters/kotak/kotak_session.hpp"
#include "broker_exec/adapters/kotak/kotak_transport.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::adapters::kotak {

// The rate-limit signal lifted from a response's headers so the rate limiter
// (Story 2.12) can throttle. Where Kotak sends X-RateLimit-*/Retry-After we
// surface them; otherwise the fields stay empty.
struct RateLimitInfo {
  bool present = false;                     // any rate-limit/Retry-After header seen
  std::optional<long> limit;                // X-RateLimit-Limit
  std::optional<long> remaining;            // X-RateLimit-Remaining
  std::optional<long> retry_after_seconds;  // Retry-After (notably on a 429)
};

// Supplies the CURRENT session bundle. Invoked once per request so a re-
// established session is picked up without rebuilding the client, and so no
// credential is ever cached here.
using BundleProvider = std::function<Result<KotakSessionBundle>()>;

class KotakRestClient {
 public:
  KotakRestClient(const HttpClient& http, BundleProvider bundle_provider);

  // ── Orders (mutations) ──
  // `params` is a Kotak order field object; it is sent as the `jData=<json>`
  // form body Kotak's quick-order endpoints expect.
  [[nodiscard]] Result<nlohmann::json> place_order(const nlohmann::json& params);
  [[nodiscard]] Result<nlohmann::json> modify_order(const nlohmann::json& params);
  [[nodiscard]] Result<nlohmann::json> cancel_order(const nlohmann::json& params);

  // ── Reads (idempotent) ──
  [[nodiscard]] Result<nlohmann::json> orders();  // order book
  [[nodiscard]] Result<nlohmann::json> trades();  // trade book
  [[nodiscard]] Result<nlohmann::json> positions();
  [[nodiscard]] Result<nlohmann::json> holdings();
  // Funds/limits. `params` selects the segment/exchange/product (Kotak takes
  // these as a jData body, not a path segment as Kite does).
  [[nodiscard]] Result<nlohmann::json> margins(const nlohmann::json& params);

  // The scrip (instrument) master is a text/CSV body, not a JSON envelope.
  [[nodiscard]] Result<std::string> scrip_master();

  // The rate-limit signal from the most recent response (empty before any call).
  [[nodiscard]] const RateLimitInfo& rate_limit() const noexcept { return last_rate_limit_; }

 private:
  using Headers = std::vector<std::pair<std::string, std::string>>;

  // One request through the transport. Resolves the bundle, builds the auth
  // headers at the call site, and (for order endpoints) attaches `sId`.
  [[nodiscard]] Result<HttpResponse> issue(HttpRequest::Method method, const std::string& path,
                                           const std::string& body, bool with_server_id);

  // JSON path: non-success envelope/status -> typed Error; success -> `data`
  // when present, else the whole envelope object. Updates the rate-limit signal.
  //
  // `is_mutation` is the DUPLICATE-ORDER GUARD: the error mapper classifies text
  // and status alone and cannot see read-vs-write, so on a place/modify/cancel
  // this downgrades a RetrySafe verdict (notably a 429) to ReconcileFirst.
  [[nodiscard]] Result<nlohmann::json> request_json(HttpRequest::Method method,
                                                    const std::string& path, std::string body,
                                                    bool with_server_id, bool is_mutation);

  // Text path (scrip master): a non-2xx status OR a body that parses as a Kotak
  // FAILURE envelope -> typed Error; a genuine CSV body -> raw text.
  [[nodiscard]] Result<std::string> request_text(HttpRequest::Method method,
                                                 const std::string& path);

  const HttpClient& http_;
  BundleProvider bundle_provider_;
  RateLimitInfo last_rate_limit_;
};

}  // namespace broker_exec::adapters::kotak
