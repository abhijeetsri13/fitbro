#include "broker_exec/adapters/kite/kite_rest_client.hpp"

#include <cctype>
#include <charconv>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "broker_exec/adapters/kite/http_client.hpp"
#include "broker_exec/domain/redaction.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/secret_provider.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::adapters::kite {

using json = nlohmann::json;

namespace {

// Percent-encode for an x-www-form-urlencoded body (unreserved chars per RFC
// 3986 pass through; everything else is %XX). ASCII-only, no locale.
[[nodiscard]] std::string url_encode(std::string_view s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (const unsigned char c : s) {
    if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4U]);
      out.push_back(kHex[c & 0x0FU]);
    }
  }
  return out;
}

// Render a Kite params object as a form-encoded body. String values pass through
// as text; non-strings are compact-dumped (numbers/bools become their literal).
[[nodiscard]] std::string form_encode(const json& params) {
  if (!params.is_object()) {
    return std::string{};
  }
  std::string out;
  for (auto it = params.begin(); it != params.end(); ++it) {
    if (!out.empty()) {
      out.push_back('&');
    }
    out += url_encode(it.key());
    out.push_back('=');
    out += url_encode(it->is_string() ? it->get<std::string>() : it->dump());
  }
  return out;
}

// ── THE PATH-SEGMENT GATE: A BROKER-SUPPLIED ID IS NOT A URL (IMP-23) ───────
//
// `order_id` reaches the mutations below straight out of broker JSON — the
// adapter's `json_str(row, "order_id")` on an orderbook row, `extract_order_id()`
// on a place ack — and nothing between there and the wire constrained what it
// was. It was then spliced into the request path by raw concatenation, and
// neither layer beneath us treats a path as opaque: cpr percent-encodes only
// `request.query`, never `request.path`, and libcurl reads `?`/`#` as delimiters
// and performs RFC 3986 dot-segment removal on the rest. So an id of
// `250101000000001?variety=amo` cancelled a DIFFERENT order (`250101000000001`)
// and reported success, and an id of `../../session/token` turned the cancel into
// `DELETE /session/token` — Kite's logout endpoint — killing the trading session
// mid-day. Both fire from square_off's own cancel, i.e. from the emergency exit.
//
// TWO LAYERS, AND THE ORDER MATTERS:
//   1. REFUSE anything that is not id-shaped, BEFORE any request is issued. An id
//      we do not recognize is one we will not act on: the adapter above states
//      the threat model outright ("Kite payloads are attacker-shaped"), and the
//      dispatcher already hardens this SAME value for the intent-log sink. Two
//      sinks, one threat, and the unhardened one was the wire.
//   2. PERCENT-ENCODE the segment we do send. Redundant against layer 1, which
//      admits only bytes `url_encode` passes through unchanged (a real 15-digit
//      Kite id is therefore byte-identical), and kept exactly so that widening
//      the charset later cannot silently re-open the splice.
//
// WHY NOT `domain::is_provenance_id_shape`: it requires TWO OR MORE separated
// segments so that a bare pasted credential cannot survive it — and a Kite order
// id is ONE unbroken run of digits, which it would refuse outright. The CHARSET
// is the part worth sharing, and the charset is the part that matters here.
[[nodiscard]] bool is_path_id_shape(std::string_view value) noexcept {
  // Kite ids are 15-16 digits. The bound is loose (a broker may widen its id
  // format) but finite: a multi-kilobyte "id" is not an id, and it would be
  // spliced into a URL.
  static constexpr std::size_t kMaxPathIdChars = 64;
  if (value.empty() || value.size() > kMaxPathIdChars) {
    return false;
  }
  for (const unsigned char c : value) {
    if (std::isalnum(c) == 0 && c != '-' && c != '_') {
      return false;
    }
  }
  return true;
}

// Validate + encode ONE path segment, or refuse the whole request.
//
// Validation/DoNotRetry is the honest verdict, and it does NOT contradict
// map_http_error's "a failure that may have left a live order MUST reconcile":
// that doctrine governs requests the BROKER answered. This one never reaches the
// wire, so it cannot have left anything live, and repeating it with the same
// bytes cannot succeed — the id itself is what needs an operator.
//
// The offending value is deliberately NOT echoed into the message: it is
// unvalidated broker text on its way to a log sink.
[[nodiscard]] Result<std::string> encoded_path_segment(std::string_view value,
                                                       std::string_view what, std::string code) {
  if (!is_path_id_shape(value)) {
    std::string message = "kite: the ";
    message += what;
    message +=
        " is not id-shaped ([A-Za-z0-9_-], 1-64 bytes); it was NOT spliced into a request URL "
        "path and nothing was sent";
    return broker_exec::fail(errors::make_error(errors::ErrorCategory::Validation,
                                                errors::SuggestedAction::DoNotRetry,
                                                std::move(message), std::move(code)));
  }
  return url_encode(value);
}

// Parse a header value as a non-negative long, leaving `out` unset on failure.
void parse_long_header(const std::string& value, std::optional<long>& out) {
  long parsed = 0;
  const char* first = value.data();
  const char* last = value.data() + value.size();
  const std::from_chars_result res = std::from_chars(first, last, parsed);
  if (res.ec == std::errc{}) {
    out = parsed;
  }
}

// Lift X-RateLimit-*/Retry-After off a response so the rate limiter can consume
// them. `present` is set if any of the recognized headers appears.
[[nodiscard]] RateLimitInfo extract_rate_limit(const HttpResponse& resp) {
  RateLimitInfo info;
  if (const auto h = resp.find_header("X-RateLimit-Limit")) {
    info.present = true;
    parse_long_header(*h, info.limit);
  }
  if (const auto h = resp.find_header("X-RateLimit-Remaining")) {
    info.present = true;
    parse_long_header(*h, info.remaining);
  }
  if (const auto h = resp.find_header("Retry-After")) {
    info.present = true;
    parse_long_header(*h, info.retry_after_seconds);
  }
  return info;
}

}  // namespace

errors::Error map_http_error(const HttpResponse& resp) {
  const long status = resp.status_code;

  // Pull Kite's {status:"error", error_type, message} when the body is JSON.
  std::string error_type;
  std::string raw_message;
  {
    const json parsed = json::parse(resp.body, nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_discarded() && parsed.is_object()) {
      if (const auto it = parsed.find("error_type"); it != parsed.end() && it->is_string()) {
        error_type = it->get<std::string>();
      }
      if (const auto it = parsed.find("message"); it != parsed.end() && it->is_string()) {
        raw_message = it->get<std::string>();
      }
    }
  }

  // broker_code: short + secret-free (HTTP status, optional error_type code).
  // error_type is a Kite exception class name, but scrub it defensively so a
  // malformed/hostile response can never smuggle a token through broker_code
  // (the one Error field not otherwise scrubbed).
  std::string code = "HTTP " + std::to_string(status);
  if (!error_type.empty()) {
    code += ' ';
    code += domain::scrub(error_type);
  }

  // Always scrub the broker message before it can reach a log/Error sink.
  const std::string scrubbed = raw_message.empty() ? std::string{} : domain::scrub(raw_message);

  const auto build = [&](errors::ErrorCategory cat, errors::SuggestedAction act,
                         std::string_view fallback) {
    errors::Error e;
    e.category = cat;
    e.action = act;
    e.message = scrubbed.empty() ? std::string(fallback) : ("kite: " + scrubbed);
    e.broker_code = code;
    return e;
  };

  // Broker verdict first (error_type), then the HTTP status. Order matters: a
  // failure that may have left a live order at the broker MUST reconcile, never
  // "do not retry".
  if (error_type == "TokenException" || error_type == "PermissionException" ||
      error_type == "TwoFAException" || status == 401 || status == 403) {
    return build(errors::ErrorCategory::SessionExpired, errors::SuggestedAction::ReEstablishSession,
                 "kite: session expired or invalid; re-establish required");
  }
  if (status == 429 || error_type == "TooManyRequests") {
    if (const auto ra = resp.find_header("Retry-After")) {
      code += " Retry-After=";
      code += *ra;
    }
    return build(errors::ErrorCategory::RateLimited, errors::SuggestedAction::RetrySafe,
                 "kite: rate limited by broker");
  }
  // A 404 IS "THERE IS NO SUCH (OPEN) ORDER", AND IT NEEDS ITS OWN CATEGORY.
  //
  // Without this arm the category was unreachable on the Kite path entirely — a
  // 404 fell through to BrokerRejected/Unknown — which quietly broke a caller that
  // legitimately keys on it: `square_off` TOLERATES an `OrderNotFound` cancel
  // outcome, because "the remainder went terminal underneath us" is the state the
  // cancel was trying to reach (AC-1b). A tolerance written against a category the
  // mapper could never mint is a tolerance that never fires, so the flatten
  // aborted on precisely the outcome it was built to shrug off, leaving a filled
  // position open under a "square_off failed" error.
  //
  // The ACTION stays ReconcileFirst, not DoNotRetry: our own path constants are a
  // tier-2 assumption, so a 404 may equally mean "we asked the wrong URL", and
  // abandoning a possibly-live order on that evidence is not a risk worth taking.
  // This mirrors `errors::classify_http` and `map_kotak_error`, both of which have
  // always classified 404 exactly this way — Kite's omission was the outlier.
  if (status == 404) {
    return build(errors::ErrorCategory::OrderNotFound, errors::SuggestedAction::ReconcileFirst,
                 "kite: order or endpoint not found; reconcile required");
  }
  if (error_type == "MarginException") {
    return build(errors::ErrorCategory::InsufficientFunds, errors::SuggestedAction::DoNotRetry,
                 "kite: insufficient margin for the order");
  }
  // OrderException / NetworkException / DataException / GeneralException and any
  // 5xx all mean the order's fate is UNCERTAIN at the broker — reconcile against
  // broker truth before deciding; never a blind retry, never a "do not retry".
  if (error_type == "OrderException" || error_type == "NetworkException" ||
      error_type == "DataException" || error_type == "GeneralException" || status >= 500) {
    return build(errors::ErrorCategory::Network, errors::SuggestedAction::ReconcileFirst,
                 "kite: broker/transport error; reconcile required");
  }
  // Genuine client-input rejections (only when not one of the live-order classes
  // above): fix the request, do not repeat it.
  //
  // A VERDICT WE ACTUALLY READ IS REQUIRED BEFORE DoNotRetry (IMP-25). The second
  // disjunct used to be `error_type.empty()` — which is true precisely when the
  // body could NOT be parsed as a Kite envelope (see the discard above). So a 400
  // whose verdict we could not read was treated as MORE definitive than one we
  // could: a readable-but-unrecognized error falls to BrokerRejected/ReconcileFirst
  // below, while an UNREADABLE one earned DoNotRetry — the one action that tells
  // the dispatcher the order is dead and no reconcile is owed. That inverts the
  // doctrine stated 50 lines above, and the 404 arm's own reasoning: we refused to
  // abandon a possibly-live order there on strictly BETTER evidence than this.
  //
  // The shape is reachable: a proxy/WAF/load balancer forwards `POST
  // /orders/regular`, the gateway ACCEPTS the order, and the response is then
  // replaced with an HTML or empty 400 page. `json::parse` discards it, so neither
  // `error_type` nor `message` survives, and the intent was marked terminally
  // Rejected over an order working at the broker — which the UnknownResolver then
  // skips (it resolves only UNKNOWN), the lifecycle absorbs (terminal), and the
  // manual-intervention detector excludes on the premise that a rejected order did
  // not change the position.
  //
  // Requiring a `message` keeps every genuine Kite 400 here — Kite always sends the
  // envelope, and its InputException is matched outright by the first disjunct —
  // and routes "the broker answered and we could not read it" to the
  // Unknown/ReconcileFirst arm at the bottom, where an ambiguous mutation belongs.
  if (error_type == "InputException" ||
      (!raw_message.empty() && (status == 400 || status == 422))) {
    return build(errors::ErrorCategory::Validation, errors::SuggestedAction::DoNotRetry,
                 "kite: request rejected as invalid");
  }
  if (!error_type.empty() || !scrubbed.empty()) {
    return build(errors::ErrorCategory::BrokerRejected, errors::SuggestedAction::ReconcileFirst,
                 "kite: broker rejected the request");
  }
  return build(errors::ErrorCategory::Unknown, errors::SuggestedAction::ReconcileFirst,
               "kite: unclassified transport error");
}

KiteRestClient::KiteRestClient(const HttpClient& http, const ports::SecretProvider& secrets,
                               std::string api_key_secret_name,
                               std::string access_token_secret_name)
    : http_(http),
      secrets_(secrets),
      api_key_secret_name_(std::move(api_key_secret_name)),
      access_token_secret_name_(std::move(access_token_secret_name)) {}

Result<KiteRestClient::Headers> KiteRestClient::auth_headers() const {
  const auto api_key = secrets_.get(api_key_secret_name_);
  if (!api_key) {
    return broker_exec::fail(api_key.error());
  }
  const auto access_token = secrets_.get(access_token_secret_name_);
  if (!access_token) {
    return broker_exec::fail(access_token.error());
  }
  Headers headers;
  // Kite Connect auth: `Authorization: token <api_key>:<access_token>`. This is
  // the ONLY place the secrets are materialized; they never reach a log/Error.
  headers.emplace_back("Authorization", "token " + api_key.value() + ":" + access_token.value());
  headers.emplace_back("X-Kite-Version", "3");
  return headers;
}

Result<HttpResponse> KiteRestClient::issue(HttpRequest::Method method, const std::string& path,
                                           const std::string& body) {
  auto headers = auth_headers();
  if (!headers) {
    return broker_exec::fail(headers.error());
  }
  HttpRequest req;
  req.method = method;
  req.path = path;
  req.headers = std::move(headers.value());
  req.body = body;
  if (!body.empty()) {
    req.headers.emplace_back("Content-Type", "application/x-www-form-urlencoded");
  }
  return http_.send(req);
}

Result<json> KiteRestClient::request_json(HttpRequest::Method method, const std::string& path,
                                          std::string body) {
  auto resp = issue(method, path, body);
  if (!resp) {
    return broker_exec::fail(resp.error());
  }
  last_rate_limit_ = extract_rate_limit(resp.value());

  const HttpResponse& r = resp.value();
  if (r.status_code < 200 || r.status_code >= 300) {
    return broker_exec::fail(map_http_error(r));
  }

  const json parsed = json::parse(r.body, nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return broker_exec::fail(errors::make_error(errors::ErrorCategory::Unknown,
                                                "kite: malformed response body",
                                                "HTTP " + std::to_string(r.status_code)));
  }
  // Some Kite errors arrive with a 2xx + {status:"error"} envelope — honor it.
  if (const auto it = parsed.find("status");
      it != parsed.end() && it->is_string() && it->get<std::string>() == "error") {
    return broker_exec::fail(map_http_error(r));
  }
  const auto data = parsed.find("data");
  if (data == parsed.end()) {
    return broker_exec::fail(errors::make_error(errors::ErrorCategory::Unknown,
                                                "kite: response missing data envelope",
                                                "HTTP " + std::to_string(r.status_code)));
  }
  return *data;
}

Result<std::string> KiteRestClient::request_text(HttpRequest::Method method,
                                                 const std::string& path) {
  auto resp = issue(method, path, std::string{});
  if (!resp) {
    return broker_exec::fail(resp.error());
  }
  last_rate_limit_ = extract_rate_limit(resp.value());

  const HttpResponse& r = resp.value();
  if (r.status_code < 200 || r.status_code >= 300) {
    return broker_exec::fail(map_http_error(r));
  }
  return r.body;
}

Result<json> KiteRestClient::place_order(const json& params) {
  return request_json(HttpRequest::Method::Post, "/orders/regular", form_encode(params));
}

Result<json> KiteRestClient::modify_order(const std::string& order_id, const json& params) {
  auto segment = encoded_path_segment(order_id, "broker order id", "KITE-MODIFY-BADORDERID");
  if (!segment) {
    return broker_exec::fail(segment.error());
  }
  return request_json(HttpRequest::Method::Put, "/orders/regular/" + segment.value(),
                      form_encode(params));
}

Result<json> KiteRestClient::cancel_order(const std::string& order_id, const json& params) {
  auto segment = encoded_path_segment(order_id, "broker order id", "KITE-CANCEL-BADORDERID");
  if (!segment) {
    return broker_exec::fail(segment.error());
  }
  return request_json(HttpRequest::Method::Delete, "/orders/regular/" + segment.value(),
                      form_encode(params));
}

Result<json> KiteRestClient::orders() {
  return request_json(HttpRequest::Method::Get, "/orders", std::string{});
}

Result<json> KiteRestClient::trades() {
  return request_json(HttpRequest::Method::Get, "/trades", std::string{});
}

Result<json> KiteRestClient::positions() {
  return request_json(HttpRequest::Method::Get, "/portfolio/positions", std::string{});
}

Result<json> KiteRestClient::holdings() {
  return request_json(HttpRequest::Method::Get, "/portfolio/holdings", std::string{});
}

Result<json> KiteRestClient::margins(const std::string& segment) {
  // THE SIBLING SPLICE, closed for the same reason. This one is safe only by
  // accident of its single caller passing the literal "equity"; the guard is what
  // keeps it safe when a second caller passes something it read somewhere.
  auto encoded = encoded_path_segment(segment, "margins segment", "KITE-MARGINS-BADSEGMENT");
  if (!encoded) {
    return broker_exec::fail(encoded.error());
  }
  return request_json(HttpRequest::Method::Get, "/user/margins/" + encoded.value(), std::string{});
}

Result<std::string> KiteRestClient::instruments() {
  return request_text(HttpRequest::Method::Get, "/instruments");
}

}  // namespace broker_exec::adapters::kite
