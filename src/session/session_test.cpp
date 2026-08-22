#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "broker_exec/adapters/kite/http_client.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/secret_provider.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/secrets/token_store.hpp"
#include "broker_exec/session/kite_session_establisher.hpp"
#include "broker_exec/session/session_state.hpp"

namespace fs = std::filesystem;

using broker_exec::Result;
using broker_exec::adapters::kite::HttpClient;
using broker_exec::adapters::kite::HttpRequest;
using broker_exec::adapters::kite::HttpResponse;
using broker_exec::errors::ErrorCategory;
using broker_exec::ports::SecretProvider;
using broker_exec::secrets::TokenStore;
using broker_exec::session::KiteSessionEstablisher;
using broker_exec::session::SessionState;

namespace {

// A known (api_key, request_token, api_secret) triple, all url-safe so they pass
// through the form encoder unchanged. The expected checksum is the lowercase hex
// SHA-256 of their concatenation, computed independently (sha256sum) — NOT via
// the code under test:
//   echo -n "apikeyAAAA1111request_tok_TTTT2222apisecretSSSS3333" | sha256sum
constexpr const char* kApiKey = "apikeyAAAA1111";
constexpr const char* kRequestToken = "request_tok_TTTT2222";
constexpr const char* kApiSecret = "apisecretSSSS3333";
constexpr const char* kExpectedChecksum =
    "671e88aa4b4b4dc9be95cf938dc9169d9d5911b121f51b2e61af712216159b7b";

constexpr const char* kAccount = "acct-1";
constexpr const char* kApiKeyName = "kite.api_key";
constexpr const char* kApiSecretName = "kite.api_secret";
constexpr const char* kAccessTokenStoreName = "kite.access_token";
constexpr const char* kAccessToken = "ATfreshDailyToken9999XXXX";

// Test double: returns canned HttpResponses keyed by (method, path); never
// touches the network. Captures the last request so the POSTed body (checksum)
// can be asserted.
class RecordedHttpClient final : public HttpClient {
 public:
  void on(HttpRequest::Method method, std::string path, HttpResponse response) {
    table_[key(method, path)] = std::move(response);
  }

  [[nodiscard]] Result<HttpResponse> send(const HttpRequest& request) const override {
    last_request_ = request;
    const auto it = table_.find(key(request.method, request.path));
    if (it == table_.end()) {
      return broker_exec::fail(broker_exec::errors::make_error(
          ErrorCategory::Unknown, "no recorded response for request", "TEST"));
    }
    return it->second;
  }

  [[nodiscard]] const HttpRequest& last_request() const noexcept { return last_request_; }

 private:
  static std::string key(HttpRequest::Method method, const std::string& path) {
    return std::to_string(static_cast<int>(method)) + " " + path;
  }

  std::map<std::string, HttpResponse> table_;
  mutable HttpRequest last_request_;
};

// A fake secret store: api_key + api_secret for the establisher, plus the
// account-scoped 32-byte key the real TokenStore looks up as "<account>.token_key".
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
  secrets.values[kApiKeyName] = kApiKey;
  secrets.values[kApiSecretName] = kApiSecret;
  // The per-account 256-bit (32-byte) encryption key for the TokenStore.
  secrets.values[std::string(kAccount) + ".token_key"] = std::string(32, '\x2A');
  return secrets;
}

[[nodiscard]] HttpResponse json_response(long status, std::string body) {
  HttpResponse resp;
  resp.status_code = status;
  resp.body = std::move(body);
  return resp;
}

// A unique temp directory, recursively removed on scope exit so runs never
// collide and leave no artifacts behind.
struct TempDir {
  fs::path path;

  explicit TempDir(const std::string& tag)
      : path(fs::temp_directory_path() / ("broker_exec_session_" + tag + "_" +
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

[[nodiscard]] KiteSessionEstablisher make_establisher(const HttpClient& http,
                                                      const SecretProvider& secrets,
                                                      TokenStore& store) {
  return KiteSessionEstablisher(http, secrets, store, kAccount, kApiKeyName, kApiSecretName,
                                kAccessTokenStoreName);
}

}  // namespace

TEST_CASE("establish exchanges the request_token and persists the access_token", "[session]") {
  RecordedHttpClient http;
  http.on(HttpRequest::Method::Post, "/session/token",
          json_response(200, std::string(R"({"status":"success","data":{"access_token":")") +
                                 kAccessToken + R"("}})"));

  const TempDir dir("establish");
  const auto secrets = make_secrets();
  TokenStore store(secrets, dir.path);
  auto establisher = make_establisher(http, secrets, store);

  const auto result = establisher.establish(kRequestToken);
  REQUIRE(result.has_value());
  CHECK(result.value() == SessionState::Healthy);

  // The token is persisted ENCRYPTED — load it back through the real TokenStore.
  const auto loaded = store.load(kAccount, kAccessTokenStoreName);
  REQUIRE(loaded.has_value());
  CHECK(loaded.value() == kAccessToken);
}

TEST_CASE("establish posts the correct SHA-256 checksum", "[session]") {
  RecordedHttpClient http;
  http.on(HttpRequest::Method::Post, "/session/token",
          json_response(200, std::string(R"({"status":"success","data":{"access_token":")") +
                                 kAccessToken + R"("}})"));

  const TempDir dir("checksum");
  const auto secrets = make_secrets();
  TokenStore store(secrets, dir.path);
  auto establisher = make_establisher(http, secrets, store);

  REQUIRE(establisher.establish(kRequestToken).has_value());

  const std::string& body = http.last_request().body;
  CHECK(body.find(std::string("checksum=") + kExpectedChecksum) != std::string::npos);
  CHECK(body.find(std::string("api_key=") + kApiKey) != std::string::npos);
  CHECK(body.find(std::string("request_token=") + kRequestToken) != std::string::npos);

  // The unauthenticated endpoint must carry NO Authorization header.
  bool has_auth = false;
  for (const auto& header : http.last_request().headers) {
    if (header.first == "Authorization") {
      has_auth = true;
    }
  }
  CHECK_FALSE(has_auth);
}

TEST_CASE("establish surfaces a typed Error and never leaks secrets", "[session]") {
  RecordedHttpClient http;
  http.on(
      HttpRequest::Method::Post, "/session/token",
      json_response(
          400,
          R"({"status":"error","error_type":"InputException","message":"Invalid `request_token`."})"));

  const TempDir dir("establish_error");
  const auto secrets = make_secrets();
  TokenStore store(secrets, dir.path);
  auto establisher = make_establisher(http, secrets, store);

  const auto result = establisher.establish(kRequestToken);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::Validation);

  // No request_token / api_secret / access_token / checksum in any Error text.
  const auto& err = result.error();
  for (const std::string secret : {std::string(kRequestToken), std::string(kApiSecret),
                                   std::string(kAccessToken), std::string(kExpectedChecksum)}) {
    CHECK(err.message.find(secret) == std::string::npos);
    CHECK(err.broker_code.find(secret) == std::string::npos);
  }

  // Nothing was persisted on the failure path.
  CHECK_FALSE(store.load(kAccount, kAccessTokenStoreName).has_value());
}

TEST_CASE("validate maps a 401 TokenException to NeedsReauth", "[session]") {
  RecordedHttpClient http;
  http.on(
      HttpRequest::Method::Get, "/user/margins/equity",
      json_response(
          401, R"({"status":"error","error_type":"TokenException","message":"Invalid session"})"));

  const TempDir dir("validate_dead");
  const auto secrets = make_secrets();
  TokenStore store(secrets, dir.path);
  REQUIRE(store.save(kAccount, kAccessTokenStoreName, kAccessToken).has_value());
  auto establisher = make_establisher(http, secrets, store);

  const auto result = establisher.validate();
  REQUIRE(result.has_value());  // a dead token is a STATE, not an error to retry
  CHECK(result.value() == SessionState::NeedsReauth);
}

TEST_CASE("validate maps a 5xx to a reconcile-first Error, not a session state", "[session]") {
  RecordedHttpClient http;
  http.on(HttpRequest::Method::Get, "/user/margins/equity",
          json_response(503, R"({"status":"error","message":"service unavailable"})"));

  const TempDir dir("validate_5xx");
  const auto secrets = make_secrets();
  TokenStore store(secrets, dir.path);
  REQUIRE(store.save(kAccount, kAccessTokenStoreName, kAccessToken).has_value());
  auto establisher = make_establisher(http, secrets, store);

  const auto result = establisher.validate();
  REQUIRE_FALSE(result.has_value());  // a server outage is NOT a dead token
  CHECK(result.error().category == ErrorCategory::Network);
  CHECK(result.error().action == broker_exec::errors::SuggestedAction::ReconcileFirst);
}

TEST_CASE("establish rejects a success envelope missing access_token and persists nothing",
          "[session]") {
  const TempDir dir("establish_no_token");
  const auto secrets = make_secrets();

  SECTION("data has no access_token") {
    RecordedHttpClient http;
    http.on(HttpRequest::Method::Post, "/session/token",
            json_response(200, R"({"status":"success","data":{}})"));
    TokenStore store(secrets, dir.path);
    auto establisher = make_establisher(http, secrets, store);

    const auto result = establisher.establish(kRequestToken);
    REQUIRE_FALSE(result.has_value());
    CHECK_FALSE(store.load(kAccount, kAccessTokenStoreName).has_value());
  }

  SECTION("access_token is the empty string") {
    RecordedHttpClient http;
    http.on(HttpRequest::Method::Post, "/session/token",
            json_response(200, R"({"status":"success","data":{"access_token":""}})"));
    TokenStore store(secrets, dir.path);
    auto establisher = make_establisher(http, secrets, store);

    const auto result = establisher.establish(kRequestToken);
    REQUIRE_FALSE(result.has_value());
    CHECK_FALSE(store.load(kAccount, kAccessTokenStoreName).has_value());
  }
}

TEST_CASE("validate returns Healthy on a successful authenticated read", "[session]") {
  RecordedHttpClient http;
  http.on(HttpRequest::Method::Get, "/user/margins/equity",
          json_response(200, R"({"status":"success","data":{"enabled":true,"net":1000}})"));

  const TempDir dir("validate_ok");
  const auto secrets = make_secrets();
  TokenStore store(secrets, dir.path);
  REQUIRE(store.save(kAccount, kAccessTokenStoreName, kAccessToken).has_value());
  auto establisher = make_establisher(http, secrets, store);

  const auto result = establisher.validate();
  REQUIRE(result.has_value());
  CHECK(result.value() == SessionState::Healthy);

  // The authenticated probe must carry the Kite Authorization header.
  bool has_auth = false;
  for (const auto& header : http.last_request().headers) {
    if (header.first == "Authorization") {
      has_auth = true;
    }
  }
  CHECK(has_auth);
}

TEST_CASE("Kite advertises no headless session refresh", "[session]") {
  STATIC_REQUIRE(KiteSessionEstablisher::kSupportsHeadlessSessionRefresh == false);
}

TEST_CASE("needs_reauth_error is the typed re-establish alert, secret-free", "[session]") {
  const auto err = KiteSessionEstablisher::needs_reauth_error();
  CHECK(err.category == broker_exec::errors::ErrorCategory::SessionExpired);
  CHECK(err.action == broker_exec::errors::SuggestedAction::ReEstablishSession);
  CHECK(err.message.find(kAccessToken) == std::string::npos);
}
