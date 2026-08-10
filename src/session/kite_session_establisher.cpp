#include "broker_exec/session/kite_session_establisher.hpp"

#include <openssl/evp.h>

#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "broker_exec/adapters/kite/http_client.hpp"
#include "broker_exec/adapters/kite/kite_rest_client.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::session {

using adapters::kite::HttpRequest;
using adapters::kite::HttpResponse;
using json = nlohmann::json;

namespace {

// RAII owner for an EVP_MD_CTX: freed on every path (success or early return) so
// no OpenSSL context leaks across the no-throw boundary (mirrors secrets'
// CipherCtx guard).
class DigestCtx {
 public:
  DigestCtx() noexcept : ctx_(EVP_MD_CTX_new()) {}
  ~DigestCtx() {
    if (ctx_ != nullptr) {
      EVP_MD_CTX_free(ctx_);
    }
  }
  DigestCtx(const DigestCtx&) = delete;
  DigestCtx& operator=(const DigestCtx&) = delete;
  DigestCtx(DigestCtx&&) = delete;
  DigestCtx& operator=(DigestCtx&&) = delete;

  [[nodiscard]] EVP_MD_CTX* get() const noexcept { return ctx_; }
  [[nodiscard]] explicit operator bool() const noexcept { return ctx_ != nullptr; }

 private:
  EVP_MD_CTX* ctx_;
};

// Lowercase hex SHA-256 of (a‖b‖c) via OpenSSL. Returns an empty string on a
// crypto failure (which the caller maps to a secret-free Error). The inputs are
// sensitive (api_secret-derived) and never appear in any log/Error.
[[nodiscard]] std::string sha256_hex(std::string_view a, std::string_view b, std::string_view c) {
  DigestCtx ctx;
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_len = 0;
  if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(ctx.get(), a.data(), a.size()) != 1 ||
      EVP_DigestUpdate(ctx.get(), b.data(), b.size()) != 1 ||
      EVP_DigestUpdate(ctx.get(), c.data(), c.size()) != 1 ||
      EVP_DigestFinal_ex(ctx.get(), digest.data(), &digest_len) != 1) {
    return std::string{};
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(static_cast<std::size_t>(digest_len) * 2);
  for (unsigned int i = 0; i < digest_len; ++i) {
    out.push_back(kHex[digest[i] >> 4U]);
    out.push_back(kHex[digest[i] & 0x0FU]);
  }
  return out;
}

// Percent-encode for an x-www-form-urlencoded body (unreserved chars per RFC
// 3986 pass through; everything else is %XX). ASCII-only, no locale. (Same shape
// as the adapter's helper; kept local so session doesn't reach into adapter
// internals.)
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

// The Kite session endpoint and the lightweight authenticated read used to probe
// liveness. Both are relative to the HttpClient's configured base URL.
constexpr std::string_view kSessionTokenPath = "/session/token";
constexpr std::string_view kValidatePath = "/user/margins/equity";

// Extract (status, error_type) from a Kite envelope body, leaving both empty when
// the body is not a JSON object. The body is parsed with allow_exceptions=false
// and is NEVER stored on an Error.
struct Envelope {
  std::string status;
  std::string error_type;
};

[[nodiscard]] Envelope parse_envelope(const std::string& body) {
  Envelope env;
  const json parsed = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return env;
  }
  if (const auto it = parsed.find("status"); it != parsed.end() && it->is_string()) {
    env.status = it->get<std::string>();
  }
  if (const auto it = parsed.find("error_type"); it != parsed.end() && it->is_string()) {
    env.error_type = it->get<std::string>();
  }
  return env;
}

[[nodiscard]] bool is_success_2xx(const HttpResponse& resp, const Envelope& env) {
  return resp.status_code >= 200 && resp.status_code < 300 && env.status != "error";
}

[[nodiscard]] bool is_token_failure(const HttpResponse& resp, const Envelope& env) {
  return resp.status_code == 401 || resp.status_code == 403 ||
         env.error_type == "TokenException" || env.error_type == "PermissionException";
}

}  // namespace

KiteSessionEstablisher::KiteSessionEstablisher(const adapters::kite::HttpClient& http,
                                               const ports::SecretProvider& secrets,
                                               secrets::TokenStore& store, std::string account_id,
                                               std::string api_key_secret_name,
                                               std::string api_secret_secret_name,
                                               std::string access_token_store_name)
    : http_(http),
      secrets_(secrets),
      store_(store),
      account_id_(std::move(account_id)),
      api_key_secret_name_(std::move(api_key_secret_name)),
      api_secret_secret_name_(std::move(api_secret_secret_name)),
      access_token_store_name_(std::move(access_token_store_name)) {}

Result<SessionState> KiteSessionEstablisher::establish(std::string request_token) {
  // Resolve credentials lazily; a missing secret surfaces as the provider's
  // (already secret-free) Error.
  const auto api_key = secrets_.get(api_key_secret_name_);
  if (!api_key) {
    return broker_exec::fail(api_key.error());
  }
  const auto api_secret = secrets_.get(api_secret_secret_name_);
  if (!api_secret) {
    return broker_exec::fail(api_secret.error());
  }

  // checksum = SHA-256(api_key + request_token + api_secret), lowercase hex.
  const std::string checksum = sha256_hex(api_key.value(), request_token, api_secret.value());
  if (checksum.empty()) {
    // A crypto failure — never echo any input.
    return broker_exec::fail(errors::make_error(
        errors::ErrorCategory::Internal, "kite: session checksum computation failed"));
  }

  // Form-encoded body for the UNauthenticated /session/token exchange. No
  // Authorization header on this endpoint.
  std::string body;
  body += "api_key=";
  body += url_encode(api_key.value());
  body += "&request_token=";
  body += url_encode(request_token);
  body += "&checksum=";
  body += url_encode(checksum);

  HttpRequest req;
  req.method = HttpRequest::Method::Post;
  req.path = std::string(kSessionTokenPath);
  req.headers.emplace_back("X-Kite-Version", "3");
  req.headers.emplace_back("Content-Type", "application/x-www-form-urlencoded");
  req.body = std::move(body);

  auto resp = http_.send(req);
  if (!resp) {
    return broker_exec::fail(resp.error());  // transport error (already scrubbed)
  }
  const HttpResponse& r = resp.value();
  const Envelope env = parse_envelope(r.body);
  if (!is_success_2xx(r, env)) {
    return broker_exec::fail(adapters::kite::map_http_error(r));
  }

  // Parse data.access_token from the success envelope.
  const json parsed = json::parse(r.body, nullptr, /*allow_exceptions=*/false);
  std::string access_token;
  if (!parsed.is_discarded() && parsed.is_object()) {
    if (const auto data = parsed.find("data"); data != parsed.end() && data->is_object()) {
      if (const auto at = data->find("access_token"); at != data->end() && at->is_string()) {
        access_token = at->get<std::string>();
      }
    }
  }
  if (access_token.empty()) {
    return broker_exec::fail(errors::make_error(
        errors::ErrorCategory::Unknown, "kite: session response missing access_token",
        "HTTP " + std::to_string(r.status_code)));
  }

  // Persist the daily token ENCRYPTED at rest. The TokenStore Error is already
  // secret-free; the token itself never reaches a log/Error.
  auto saved = store_.save(account_id_, access_token_store_name_, access_token);
  if (!saved) {
    return broker_exec::fail(std::move(saved).error());
  }
  return SessionState::Healthy;
}

Result<SessionState> KiteSessionEstablisher::validate() {
  // The daily token lives encrypted in the TokenStore (written by establish()),
  // so validate() loads it from there — the single, end-to-end-consistent source
  // — rather than via a second SecretProvider name. The api_key still comes from
  // the SecretProvider; both are materialized ONLY into the Authorization header.
  const auto api_key = secrets_.get(api_key_secret_name_);
  if (!api_key) {
    return broker_exec::fail(api_key.error());
  }
  auto access_token = store_.load(account_id_, access_token_store_name_);
  if (!access_token) {
    return broker_exec::fail(std::move(access_token).error());
  }

  HttpRequest req;
  req.method = HttpRequest::Method::Get;
  req.path = std::string(kValidatePath);
  req.headers.emplace_back("Authorization",
                           "token " + api_key.value() + ":" + access_token.value());
  req.headers.emplace_back("X-Kite-Version", "3");

  auto resp = http_.send(req);
  if (!resp) {
    return broker_exec::fail(resp.error());  // transport error -> reconcile-first
  }
  const HttpResponse& r = resp.value();
  const Envelope env = parse_envelope(r.body);

  // A dead token is a normalized state, NOT an Error to retry: the caller blocks
  // trading and raises the operator alert (never auto-refresh).
  if (is_token_failure(r, env)) {
    return SessionState::NeedsReauth;
  }
  if (is_success_2xx(r, env)) {
    return SessionState::Healthy;
  }
  // Any other failure (5xx, broker reject, malformed) -> reconcile-first Error.
  return broker_exec::fail(adapters::kite::map_http_error(r));
}

errors::Error KiteSessionEstablisher::needs_reauth_error() {
  errors::Error err;
  err.category = errors::ErrorCategory::SessionExpired;
  err.action = errors::SuggestedAction::ReEstablishSession;
  err.message = "kite: daily session expired; operator must re-establish (no headless refresh)";
  return err;
}

}  // namespace broker_exec::session
