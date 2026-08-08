// Kotak Neo multi-step session tests (Story 6.1, AC-1/AC-2/AC-3).
//
// The login/2FA responses below are COMMITTED RECORDED-RESPONSE FIXTURES — the
// auth-flow shape is one of the day-one-critical-path unknowns, so it is pinned
// here (VCR substrate, TO-6 tier-1) and exercised with no network and no live
// credentials. Live verification stays a tracked tier-2 operator step.
//
// All credentials are SYNTHETIC and token-shaped so `domain::scrub` must redact
// them wherever the broker echoes them back.

#include "broker_exec/adapters/kotak/kotak_session.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "broker_exec/adapters/kotak/kotak_transport.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/secret_provider.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/secrets/token_store.hpp"
#include "broker_exec/session/session_state.hpp"

namespace fs = std::filesystem;

using broker_exec::Result;
using broker_exec::adapters::kotak::HttpClient;
using broker_exec::adapters::kotak::HttpRequest;
using broker_exec::adapters::kotak::HttpResponse;
using broker_exec::adapters::kotak::KotakSessionBundle;
using broker_exec::adapters::kotak::KotakSessionEstablisher;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::SuggestedAction;
using broker_exec::ports::SecretProvider;
using broker_exec::secrets::TokenStore;
using broker_exec::session::SessionState;

namespace {

// The wire paths come from the header, so a path change cannot silently desync
// the fixtures from the code under test.
namespace endpoints = broker_exec::adapters::kotak::endpoints;

// ── Synthetic credentials (token-shaped on purpose) ──────────────────────────
constexpr const char* kConsumerKey = "ckAAAA1111BBBB2222CCCC3333";
constexpr const char* kConsumerSecret = "csDDDD4444EEEE5555FFFF6666";
constexpr const char* kMobile = "+919000000000";
constexpr const char* kPassword = "pwGGGG7777HHHH8888";
constexpr const char* kMpin = "4321";

constexpr const char* kAccessToken = "atACCESS0000AAAA1111BBBB2222";
constexpr const char* kViewToken = "vtVIEW3333CCCC4444DDDD5555";
constexpr const char* kViewSid = "vsSID6666EEEE7777FFFF8888";
constexpr const char* kFinalToken = "ftFINAL9999GGGG0000HHHH1111";
constexpr const char* kFinalSid = "fsSID2222IIII3333JJJJ4444";
constexpr const char* kServerId = "server3";

constexpr const char* kAccount = "kotak-acct-1";
constexpr const char* kBundleName = "kotak.session_bundle";

// ── Committed Kotak Neo auth fixtures ────────────────────────────────────────

// Leg 1 — POST /oauth2/token (Basic consumer key/secret).
const std::string kOauthOk = std::string(R"({"access_token":")") + kAccessToken +
                             R"(","token_type":"bearer","expires_in":86400,"scope":"default"})";

// Leg 2 — POST /login/1.0/login/v2/validate (mobile + password) -> VIEW token.
const std::string kLoginViewOk =
    std::string(R"({"data":{"token":")") + kViewToken + R"(","sid":")" + kViewSid +
    R"(","rid":"11111111-2222-3333-4444-555555555555","hsServerId":")" + kServerId +
    R"(","isUserPwdExpired":"false","ucc":"AB1234","greetingName":"TEST USER"}})";

// Leg 3 — same endpoint with the MPIN and the view Auth/Sid -> FINAL bundle.
const std::string kMpinOk = std::string(R"({"data":{"token":")") + kFinalToken + R"(","sid":")" +
                            kFinalSid + R"(","hsServerId":")" + kServerId +
                            R"(","ucc":"AB1234","kType":"TRADE"}})";

// Leg 3 refused — the wrong-MPIN fixture.
constexpr const char* kMpinWrong =
    R"({"error":[{"code":"10022","message":"Invalid Credentials"}]})";

// A healthy authenticated read (the validate() probe).
constexpr const char* kOrderBookOk =
    R"({"stat":"Ok","stCode":200,"data":[{"nOrdNo":"220101000000001","ordSt":"complete"}]})";

// An expired sid on a read — HTTP 200 with a dead-session envelope.
constexpr const char* kSessionDead =
    R"({"stat":"Not_Ok","errMsg":"Invalid Session or Token expired, please re-login","stCode":10502})";

constexpr const char* kGatewayDown =
    R"({"fault":{"code":"500","message":"Internal Server Error at the gateway"}})";

// ── Test doubles ─────────────────────────────────────────────────────────────

// A SCRIPTED fake transport: responses are queued per (method, path) and served
// FIFO, because the Kotak login and 2FA legs hit the SAME endpoint. It records
// every request so the auth wiring can be asserted. Never touches the network.
class ScriptedHttpClient final : public HttpClient {
 public:
  void script(HttpRequest::Method method, std::string_view path, long status, std::string body,
              std::vector<std::pair<std::string, std::string>> headers = {}) {
    HttpResponse response;
    response.status_code = status;
    response.body = std::move(body);
    response.headers = std::move(headers);
    script_[key(method, path)].push_back(std::move(response));
  }

  [[nodiscard]] Result<HttpResponse> send(const HttpRequest& request) const override {
    requests_.push_back(request);
    const auto it = script_.find(key(request.method, request.path));
    if (it == script_.end() || it->second.empty()) {
      return broker_exec::fail(broker_exec::errors::make_error(
          ErrorCategory::Unknown, "no scripted response for request", "TEST"));
    }
    HttpResponse response = it->second.front();
    it->second.pop_front();
    return response;
  }

  [[nodiscard]] const std::vector<HttpRequest>& requests() const noexcept { return requests_; }

  [[nodiscard]] static std::string header_of(const HttpRequest& request, std::string_view name) {
    for (const auto& [key_name, value] : request.headers) {
      if (key_name == name) {
        return value;
      }
    }
    return std::string{};
  }

 private:
  static std::string key(HttpRequest::Method method, std::string_view path) {
    return std::to_string(static_cast<int>(method)) + " " + std::string(path);
  }

  mutable std::map<std::string, std::deque<HttpResponse>> script_;
  mutable std::vector<HttpRequest> requests_;
};

class FakeSecretProvider final : public SecretProvider {
 public:
  std::map<std::string, std::string> values;

  [[nodiscard]] Result<std::string> get(std::string_view key) const override {
    const auto it = values.find(std::string(key));
    if (it == values.end()) {
      return broker_exec::fail(
          broker_exec::errors::make_error(ErrorCategory::Auth, "missing secret", "TEST"));
    }
    return it->second;
  }
};

[[nodiscard]] FakeSecretProvider make_secrets() {
  FakeSecretProvider secrets;
  secrets.values["kotak_consumer_key"] = kConsumerKey;
  secrets.values["kotak_consumer_secret"] = kConsumerSecret;
  secrets.values["kotak_mobile"] = kMobile;
  secrets.values["kotak_password"] = kPassword;
  secrets.values["kotak_mpin"] = kMpin;
  // The per-account 256-bit key the real TokenStore looks up as
  // "<account>.token_key" — deliberately NOT co-located with the ciphertext.
  secrets.values[std::string(kAccount) + ".token_key"] = std::string(32, '\x2A');
  return secrets;
}

// A unique temp directory, recursively removed on scope exit.
struct TempDir {
  fs::path path;

  explicit TempDir(const std::string& tag)
      : path(fs::temp_directory_path() /
             ("broker_exec_kotak_" + tag + "_" +
              std::to_string(reinterpret_cast<std::uintptr_t>(this)))) {
    std::error_code ec;
    fs::create_directories(path, ec);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

[[nodiscard]] fs::path blob_path(const TempDir& dir) {
  return dir.path / kAccount / (std::string(kBundleName) + ".enc");
}

[[nodiscard]] std::string read_raw(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// Script the full happy three-leg flow.
void script_happy_auth(ScriptedHttpClient& http) {
  http.script(HttpRequest::Method::Post, endpoints::kOauthToken, 200, kOauthOk);
  http.script(HttpRequest::Method::Post, endpoints::kLoginValidate, 200, kLoginViewOk);
  http.script(HttpRequest::Method::Post, endpoints::kLoginValidate, 200, kMpinOk);
}

}  // namespace

TEST_CASE("the three-leg auth happy path builds and persists an ENCRYPTED bundle",
          "[kotak][session]") {
  ScriptedHttpClient http;
  script_happy_auth(http);

  const auto secrets = make_secrets();
  TempDir dir("happy");
  TokenStore store(secrets, dir.path);
  KotakSessionEstablisher establisher(http, secrets, store, kAccount, kBundleName);

  const auto state = establisher.establish();
  REQUIRE(state.has_value());
  CHECK(state.value() == SessionState::Healthy);

  // All four opaque artifacts survived into the bundle (IBR-6: not one string).
  const auto bundle = establisher.load();
  REQUIRE(bundle.has_value());
  CHECK(bundle.value().access_token == kAccessToken);
  CHECK(bundle.value().token == kFinalToken);
  CHECK(bundle.value().sid == kFinalSid);
  CHECK(bundle.value().hs_server_id == kServerId);
  CHECK(bundle.value().complete());
  // Opaque extras are carried through persistence untouched.
  REQUIRE(bundle.value().find_extra("ucc") != nullptr);
  CHECK(*bundle.value().find_extra("ucc") == "AB1234");

  // AT REST: the blob exists and contains NO plaintext credential and no field
  // name from the serialized JSON — it is AES-256-GCM ciphertext, not JSON.
  REQUIRE(fs::exists(blob_path(dir)));
  const std::string raw = read_raw(blob_path(dir));
  CHECK_FALSE(raw.empty());
  CHECK(raw.find(kFinalToken) == std::string::npos);
  CHECK(raw.find(kFinalSid) == std::string::npos);
  CHECK(raw.find(kAccessToken) == std::string::npos);
  CHECK(raw.find(kServerId) == std::string::npos);
  CHECK(raw.find("access_token") == std::string::npos);
}

TEST_CASE("each auth leg carries exactly the credentials that leg needs", "[kotak][session]") {
  ScriptedHttpClient http;
  script_happy_auth(http);

  const auto secrets = make_secrets();
  TempDir dir("wiring");
  TokenStore store(secrets, dir.path);
  KotakSessionEstablisher establisher(http, secrets, store, kAccount, kBundleName);
  REQUIRE(establisher.establish().has_value());

  REQUIRE(http.requests().size() == 3);
  const HttpRequest& oauth = http.requests()[0];
  const HttpRequest& login = http.requests()[1];
  const HttpRequest& two_fa = http.requests()[2];

  // Leg 1: HTTP Basic over the consumer key/secret (base64 computed independently:
  //   printf 'ckAAAA1111BBBB2222CCCC3333:csDDDD4444EEEE5555FFFF6666' | base64
  CHECK(ScriptedHttpClient::header_of(oauth, "Authorization") ==
        "Basic Y2tBQUFBMTExMUJCQkIyMjIyQ0NDQzMzMzM6Y3NERERENDQ0NEVFRUU1NTU1RkZGRjY2NjY=");
  CHECK(oauth.body == "grant_type=client_credentials");

  // Leg 2: the OAuth bearer, and NO session headers yet.
  CHECK(ScriptedHttpClient::header_of(login, "Authorization") ==
        std::string("Bearer ") + kAccessToken);
  CHECK(ScriptedHttpClient::header_of(login, "Auth").empty());
  CHECK(ScriptedHttpClient::header_of(login, "Sid").empty());
  CHECK(login.body.find("mobileNumber") != std::string::npos);

  // Leg 3: the bearer PLUS the view token/sid from leg 2 — the multi-step link.
  CHECK(ScriptedHttpClient::header_of(two_fa, "Auth") == kViewToken);
  CHECK(ScriptedHttpClient::header_of(two_fa, "Sid") == kViewSid);
  CHECK(two_fa.body.find("mpin") != std::string::npos);
  // The consumer secret is used ONLY on leg 1 and never re-sent.
  CHECK(two_fa.body.find(kConsumerSecret) == std::string::npos);
}

TEST_CASE("a wrong MPIN fails establishment and persists NOTHING", "[kotak][session]") {
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Post, endpoints::kOauthToken, 200, kOauthOk);
  http.script(HttpRequest::Method::Post, endpoints::kLoginValidate, 200, kLoginViewOk);
  http.script(HttpRequest::Method::Post, endpoints::kLoginValidate, 401, kMpinWrong);

  const auto secrets = make_secrets();
  TempDir dir("wrong_mpin");
  TokenStore store(secrets, dir.path);
  KotakSessionEstablisher establisher(http, secrets, store, kAccount, kBundleName);

  const auto state = establisher.establish();
  REQUIRE(state.has_value());
  CHECK(state.value() == SessionState::Failed);

  // THE POINT: no half-session on disk that a later boot could mistake for live.
  CHECK_FALSE(fs::exists(blob_path(dir)));
  CHECK_FALSE(establisher.load().has_value());
}

TEST_CASE("a refused first leg persists nothing either", "[kotak][session]") {
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Post, endpoints::kOauthToken, 401,
              R"({"fault":{"code":"900901","message":"Invalid Credentials"}})");

  const auto secrets = make_secrets();
  TempDir dir("bad_consumer");
  TokenStore store(secrets, dir.path);
  KotakSessionEstablisher establisher(http, secrets, store, kAccount, kBundleName);

  const auto state = establisher.establish();
  REQUIRE(state.has_value());
  CHECK(state.value() == SessionState::Failed);
  CHECK_FALSE(fs::exists(blob_path(dir)));
  // Only leg 1 was attempted — no credential was sent anywhere else.
  CHECK(http.requests().size() == 1);
}

TEST_CASE("a 5xx during login is a retryable Error, not a credential Failure", "[kotak][session]") {
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Post, endpoints::kOauthToken, 503, kGatewayDown);

  const auto secrets = make_secrets();
  TempDir dir("gateway_down");
  TokenStore store(secrets, dir.path);
  KotakSessionEstablisher establisher(http, secrets, store, kAccount, kBundleName);

  const auto state = establisher.establish();
  REQUIRE_FALSE(state.has_value());
  CHECK(state.error().category == ErrorCategory::Network);
  CHECK(state.error().action == SuggestedAction::ReconcileFirst);
  CHECK_FALSE(fs::exists(blob_path(dir)));
}

TEST_CASE("a missing secret surfaces a typed Error and never throws", "[kotak][session]") {
  ScriptedHttpClient http;
  FakeSecretProvider secrets;  // intentionally empty
  TempDir dir("no_secrets");
  TokenStore store(secrets, dir.path);
  KotakSessionEstablisher establisher(http, secrets, store, kAccount, kBundleName);

  const auto state = establisher.establish();
  REQUIRE_FALSE(state.has_value());
  CHECK(state.error().category == ErrorCategory::Auth);
  CHECK(http.requests().empty());
}

TEST_CASE("validate() probes the stored bundle and reports Healthy", "[kotak][session]") {
  ScriptedHttpClient http;
  script_happy_auth(http);
  http.script(HttpRequest::Method::Get, endpoints::kOrderBook, 200, kOrderBookOk);

  const auto secrets = make_secrets();
  TempDir dir("validate_ok");
  TokenStore store(secrets, dir.path);
  KotakSessionEstablisher establisher(http, secrets, store, kAccount, kBundleName);
  REQUIRE(establisher.establish().has_value());

  const auto state = establisher.validate();
  REQUIRE(state.has_value());
  CHECK(state.value() == SessionState::Healthy);

  // The probe authenticated with the FINAL bundle and routed by hsServerId.
  const HttpRequest& probe = http.requests().back();
  CHECK(ScriptedHttpClient::header_of(probe, "Auth") == kFinalToken);
  CHECK(ScriptedHttpClient::header_of(probe, "Sid") == kFinalSid);
  CHECK(ScriptedHttpClient::header_of(probe, "neo-fin-key") == "neotradeapi");
  REQUIRE(probe.query.size() == 1);
  CHECK(probe.query.front().first == "sId");
  CHECK(probe.query.front().second == kServerId);
}

TEST_CASE("an expired sid on a read reports NeedsReauth, never a silent refresh",
          "[kotak][session]") {
  ScriptedHttpClient http;
  script_happy_auth(http);
  // The dangerous shape: HTTP 200 carrying a dead-session envelope.
  http.script(HttpRequest::Method::Get, endpoints::kOrderBook, 200, kSessionDead);

  const auto secrets = make_secrets();
  TempDir dir("expired_sid");
  TokenStore store(secrets, dir.path);
  KotakSessionEstablisher establisher(http, secrets, store, kAccount, kBundleName);
  REQUIRE(establisher.establish().has_value());

  const auto state = establisher.validate();
  REQUIRE(state.has_value());
  CHECK(state.value() == SessionState::NeedsReauth);
}

TEST_CASE("a 401 on the probe also reports NeedsReauth", "[kotak][session]") {
  ScriptedHttpClient http;
  script_happy_auth(http);
  http.script(HttpRequest::Method::Get, endpoints::kOrderBook, 401, "");

  const auto secrets = make_secrets();
  TempDir dir("probe_401");
  TokenStore store(secrets, dir.path);
  KotakSessionEstablisher establisher(http, secrets, store, kAccount, kBundleName);
  REQUIRE(establisher.establish().has_value());

  const auto state = establisher.validate();
  REQUIRE(state.has_value());
  CHECK(state.value() == SessionState::NeedsReauth);
}

TEST_CASE("a 5xx on the probe reconciles first rather than declaring the session dead",
          "[kotak][session]") {
  ScriptedHttpClient http;
  script_happy_auth(http);
  http.script(HttpRequest::Method::Get, endpoints::kOrderBook, 503, kGatewayDown);

  const auto secrets = make_secrets();
  TempDir dir("probe_503");
  TokenStore store(secrets, dir.path);
  KotakSessionEstablisher establisher(http, secrets, store, kAccount, kBundleName);
  REQUIRE(establisher.establish().has_value());

  const auto state = establisher.validate();
  REQUIRE_FALSE(state.has_value());
  CHECK(state.error().category == ErrorCategory::Network);
  CHECK(state.error().action == SuggestedAction::ReconcileFirst);
}

TEST_CASE("a 200 that is not Kotak at all is NEVER reported Healthy", "[kotak][session]") {
  // A captive portal, a load-balancer maintenance page or any HTML error page
  // answers 200 with a body that is not Kotak. `is_kotak_success` cannot
  // contradict a 2xx it cannot parse, so the probe must additionally require a
  // POSITIVELY PARSED envelope — otherwise safe-start green-lights trading
  // against a device that is not the broker.
  ScriptedHttpClient http;
  script_happy_auth(http);
  http.script(HttpRequest::Method::Get, endpoints::kOrderBook, 200,
              "<html><body>Network sign-in required</body></html>");

  const auto secrets = make_secrets();
  TempDir dir("captive_portal");
  TokenStore store(secrets, dir.path);
  KotakSessionEstablisher establisher(http, secrets, store, kAccount, kBundleName);
  REQUIRE(establisher.establish().has_value());

  const auto state = establisher.validate();
  REQUIRE_FALSE(state.has_value());
  CHECK(state.error().category == ErrorCategory::Unknown);
  CHECK(state.error().action == SuggestedAction::ReconcileFirst);
}

TEST_CASE("an empty 200 body is not proof of a live session", "[kotak][session]") {
  ScriptedHttpClient http;
  script_happy_auth(http);
  http.script(HttpRequest::Method::Get, endpoints::kOrderBook, 200, "");

  const auto secrets = make_secrets();
  TempDir dir("empty_probe");
  TokenStore store(secrets, dir.path);
  KotakSessionEstablisher establisher(http, secrets, store, kAccount, kBundleName);
  REQUIRE(establisher.establish().has_value());

  REQUIRE_FALSE(establisher.validate().has_value());
}

TEST_CASE("an instrument-token reject on the probe is an Error, not NeedsReauth",
          "[kotak][session]") {
  // The canonical classifier matches a bare "token". Without the tightened
  // session rule this healthy session would be declared dead and the operator
  // dragged through a pointless re-login.
  ScriptedHttpClient http;
  script_happy_auth(http);
  http.script(
      HttpRequest::Method::Get, endpoints::kOrderBook, 200,
      R"({"stat":"Not_Ok","errMsg":"Invalid instrument token for this exchange segment","stCode":5006})");

  const auto secrets = make_secrets();
  TempDir dir("instrument_token");
  TokenStore store(secrets, dir.path);
  KotakSessionEstablisher establisher(http, secrets, store, kAccount, kBundleName);
  REQUIRE(establisher.establish().has_value());

  const auto state = establisher.validate();
  REQUIRE_FALSE(state.has_value());
  CHECK(state.error().category != ErrorCategory::SessionExpired);
  CHECK(state.error().action != SuggestedAction::ReEstablishSession);
}

TEST_CASE("an unrecognized login failure is an Error, not 'your credentials are wrong'",
          "[kotak][session]") {
  // Establishment reports Failed ONLY on a positive credential refusal. An
  // unclassifiable status with no Kotak envelope (proxy error, wrong host, an
  // interposed appliance) must stay a retryable Error — telling the operator to
  // rotate credentials during an infrastructure fault sends them the wrong way.
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Post, endpoints::kOauthToken, 409,
              "<html><body>Conflict</body></html>");

  const auto secrets = make_secrets();
  TempDir dir("unclassified_login");
  TokenStore store(secrets, dir.path);
  KotakSessionEstablisher establisher(http, secrets, store, kAccount, kBundleName);

  const auto state = establisher.establish();
  REQUIRE_FALSE(state.has_value());
  CHECK(state.error().action == SuggestedAction::ReconcileFirst);
  CHECK_FALSE(fs::exists(blob_path(dir)));
}

TEST_CASE("a 200 login response carrying no token is an Error, not a credential Failure",
          "[kotak][session]") {
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Post, endpoints::kOauthToken, 200,
              R"({"token_type":"bearer","expires_in":86400})");

  const auto secrets = make_secrets();
  TempDir dir("no_token");
  TokenStore store(secrets, dir.path);
  KotakSessionEstablisher establisher(http, secrets, store, kAccount, kBundleName);

  const auto state = establisher.establish();
  REQUIRE_FALSE(state.has_value());
  CHECK(state.error().category == ErrorCategory::Auth);
  CHECK_FALSE(fs::exists(blob_path(dir)));
}

TEST_CASE("a stored-but-incomplete bundle validates as Failed, never Healthy", "[kotak][session]") {
  ScriptedHttpClient http;
  const auto secrets = make_secrets();
  TempDir dir("incomplete");
  TokenStore store(secrets, dir.path);

  // A bundle with no hsServerId cannot route an order — it is not a live session.
  KotakSessionBundle partial;
  partial.access_token = kAccessToken;
  partial.token = kFinalToken;
  partial.sid = kFinalSid;
  REQUIRE(store.save(kAccount, kBundleName, partial.to_json()).has_value());

  KotakSessionEstablisher establisher(http, secrets, store, kAccount, kBundleName);
  const auto state = establisher.validate();
  REQUIRE(state.has_value());
  CHECK(state.value() == SessionState::Failed);
  CHECK(http.requests().empty());  // no call was made with a broken bundle
}

TEST_CASE("the bundle survives a JSON round trip, including extras", "[kotak][session]") {
  KotakSessionBundle bundle;
  bundle.access_token = kAccessToken;
  bundle.token = kFinalToken;
  bundle.sid = kFinalSid;
  bundle.hs_server_id = kServerId;
  bundle.extras.emplace_back("ucc", "AB1234");
  bundle.extras.emplace_back("kType", "TRADE");

  const auto restored = KotakSessionBundle::from_json(bundle.to_json());
  REQUIRE(restored.has_value());
  CHECK(restored.value().access_token == bundle.access_token);
  CHECK(restored.value().token == bundle.token);
  CHECK(restored.value().sid == bundle.sid);
  CHECK(restored.value().hs_server_id == bundle.hs_server_id);
  REQUIRE(restored.value().find_extra("kType") != nullptr);
  CHECK(*restored.value().find_extra("kType") == "TRADE");
  CHECK(restored.value().find_extra("nope") == nullptr);
}

TEST_CASE("a malformed stored blob fails closed without echoing the blob",
          "[kotak][session][scrub]") {
  const auto restored = KotakSessionBundle::from_json("not json {{{");
  REQUIRE_FALSE(restored.has_value());
  CHECK(restored.error().category == ErrorCategory::Internal);
  CHECK(restored.error().message.find("not json") == std::string::npos);
}

TEST_CASE("no login credential ever reaches an Error message", "[kotak][session][scrub]") {
  // A hostile/failing broker echoes the MPIN, the consumer secret AND the
  // password back at us. Every one of these is really present in the fixture —
  // asserting the absence of something the fixture never contained proves
  // nothing.
  const std::string hostile =
      std::string(R"({"stat":"Not_Ok","errMsg":"login failed: mpin 4321 secret )") +
      kConsumerSecret + " password=" + kPassword + R"( timed out","stCode":5203})";

  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Post, endpoints::kOauthToken, 504, hostile);

  const auto secrets = make_secrets();
  TempDir dir("scrub");
  TokenStore store(secrets, dir.path);
  KotakSessionEstablisher establisher(http, secrets, store, kAccount, kBundleName);

  const auto state = establisher.establish();
  REQUIRE_FALSE(state.has_value());

  const auto& error = state.error();
  REQUIRE(hostile.find(kConsumerSecret) != std::string::npos);  // the fixture really carries them
  REQUIRE(hostile.find(kPassword) != std::string::npos);
  CHECK(error.message.find(kConsumerSecret) == std::string::npos);
  CHECK(error.message.find(kMpin) == std::string::npos);
  CHECK(error.message.find(kPassword) == std::string::npos);
  CHECK(error.broker_code.find(kConsumerSecret) == std::string::npos);
}

TEST_CASE("the re-auth alert is typed and secret-free", "[kotak][session]") {
  const auto error = KotakSessionEstablisher::needs_reauth_error();
  CHECK(error.category == ErrorCategory::SessionExpired);
  CHECK(error.action == SuggestedAction::ReEstablishSession);
  CHECK(error.broker_code.empty());
  CHECK(error.message.find("re-establish") != std::string::npos);
}

TEST_CASE("Kotak headless session refresh is NOT claimed", "[kotak][session]") {
  // Unknown in the tri-state model collapses to this fail-closed boolean: the
  // runtime must never auto-refresh a Kotak session.
  STATIC_REQUIRE_FALSE(KotakSessionEstablisher::kSupportsHeadlessSessionRefresh);
}
