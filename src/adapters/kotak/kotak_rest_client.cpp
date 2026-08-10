#include "broker_exec/adapters/kotak/kotak_rest_client.hpp"

#include <cctype>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "broker_exec/adapters/kotak/kotak_errors.hpp"
#include "broker_exec/adapters/kotak/kotak_session.hpp"
#include "broker_exec/adapters/kotak/kotak_transport.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::adapters::kotak {

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

// Kotak's quick endpoints take the whole request object as ONE form field:
//   jData=<url-encoded compact json>
// Built through the JSON writer so no field value can break out of the frame.
//
// `error_handler_t::replace` is REQUIRED, not stylistic: the default handler
// THROWS on a non-UTF-8 byte, and these params can carry broker- or
// scrip-master-derived text. A throw here would cross the no-throw boundary.
[[nodiscard]] std::string j_data_body(const json& params) {
  // dump(indent, indent_char, ensure_ascii=false, replace-on-invalid-UTF-8).
  static constexpr auto kReplace = json::error_handler_t::replace;
  const std::string json_text =
      params.is_null() ? std::string("{}") : params.dump(-1, ' ', false, kReplace);
  return "jData=" + url_encode(json_text);
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

[[nodiscard]] RateLimitInfo extract_rate_limit(const HttpResponse& response) {
  RateLimitInfo info;
  if (const auto header = response.find_header("X-RateLimit-Limit")) {
    info.present = true;
    parse_long_header(*header, info.limit);
  }
  if (const auto header = response.find_header("X-RateLimit-Remaining")) {
    info.present = true;
    parse_long_header(*header, info.remaining);
  }
  if (const auto header = response.find_header("Retry-After")) {
    info.present = true;
    parse_long_header(*header, info.retry_after_seconds);
  }
  return info;
}

constexpr std::string_view kFormContentType = "application/x-www-form-urlencoded";

}  // namespace

KotakRestClient::KotakRestClient(const HttpClient& http, BundleProvider bundle_provider)
    : http_(http), bundle_provider_(std::move(bundle_provider)) {}

Result<HttpResponse> KotakRestClient::issue(HttpRequest::Method method, const std::string& path,
                                            const std::string& body, bool with_server_id) {
  if (!bundle_provider_) {
    return broker_exec::fail(errors::make_error(errors::ErrorCategory::Internal,
                                                "kotak: no session bundle provider configured"));
  }
  // Resolved PER CALL and dropped when this function returns — the client never
  // holds session material.
  auto bundle = bundle_provider_();
  if (!bundle) {
    return broker_exec::fail(bundle.error());
  }
  if (!bundle.value().complete()) {
    errors::Error error = KotakSessionEstablisher::needs_reauth_error();
    error.message = "kotak: session bundle incomplete; re-establish required";
    return broker_exec::fail(std::move(error));
  }

  HttpRequest request;
  request.method = method;
  request.path = path;
  request.headers = auth_headers(bundle.value());
  if (with_server_id) {
    // Kotak routes order traffic by the login-issued server id.
    request.query.emplace_back("sId", bundle.value().hs_server_id);
  }
  if (!body.empty()) {
    request.headers.emplace_back("Content-Type", std::string(kFormContentType));
    request.body = body;
  }
  return http_.send(request);
}

Result<json> KotakRestClient::request_json(HttpRequest::Method method, const std::string& path,
                                           std::string body, bool with_server_id,
                                           bool is_mutation) {
  // A stale signal is worse than none: clear it before the call so a caller can
  // never read the PREVIOUS response's Retry-After after a header-less failure.
  last_rate_limit_ = RateLimitInfo{};

  // THE DUPLICATE-ORDER GUARD. `brokerreason`/`map_kotak_error` classify text and
  // status alone; they cannot see whether the failed call was a read or a write.
  // RetrySafe on a PLACE/MODIFY/CANCEL would re-send a request that may already
  // have reached the exchange, so on a mutation it is downgraded to reconcile.
  const auto guard_mutation = [is_mutation](errors::Error error) {
    if (is_mutation && error.action == errors::SuggestedAction::RetrySafe) {
      error.action = errors::SuggestedAction::ReconcileFirst;
    }
    return error;
  };

  auto response = issue(method, path, body, with_server_id);
  if (!response) {
    return broker_exec::fail(guard_mutation(response.error()));
  }
  last_rate_limit_ = extract_rate_limit(response.value());

  const HttpResponse& r = response.value();
  // Envelope FIRST, status second: an HTTP 200 `stat:"Not_Ok"` is a rejection.
  if (!is_kotak_success(r)) {
    return broker_exec::fail(guard_mutation(map_kotak_error(r)));
  }

  const json parsed = json::parse(r.body, nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded()) {
    return broker_exec::fail(errors::make_error(errors::ErrorCategory::Unknown,
                                                "kotak: malformed response body",
                                                "HTTP " + std::to_string(r.status_code)));
  }
  if (parsed.is_object()) {
    if (const auto data = parsed.find("data"); data != parsed.end()) {
      return *data;
    }
    // Mutations inline their result beside `stat` — hand back the whole object.
    return parsed;
  }
  if (parsed.is_array()) {
    return parsed;  // a few reads answer with a bare array
  }
  return broker_exec::fail(errors::make_error(errors::ErrorCategory::Unknown,
                                              "kotak: unexpected response shape",
                                              "HTTP " + std::to_string(r.status_code)));
}

Result<std::string> KotakRestClient::request_text(HttpRequest::Method method,
                                                  const std::string& path) {
  last_rate_limit_ = RateLimitInfo{};

  auto response = issue(method, path, std::string{}, /*with_server_id=*/false);
  if (!response) {
    return broker_exec::fail(response.error());
  }
  last_rate_limit_ = extract_rate_limit(response.value());

  const HttpResponse& r = response.value();
  if (r.status_code < 200 || r.status_code >= 300) {
    return broker_exec::fail(map_kotak_error(r));
  }
  // A status check ALONE is not enough even on the text path: Kotak answers this
  // endpoint's failures with an HTTP 200 error ENVELOPE, and handing that body
  // back as instrument data would feed a JSON error string to the scrip parser.
  // A genuine CSV body does not parse as JSON, so it sails through untouched.
  const KotakEnvelope envelope = parse_kotak_envelope(r.body);
  if (envelope.parsed &&
      (envelope.has_fault || envelope.has_error || (envelope.has_stat && !envelope.stat_ok))) {
    return broker_exec::fail(map_kotak_error(r));
  }
  return r.body;
}

Result<json> KotakRestClient::place_order(const json& params) {
  return request_json(HttpRequest::Method::Post, std::string(endpoints::kPlaceOrder),
                      j_data_body(params), /*with_server_id=*/true, /*is_mutation=*/true);
}

Result<json> KotakRestClient::modify_order(const json& params) {
  return request_json(HttpRequest::Method::Post, std::string(endpoints::kModifyOrder),
                      j_data_body(params), /*with_server_id=*/true, /*is_mutation=*/true);
}

Result<json> KotakRestClient::cancel_order(const json& params) {
  return request_json(HttpRequest::Method::Post, std::string(endpoints::kCancelOrder),
                      j_data_body(params), /*with_server_id=*/true, /*is_mutation=*/true);
}

Result<json> KotakRestClient::orders() {
  return request_json(HttpRequest::Method::Get, std::string(endpoints::kOrderBook), std::string{},
                      /*with_server_id=*/true, /*is_mutation=*/false);
}

Result<json> KotakRestClient::trades() {
  return request_json(HttpRequest::Method::Get, std::string(endpoints::kTradeBook), std::string{},
                      /*with_server_id=*/true, /*is_mutation=*/false);
}

Result<json> KotakRestClient::positions() {
  return request_json(HttpRequest::Method::Get, std::string(endpoints::kPositions), std::string{},
                      /*with_server_id=*/true, /*is_mutation=*/false);
}

Result<json> KotakRestClient::holdings() {
  return request_json(HttpRequest::Method::Get, std::string(endpoints::kHoldings), std::string{},
                      /*with_server_id=*/false, /*is_mutation=*/false);
}

// `margins` is a POST only because Kotak takes its filter as a jData body; it is
// a READ, so it keeps the read posture.
Result<json> KotakRestClient::margins(const json& params) {
  return request_json(HttpRequest::Method::Post, std::string(endpoints::kLimits),
                      j_data_body(params), /*with_server_id=*/true, /*is_mutation=*/false);
}

Result<std::string> KotakRestClient::scrip_master() {
  return request_text(HttpRequest::Method::Get, std::string(endpoints::kScripMaster));
}

}  // namespace broker_exec::adapters::kotak
