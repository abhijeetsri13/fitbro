#include "broker_exec/adapters/kite/kite_rest_client.hpp"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "broker_exec/adapters/kite/http_client.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/secret_provider.hpp"
#include "broker_exec/result.hpp"

using broker_exec::Result;
using broker_exec::adapters::kite::HttpClient;
using broker_exec::adapters::kite::HttpRequest;
using broker_exec::adapters::kite::HttpResponse;
using broker_exec::adapters::kite::KiteRestClient;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::SuggestedAction;
using broker_exec::ports::SecretProvider;

namespace {

// Token-shaped synthetic credentials: long alphanumeric runs that domain::scrub
// must redact wherever they appear in error text. NO live creds.
constexpr const char* kApiKey = "apikeyAAAA1111BBBB2222CCCC";
constexpr const char* kAccessToken = "accesstoken0000ZZZZ9999YYYY8888";

// A test double: returns canned HttpResponses keyed by (method, path); never
// touches the network. Captures the last request so auth wiring can be asserted.
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

// A fake secret store seeded with the synthetic api_key + access_token.
class FakeSecretProvider final : public SecretProvider {
 public:
  std::map<std::string, std::string> values;

  [[nodiscard]] Result<std::string> get(std::string_view key) const override {
    const auto it = values.find(std::string(key));
    if (it == values.end()) {
      return broker_exec::fail(broker_exec::errors::make_error(ErrorCategory::Auth,
                                                               "missing secret", "TEST"));
    }
    return it->second;
  }
};

[[nodiscard]] FakeSecretProvider make_secrets() {
  FakeSecretProvider secrets;
  secrets.values["kite.api_key"] = kApiKey;
  secrets.values["kite.access_token"] = kAccessToken;
  return secrets;
}

[[nodiscard]] HttpResponse json_response(long status, std::string body,
                                         std::vector<std::pair<std::string, std::string>> headers = {}) {
  HttpResponse resp;
  resp.status_code = status;
  resp.body = std::move(body);
  resp.headers = std::move(headers);
  return resp;
}

}  // namespace

TEST_CASE("place_order success parses data.order_id", "[kite]") {
  RecordedHttpClient http;
  http.on(HttpRequest::Method::Post, "/orders/regular",
          json_response(200, R"({"status":"success","data":{"order_id":"151220000000000"}})"));

  const auto secrets = make_secrets();
  KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

  const nlohmann::json params = {{"tradingsymbol", "INFY"}, {"quantity", 1}};
  const auto result = client.place_order(params);
  REQUIRE(result.has_value());
  CHECK(result.value().at("order_id").get<std::string>() == "151220000000000");
}

TEST_CASE("auth headers carry the Kite token and version", "[kite]") {
  RecordedHttpClient http;
  http.on(HttpRequest::Method::Get, "/orders",
          json_response(200, R"({"status":"success","data":[]})"));

  const auto secrets = make_secrets();
  KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

  REQUIRE(client.orders().has_value());

  const auto auth = http.last_request().headers;
  const auto find = [&](std::string_view name) -> std::string {
    for (const auto& [k, v] : auth) {
      if (k == name) {
        return v;
      }
    }
    return std::string{};
  };
  CHECK(find("Authorization") == std::string("token ") + kApiKey + ":" + kAccessToken);
  CHECK(find("X-Kite-Version") == "3");
}

TEST_CASE("orders / positions / margins success parse", "[kite]") {
  RecordedHttpClient http;
  http.on(HttpRequest::Method::Get, "/orders",
          json_response(200, R"({"status":"success","data":[{"order_id":"a","status":"COMPLETE"}]})"));
  http.on(HttpRequest::Method::Get, "/portfolio/positions",
          json_response(200, R"({"status":"success","data":{"net":[],"day":[]}})"));
  http.on(HttpRequest::Method::Get, "/user/margins/equity",
          json_response(200, R"({"status":"success","data":{"enabled":true,"net":1000}})"));

  const auto secrets = make_secrets();
  KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

  const auto orders = client.orders();
  REQUIRE(orders.has_value());
  REQUIRE(orders.value().is_array());
  CHECK(orders.value().at(0).at("order_id").get<std::string>() == "a");

  const auto positions = client.positions();
  REQUIRE(positions.has_value());
  CHECK(positions.value().contains("net"));

  const auto margins = client.margins("equity");
  REQUIRE(margins.has_value());
  CHECK(margins.value().at("enabled").get<bool>());
}

TEST_CASE("instruments returns the raw CSV body", "[kite]") {
  RecordedHttpClient http;
  http.on(HttpRequest::Method::Get, "/instruments",
          json_response(200, "instrument_token,tradingsymbol\n123,INFY\n"));

  const auto secrets = make_secrets();
  KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

  const auto csv = client.instruments();
  REQUIRE(csv.has_value());
  CHECK(csv.value().find("tradingsymbol") != std::string::npos);
}

TEST_CASE("401 TokenException maps to re-establish-session", "[kite]") {
  RecordedHttpClient http;
  http.on(HttpRequest::Method::Get, "/orders",
          json_response(401,
                        R"({"status":"error","error_type":"TokenException","message":"Invalid session"})"));

  const auto secrets = make_secrets();
  KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

  const auto result = client.orders();
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::SessionExpired);
  CHECK(result.error().action == SuggestedAction::ReEstablishSession);
}

TEST_CASE("429 maps to rate-limited and surfaces Retry-After", "[kite]") {
  RecordedHttpClient http;
  http.on(HttpRequest::Method::Get, "/orders",
          json_response(429,
                        R"({"status":"error","error_type":"TooManyRequests","message":"Too many requests"})",
                        {{"Retry-After", "2"}, {"X-RateLimit-Remaining", "0"}}));

  const auto secrets = make_secrets();
  KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

  const auto result = client.orders();
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::RateLimited);
  CHECK(result.error().action == SuggestedAction::RetrySafe);
  CHECK(result.error().broker_code.find("Retry-After=2") != std::string::npos);

  // The rate-limit signal is surfaced for the rate limiter (Story 2.12).
  REQUIRE(client.rate_limit().present);
  REQUIRE(client.rate_limit().retry_after_seconds.has_value());
  CHECK(client.rate_limit().retry_after_seconds.value() == 2);
  REQUIRE(client.rate_limit().remaining.has_value());
  CHECK(client.rate_limit().remaining.value() == 0);
}

TEST_CASE("malformed body yields a typed Error", "[kite]") {
  RecordedHttpClient http;
  http.on(HttpRequest::Method::Get, "/orders", json_response(200, "not json {{{"));

  const auto secrets = make_secrets();
  KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

  const auto result = client.orders();
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::Unknown);
}

TEST_CASE("a missing secret surfaces a typed Error, never throws", "[kite]") {
  RecordedHttpClient http;
  http.on(HttpRequest::Method::Get, "/orders",
          json_response(200, R"({"status":"success","data":[]})"));

  FakeSecretProvider secrets;  // intentionally empty: no api_key/access_token
  KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

  const auto result = client.orders();
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::Auth);
}

TEST_CASE("error text never leaks the access_token, even when echoed by the server", "[kite]") {
  // The server error echoes the token verbatim in its message; scrub MUST hide
  // it before it reaches Error.message. The Authorization header/token must
  // never appear in any returned error string.
  const std::string body = std::string(
                               R"({"status":"error","error_type":"GeneralException","message":"auth failed for token )") +
                           kAccessToken + R"("})";

  RecordedHttpClient http;
  http.on(HttpRequest::Method::Get, "/orders", json_response(400, body));

  const auto secrets = make_secrets();
  KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

  const auto result = client.orders();
  REQUIRE_FALSE(result.has_value());

  const auto& err = result.error();
  CHECK(err.message.find(kAccessToken) == std::string::npos);
  CHECK(err.message.find(kApiKey) == std::string::npos);
  CHECK(err.broker_code.find(kAccessToken) == std::string::npos);
  CHECK(err.message.find("token") != std::string::npos);  // the word survives...
  CHECK(err.message.find("REDACTED") != std::string::npos);  // ...the value does not
}

TEST_CASE("a NetworkException reconciles first — never do-not-retry (the order may be live)",
          "[kite]") {
  // Kite can surface NetworkException with a 4xx status. The order's fate is
  // uncertain, so the verdict MUST be reconcile-first, not "fix input/do-not-retry".
  RecordedHttpClient http;
  http.on(HttpRequest::Method::Post, "/orders/regular",
          json_response(400,
                        R"({"status":"error","error_type":"NetworkException","message":"order routing failed"})"));

  const auto secrets = make_secrets();
  KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

  const auto result = client.place_order({{"tradingsymbol", "INFY"}, {"quantity", 1}});
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::Network);
  CHECK(result.error().action == SuggestedAction::ReconcileFirst);
}

TEST_CASE("a 5xx server error reconciles first", "[kite]") {
  RecordedHttpClient http;
  http.on(HttpRequest::Method::Get, "/orders",
          json_response(503, R"({"status":"error","message":"service unavailable"})"));

  const auto secrets = make_secrets();
  KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

  const auto result = client.orders();
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::Network);
  CHECK(result.error().action == SuggestedAction::ReconcileFirst);
}

TEST_CASE("a MarginException maps to insufficient funds", "[kite]") {
  RecordedHttpClient http;
  http.on(HttpRequest::Method::Post, "/orders/regular",
          json_response(400,
                        R"({"status":"error","error_type":"MarginException","message":"insufficient funds"})"));

  const auto secrets = make_secrets();
  KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

  const auto result = client.place_order({{"tradingsymbol", "INFY"}, {"quantity", 1}});
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::InsufficientFunds);
}
