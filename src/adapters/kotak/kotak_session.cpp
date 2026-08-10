#include "broker_exec/adapters/kotak/kotak_session.hpp"

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "broker_exec/adapters/kotak/kotak_errors.hpp"
#include "broker_exec/adapters/kotak/kotak_transport.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/session/session_state.hpp"

namespace broker_exec::adapters::kotak {

using json = nlohmann::json;
using session::SessionState;

namespace {

// Standard base64 (RFC 4648) for the HTTP Basic credential. Local, standard-
// library-only, and deliberately NOT a dependency: the only input is the
// consumer key/secret pair, which is materialized for exactly one header.
[[nodiscard]] std::string base64_encode(std::string_view input) {
  static constexpr std::string_view kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((input.size() + 2) / 3) * 4);

  std::size_t i = 0;
  while (i + 2 < input.size()) {
    const auto b0 = static_cast<unsigned>(static_cast<unsigned char>(input[i]));
    const auto b1 = static_cast<unsigned>(static_cast<unsigned char>(input[i + 1]));
    const auto b2 = static_cast<unsigned>(static_cast<unsigned char>(input[i + 2]));
    const unsigned triple = (b0 << 16U) | (b1 << 8U) | b2;
    out.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
    out.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
    out.push_back(kAlphabet[(triple >> 6U) & 0x3FU]);
    out.push_back(kAlphabet[triple & 0x3FU]);
    i += 3;
  }
  if (const std::size_t remaining = input.size() - i; remaining > 0) {
    const auto b0 = static_cast<unsigned>(static_cast<unsigned char>(input[i]));
    const unsigned b1 =
        remaining > 1 ? static_cast<unsigned>(static_cast<unsigned char>(input[i + 1])) : 0U;
    const unsigned triple = (b0 << 16U) | (b1 << 8U);
    out.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
    out.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
    out.push_back(remaining > 1 ? kAlphabet[(triple >> 6U) & 0x3FU] : '=');
    out.push_back('=');
  }
  return out;
}

// The login `data` field names, with the spelling variants Kotak has shipped.
[[nodiscard]] std::string pick_string(const json& object,
                                      std::initializer_list<const char*> names) {
  for (const char* name : names) {
    const auto it = object.find(name);
    if (it != object.end() && it->is_string()) {
      std::string value = it->get<std::string>();
      if (!value.empty()) {
        return value;
      }
    }
  }
  return std::string{};
}

// True for a key we lift into a dedicated bundle field (so it is not duplicated
// into `extras`).
[[nodiscard]] bool is_core_field(std::string_view key) {
  return key == "token" || key == "sid" || key == "hsServerId" || key == "hsServerid" ||
         key == "serverId" || key == "sId";
}

constexpr std::string_view kJsonContentType = "application/json";

// Serialize without ever throwing. nlohmann's DEFAULT dump() throws
// `type_error.316` on a non-UTF-8 byte, and these payloads carry operator-
// supplied credentials straight from the SecretProvider — a password or MPIN
// with one stray byte would throw across our no-throw boundary during login.
// `error_handler_t::replace` substitutes U+FFFD instead.
[[nodiscard]] std::string dump_no_throw(const json& value) {
  return value.dump(-1, ' ', /*ensure_ascii=*/false, json::error_handler_t::replace);
}

// Did the BROKER positively refuse our credentials (-> a Failed establishment),
// as opposed to being unreachable/broken (-> a typed Error the caller retries)?
//
// A POSITIVE ALLOWLIST on purpose. A negative filter ("anything that is not a
// network error means the credentials are wrong") turns every future surprise —
// an Internal bug, an Unknown shape, a 409 — into "operator, your password is
// wrong", sending them to rotate credentials during what is really an outage.
// Validation/BrokerRejected additionally require a positively PARSED envelope:
// a bare 400 with an empty body is not evidence about a credential.
[[nodiscard]] bool is_credential_refusal(const errors::Error& error, const KotakEnvelope& env) {
  switch (error.category) {
    case errors::ErrorCategory::Auth:
    case errors::ErrorCategory::SessionExpired:
      return true;  // an explicit auth verdict from the broker
    case errors::ErrorCategory::Validation:
    case errors::ErrorCategory::BrokerRejected:
      return env.parsed;
    default:
      return false;  // Network/Timeout/RateLimited/Internal/Unknown -> try again
  }
}

}  // namespace

// ── KotakSessionBundle ───────────────────────────────────────────────────────

bool KotakSessionBundle::complete() const noexcept {
  return !access_token.empty() && !token.empty() && !sid.empty() && !hs_server_id.empty();
}

const std::string* KotakSessionBundle::find_extra(std::string_view name) const noexcept {
  for (const auto& entry : extras) {
    if (entry.first == name) {
      return &entry.second;
    }
  }
  return nullptr;
}

std::string KotakSessionBundle::to_json() const {
  json out;
  out["access_token"] = access_token;
  out["token"] = token;
  out["sid"] = sid;
  out["hs_server_id"] = hs_server_id;
  json extra_object = json::object();
  for (const auto& [key, value] : extras) {
    extra_object[key] = value;
  }
  out["extras"] = std::move(extra_object);
  return dump_no_throw(out);
}

Result<KotakSessionBundle> KotakSessionBundle::from_json(std::string_view text) {
  const json parsed = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    // The blob is credential material — it is NEVER echoed into the Error.
    return broker_exec::fail(errors::make_error(errors::ErrorCategory::Internal,
                                                "kotak: stored session bundle is malformed"));
  }
  KotakSessionBundle bundle;
  bundle.access_token = pick_string(parsed, {"access_token"});
  bundle.token = pick_string(parsed, {"token"});
  bundle.sid = pick_string(parsed, {"sid"});
  bundle.hs_server_id = pick_string(parsed, {"hs_server_id"});
  if (const auto extras = parsed.find("extras"); extras != parsed.end() && extras->is_object()) {
    for (auto it = extras->begin(); it != extras->end(); ++it) {
      if (it->is_string()) {
        bundle.extras.emplace_back(it.key(), it->get<std::string>());
      }
    }
  }
  return bundle;
}

std::vector<std::pair<std::string, std::string>> auth_headers(const KotakSessionBundle& bundle) {
  std::vector<std::pair<std::string, std::string>> headers;
  // The three artifacts that authenticate a Kotak call. This is the ONLY place
  // they are materialized; they are never cached on a client or reachable from
  // any log/Error path.
  headers.emplace_back("Authorization", "Bearer " + bundle.access_token);
  headers.emplace_back("Auth", bundle.token);
  headers.emplace_back("Sid", bundle.sid);
  headers.emplace_back(std::string(kNeoFinKeyHeader), std::string(kNeoFinKeyValue));
  return headers;
}

// ── KotakSessionEstablisher ──────────────────────────────────────────────────

KotakSessionEstablisher::KotakSessionEstablisher(const HttpClient& http,
                                                 const ports::SecretProvider& secrets,
                                                 secrets::TokenStore& store,
                                                 std::string account_id,
                                                 std::string bundle_store_name)
    : http_(http),
      secrets_(secrets),
      store_(store),
      account_id_(std::move(account_id)),
      bundle_store_name_(std::move(bundle_store_name)) {}

Result<std::string> KotakSessionEstablisher::fetch_access_token(
    const std::string& consumer_key, const std::string& consumer_secret,
    bool& credential_refusal) const {
  credential_refusal = false;

  HttpRequest request;
  request.method = HttpRequest::Method::Post;
  request.path = std::string(endpoints::kOauthToken);
  // HTTP Basic over the consumer credentials — materialized here and nowhere
  // else. The encoded value never reaches a log, an Error, or the bundle.
  request.headers.emplace_back("Authorization",
                               "Basic " + base64_encode(consumer_key + ":" + consumer_secret));
  request.headers.emplace_back("Content-Type", "application/x-www-form-urlencoded");
  request.body = "grant_type=client_credentials";

  auto response = http_.send(request);
  if (!response) {
    return broker_exec::fail(response.error());  // transport error (already scrubbed)
  }
  if (!is_kotak_success(response.value())) {
    const KotakEnvelope envelope = parse_kotak_envelope(response.value().body);
    errors::Error mapped = map_kotak_error(response.value());
    credential_refusal = is_credential_refusal(mapped, envelope);
    return broker_exec::fail(std::move(mapped));
  }

  const json parsed = json::parse(response.value().body, nullptr, /*allow_exceptions=*/false);
  std::string access_token;
  if (!parsed.is_discarded() && parsed.is_object()) {
    access_token = pick_string(parsed, {"access_token"});
    if (access_token.empty()) {
      if (const auto data = parsed.find("data"); data != parsed.end() && data->is_object()) {
        access_token = pick_string(*data, {"access_token"});
      }
    }
  }
  if (access_token.empty()) {
    return broker_exec::fail(errors::make_error(
        errors::ErrorCategory::Auth, "kotak: oauth response carried no access token",
        "HTTP " + std::to_string(response.value().status_code)));
  }
  return access_token;
}

Result<KotakSessionEstablisher::LoginLeg> KotakSessionEstablisher::post_login(
    const std::string& access_token, const std::string& body,
    const std::vector<std::pair<std::string, std::string>>& extra_headers,
    bool& credential_refusal) const {
  credential_refusal = false;

  HttpRequest request;
  request.method = HttpRequest::Method::Post;
  request.path = std::string(endpoints::kLoginValidate);
  request.headers.emplace_back("Authorization", "Bearer " + access_token);
  request.headers.emplace_back(std::string(kNeoFinKeyHeader), std::string(kNeoFinKeyValue));
  request.headers.emplace_back("Content-Type", std::string(kJsonContentType));
  for (const auto& header : extra_headers) {
    request.headers.push_back(header);
  }
  request.body = body;

  auto response = http_.send(request);
  if (!response) {
    return broker_exec::fail(response.error());
  }
  if (!is_kotak_success(response.value())) {
    const KotakEnvelope envelope = parse_kotak_envelope(response.value().body);
    errors::Error mapped = map_kotak_error(response.value());
    credential_refusal = is_credential_refusal(mapped, envelope);
    return broker_exec::fail(std::move(mapped));
  }

  const json parsed = json::parse(response.value().body, nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return broker_exec::fail(errors::make_error(
        errors::ErrorCategory::Auth, "kotak: login response was not a JSON object",
        "HTTP " + std::to_string(response.value().status_code)));
  }
  // Kotak nests the artifacts under `data`; tolerate a flat shape too.
  const auto data_it = parsed.find("data");
  const json& data = (data_it != parsed.end() && data_it->is_object()) ? *data_it : parsed;

  LoginLeg leg;
  leg.token = pick_string(data, {"token"});
  leg.sid = pick_string(data, {"sid"});
  leg.hs_server_id = pick_string(data, {"hsServerId", "hsServerid", "serverId", "sId"});
  for (auto it = data.begin(); it != data.end(); ++it) {
    if (it->is_string() && !is_core_field(it.key())) {
      leg.extras.emplace_back(it.key(), it->get<std::string>());
    }
  }
  if (leg.token.empty()) {
    return broker_exec::fail(errors::make_error(
        errors::ErrorCategory::Auth, "kotak: login response carried no session token",
        "HTTP " + std::to_string(response.value().status_code)));
  }
  return leg;
}

Result<SessionState> KotakSessionEstablisher::establish(const KotakLoginInputs& inputs) {
  // Resolve every credential FIRST, so that from here on any failure can only
  // have come from the broker. A missing/unreadable secret is a CONFIGURATION
  // error and must surface as its own typed Error — never as the "your
  // credentials were refused" state. The values live only in these locals and
  // are gone when this function returns; none is stored, logged, or persisted.
  const auto consumer_key = secrets_.get(inputs.consumer_key_secret_name);
  if (!consumer_key) {
    return broker_exec::fail(consumer_key.error());
  }
  const auto consumer_secret = secrets_.get(inputs.consumer_secret_secret_name);
  if (!consumer_secret) {
    return broker_exec::fail(consumer_secret.error());
  }
  const auto mobile = secrets_.get(inputs.mobile_secret_name);
  if (!mobile) {
    return broker_exec::fail(mobile.error());
  }
  const auto password = secrets_.get(inputs.password_secret_name);
  if (!password) {
    return broker_exec::fail(password.error());
  }
  const auto mpin = secrets_.get(inputs.mpin_secret_name);
  if (!mpin) {
    return broker_exec::fail(mpin.error());
  }

  // Set by each leg when the BROKER positively refused the credentials (see
  // is_credential_refusal): that is a Failed establishment. Anything else — an
  // outage, a throttle, an unrecognized shape — stays a typed Error the caller
  // can retry, rather than telling the operator their password is wrong.
  bool credential_refusal = false;

  // ── Leg 1: consumer credentials -> OAuth access_token ──
  auto access_token =
      fetch_access_token(consumer_key.value(), consumer_secret.value(), credential_refusal);
  if (!access_token) {
    if (credential_refusal) {
      return SessionState::Failed;  // nothing persisted
    }
    return broker_exec::fail(access_token.error());
  }

  // ── Leg 2: mobile + password -> the VIEW token/sid ──
  // The bodies are built with the JSON writer (never string concatenation) so a
  // credential containing a quote/backslash cannot corrupt or escape the frame.
  std::string login_body;
  {
    json body;
    body["mobileNumber"] = mobile.value();
    body["password"] = password.value();
    login_body = dump_no_throw(body);
  }
  auto view = post_login(access_token.value(), login_body, {}, credential_refusal);
  if (!view) {
    if (credential_refusal) {
      return SessionState::Failed;  // nothing persisted
    }
    return broker_exec::fail(view.error());
  }

  // ── Leg 3: 2FA (MPIN) carrying the view token/sid -> the FINAL bundle ──
  std::string mpin_body;
  {
    json body;
    body["mobileNumber"] = mobile.value();
    body["mpin"] = mpin.value();
    mpin_body = dump_no_throw(body);
  }
  std::vector<std::pair<std::string, std::string>> two_fa_headers;
  two_fa_headers.emplace_back("Auth", view.value().token);
  two_fa_headers.emplace_back("Sid", view.value().sid);

  auto final_leg = post_login(access_token.value(), mpin_body, two_fa_headers, credential_refusal);
  if (!final_leg) {
    if (credential_refusal) {
      return SessionState::Failed;  // nothing persisted — the wrong-MPIN path
    }
    return broker_exec::fail(final_leg.error());
  }

  KotakSessionBundle bundle;
  bundle.access_token = std::move(access_token).value();
  bundle.token = std::move(final_leg.value().token);
  // Leg 3 usually re-issues sid/hsServerId; fall back to leg 2's when it does not.
  bundle.sid = final_leg.value().sid.empty() ? view.value().sid : final_leg.value().sid;
  bundle.hs_server_id = final_leg.value().hs_server_id.empty() ? view.value().hs_server_id
                                                              : final_leg.value().hs_server_id;
  bundle.extras = std::move(final_leg.value().extras);

  // An incomplete bundle is a failed establishment — we do NOT persist a session
  // that cannot place an order.
  if (!bundle.complete()) {
    return SessionState::Failed;
  }

  // ONLY here, with a complete bundle in hand, does anything touch the disk — and
  // only through the AES-256-GCM TokenStore.
  auto saved = store_.save(account_id_, bundle_store_name_, bundle.to_json());
  if (!saved) {
    return broker_exec::fail(std::move(saved).error());
  }
  return SessionState::Healthy;
}

Result<KotakSessionBundle> KotakSessionEstablisher::load() const {
  auto blob = store_.load(account_id_, bundle_store_name_);
  if (!blob) {
    return broker_exec::fail(std::move(blob).error());
  }
  return KotakSessionBundle::from_json(blob.value());
}

Result<SessionState> KotakSessionEstablisher::validate() const {
  auto bundle = load();
  if (!bundle) {
    return broker_exec::fail(bundle.error());
  }
  if (!bundle.value().complete()) {
    return SessionState::Failed;
  }

  // One cheap authenticated read. The order book is a GET and is the same
  // endpoint the REST client uses, so a probe pass means real calls will pass.
  HttpRequest request;
  request.method = HttpRequest::Method::Get;
  request.path = std::string(endpoints::kOrderBook);
  request.headers = auth_headers(bundle.value());
  request.query.emplace_back("sId", bundle.value().hs_server_id);

  auto response = http_.send(request);
  if (!response) {
    return broker_exec::fail(response.error());  // transport -> reconcile-first
  }
  // A dead session is a normalized STATE, not a retryable error: the caller
  // blocks trading and alerts the operator. Never a silent refresh.
  if (is_session_death(response.value())) {
    return SessionState::NeedsReauth;
  }
  if (is_kotak_success(response.value())) {
    // A 2xx is NOT proof of a live session. A captive portal, a load-balancer
    // maintenance page or an HTML error page all answer 200 with a body that is
    // not Kotak at all — and `is_kotak_success` cannot contradict a 2xx it
    // cannot parse. Requiring a POSITIVELY PARSED envelope is what stops this
    // gate from green-lighting trading against something that is not the broker.
    if (parse_kotak_envelope(response.value().body).parsed) {
      return SessionState::Healthy;
    }
    return broker_exec::fail(errors::make_error(
        errors::ErrorCategory::Unknown,
        "kotak: session probe returned an unrecognized (non-JSON) body; not treating as healthy",
        "HTTP " + std::to_string(response.value().status_code)));
  }
  return broker_exec::fail(map_kotak_error(response.value()));
}

errors::Error KotakSessionEstablisher::needs_reauth_error() {
  errors::Error error;
  error.category = errors::ErrorCategory::SessionExpired;
  error.action = errors::SuggestedAction::ReEstablishSession;
  error.message =
      "kotak: session bundle expired; operator must re-establish (headless refresh unverified)";
  return error;
}

}  // namespace broker_exec::adapters::kotak
