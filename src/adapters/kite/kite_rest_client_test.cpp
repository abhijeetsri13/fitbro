#include "broker_exec/adapters/kite/kite_rest_client.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "broker_exec/adapters/kite/http_client.hpp"
#include "broker_exec/adapters/kite/kite_broker_adapter.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/secret_provider.hpp"
#include "broker_exec/result.hpp"

using broker_exec::Result;
using broker_exec::adapters::kite::HttpClient;
using broker_exec::adapters::kite::HttpRequest;
using broker_exec::adapters::kite::HttpResponse;
using broker_exec::adapters::kite::KiteBrokerAdapter;
using broker_exec::adapters::kite::KiteRestClient;
using broker_exec::domain::Money;
using broker_exec::domain::Order;
using broker_exec::domain::OrderIntent;
using broker_exec::domain::Position;
using broker_exec::domain::Product;
using broker_exec::domain::Quantity;
using broker_exec::domain::Trade;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::SuggestedAction;
using broker_exec::ports::FundsSnapshot;
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
    ++send_count_;
    last_request_ = request;
    const auto it = table_.find(key(request.method, request.path));
    if (it == table_.end()) {
      return broker_exec::fail(broker_exec::errors::make_error(
          ErrorCategory::Unknown, "no recorded response for request", "TEST"));
    }
    return it->second;
  }

  [[nodiscard]] const HttpRequest& last_request() const noexcept { return last_request_; }

  // How many requests actually reached the transport. A guard that REFUSES to
  // send must be provable by ABSENCE — an error value alone cannot distinguish
  // "we never sent it" from "we sent it and the broker said no".
  [[nodiscard]] std::size_t send_count() const noexcept { return send_count_; }

 private:
  static std::string key(HttpRequest::Method method, const std::string& path) {
    return std::to_string(static_cast<int>(method)) + " " + path;
  }

  std::map<std::string, HttpResponse> table_;
  mutable HttpRequest last_request_;
  mutable std::size_t send_count_ = 0;
};

// A fake secret store seeded with the synthetic api_key + access_token.
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
  secrets.values["kite.api_key"] = kApiKey;
  secrets.values["kite.access_token"] = kAccessToken;
  return secrets;
}

[[nodiscard]] HttpResponse json_response(
    long status, std::string body, std::vector<std::pair<std::string, std::string>> headers = {}) {
  HttpResponse resp;
  resp.status_code = status;
  resp.body = std::move(body);
  resp.headers = std::move(headers);
  return resp;
}

// ── Adapter-level fixtures ──────────────────────────────────────────────────
// Drive ONE KiteBrokerAdapter call against a single canned Kite response. The
// transport, the secrets and the rest client all outlive the call and the value
// returned owns its data, so nothing dangles once the fixture unwinds.

[[nodiscard]] Result<std::vector<Order>> orders_from(const std::string& body) {
  RecordedHttpClient http;
  http.on(HttpRequest::Method::Get, "/orders", json_response(200, body));
  const auto secrets = make_secrets();
  KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");
  KiteBrokerAdapter adapter(client);
  return adapter.fetch_orders();
}

[[nodiscard]] Result<std::vector<Trade>> trades_from(const std::string& body) {
  RecordedHttpClient http;
  http.on(HttpRequest::Method::Get, "/trades", json_response(200, body));
  const auto secrets = make_secrets();
  KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");
  KiteBrokerAdapter adapter(client);
  return adapter.fetch_trades();
}

[[nodiscard]] Result<std::vector<Position>> positions_from(const std::string& body) {
  RecordedHttpClient http;
  http.on(HttpRequest::Method::Get, "/portfolio/positions", json_response(200, body));
  const auto secrets = make_secrets();
  KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");
  KiteBrokerAdapter adapter(client);
  return adapter.fetch_positions();
}

[[nodiscard]] Result<FundsSnapshot> funds_from(const std::string& body) {
  RecordedHttpClient http;
  http.on(HttpRequest::Method::Get, "/user/margins/equity", json_response(200, body));
  const auto secrets = make_secrets();
  KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");
  KiteBrokerAdapter adapter(client);
  return adapter.fetch_funds();
}

// Place ONE order through an adapter with NO exchange resolver wired — i.e. with
// the symbol-shape heuristic in force, exactly as the live assembly runs today —
// and hand back the form-encoded body that reached the transport.
[[nodiscard]] std::string placed_body_for(const std::string& symbol, Product product) {
  RecordedHttpClient http;
  http.on(HttpRequest::Method::Post, "/orders/regular",
          json_response(200, R"({"status":"success","data":{"order_id":"251220000000001"}})"));
  const auto secrets = make_secrets();
  KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");
  KiteBrokerAdapter adapter(client);
  REQUIRE_FALSE(adapter.has_exchange_resolver());  // the guess IS the path under test

  OrderIntent intent;
  intent.client_ref = "equity-0a1b2c3d-deadbeef-cafe-4bab-8abe-0123456789ab";
  intent.symbol = symbol;
  intent.quantity = Quantity::of(1);
  intent.product = product;
  REQUIRE(adapter.place(intent).has_value());
  return http.last_request().body;
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
  http.on(
      HttpRequest::Method::Get, "/orders",
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
  http.on(
      HttpRequest::Method::Get, "/orders",
      json_response(
          401, R"({"status":"error","error_type":"TokenException","message":"Invalid session"})"));

  const auto secrets = make_secrets();
  KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

  const auto result = client.orders();
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::SessionExpired);
  CHECK(result.error().action == SuggestedAction::ReEstablishSession);
}

TEST_CASE("429 maps to rate-limited and surfaces Retry-After", "[kite]") {
  RecordedHttpClient http;
  http.on(
      HttpRequest::Method::Get, "/orders",
      json_response(
          429, R"({"status":"error","error_type":"TooManyRequests","message":"Too many requests"})",
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
  const std::string body =
      std::string(
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
  CHECK(err.message.find("token") != std::string::npos);     // the word survives...
  CHECK(err.message.find("REDACTED") != std::string::npos);  // ...the value does not
}

TEST_CASE("a NetworkException reconciles first — never do-not-retry (the order may be live)",
          "[kite]") {
  // Kite can surface NetworkException with a 4xx status. The order's fate is
  // uncertain, so the verdict MUST be reconcile-first, not "fix input/do-not-retry".
  RecordedHttpClient http;
  http.on(
      HttpRequest::Method::Post, "/orders/regular",
      json_response(
          400,
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
  http.on(
      HttpRequest::Method::Post, "/orders/regular",
      json_response(
          400,
          R"({"status":"error","error_type":"MarginException","message":"insufficient funds"})"));

  const auto secrets = make_secrets();
  KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

  const auto result = client.place_order({{"tradingsymbol", "INFY"}, {"quantity", 1}});
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::InsufficientFunds);
}

// ── IMP-23: A BROKER-SUPPLIED ID IS NOT A URL ───────────────────────────────

TEST_CASE("an order id that is not id-shaped is refused BEFORE any request is issued", "[kite]") {
  // Every id below is something the broker's own JSON can carry: `order_id` is
  // read with `json_str`, which accepts any string Kite sends, and nothing between
  // there and the wire constrained it. Spliced raw into the path, `?variety=amo`
  // truncated the request onto a DIFFERENT order and reported success, and
  // `../../` was dot-segment-removed by libcurl into a DELETE of Kite's logout
  // endpoint — from square_off's own cancel.
  //
  // EACH CANNED RESPONSE IS REGISTERED AT THE PATH THE OLD CODE WOULD HAVE BUILT,
  // so a request that still went out would SUCCEED. The refusal is therefore the
  // only thing that can make these fail, and `send_count()` proves it by absence.
  const auto secrets = make_secrets();
  const nlohmann::json no_params = nlohmann::json::object();
  const std::string ack = R"({"status":"success","data":{"order_id":"250101000000001"}})";

  SECTION("a query delimiter truncates the path onto another order") {
    RecordedHttpClient http;
    http.on(HttpRequest::Method::Delete, "/orders/regular/250101000000001?variety=amo",
            json_response(200, ack));
    KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

    const auto result = client.cancel_order("250101000000001?variety=amo", no_params);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().category == ErrorCategory::Validation);
    CHECK(result.error().action == SuggestedAction::DoNotRetry);
    CHECK(result.error().broker_code == "KITE-CANCEL-BADORDERID");
    CHECK(http.send_count() == 0U);
  }

  SECTION("a dot-segment walks the cancel off the orders endpoint entirely") {
    RecordedHttpClient http;
    http.on(HttpRequest::Method::Delete, "/orders/regular/../../session/token",
            json_response(200, ack));
    KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

    const auto result = client.cancel_order("../../session/token", no_params);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().broker_code == "KITE-CANCEL-BADORDERID");
    CHECK(http.send_count() == 0U);
  }

  SECTION("modify splices the same id into the same path, and is gated the same way") {
    RecordedHttpClient http;
    http.on(HttpRequest::Method::Put, "/orders/regular/250101000000001?variety=amo",
            json_response(200, ack));
    KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

    const auto result = client.modify_order("250101000000001?variety=amo", no_params);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().category == ErrorCategory::Validation);
    CHECK(result.error().broker_code == "KITE-MODIFY-BADORDERID");
    CHECK(http.send_count() == 0U);
  }

  SECTION("an EMPTY id is refused: it addresses the collection, not an order") {
    RecordedHttpClient http;
    http.on(HttpRequest::Method::Delete, "/orders/regular/", json_response(200, ack));
    KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

    const auto result = client.cancel_order("", no_params);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().category == ErrorCategory::Validation);
    CHECK(http.send_count() == 0U);
  }

  SECTION("the sibling splice on /user/margins is gated too") {
    RecordedHttpClient http;
    http.on(HttpRequest::Method::Get, "/user/margins/equity/../../session/token",
            json_response(200, R"({"status":"success","data":{"net":"0.00"}})"));
    KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

    const auto result = client.margins("equity/../../session/token");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().broker_code == "KITE-MARGINS-BADSEGMENT");
    CHECK(http.send_count() == 0U);
  }

  SECTION("NON-VACUITY: a real Kite id still reaches the broker byte-for-byte") {
    RecordedHttpClient http;
    http.on(HttpRequest::Method::Delete, "/orders/regular/251220000000001",
            json_response(200, R"({"status":"success","data":{"order_id":"251220000000001"}})"));
    KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

    const auto result = client.cancel_order("251220000000001", no_params);
    REQUIRE(result.has_value());
    // The percent-encoding leaves the unreserved set alone, so the path is the
    // same string it always was — the gate costs a normal cancel nothing.
    CHECK(http.last_request().path == "/orders/regular/251220000000001");
    CHECK(http.send_count() == 1U);
  }
}

// ── IMP-25: DoNotRetry REQUIRES A VERDICT WE ACTUALLY READ ──────────────────

TEST_CASE("a 400 whose body we could not READ must reconcile, never do-not-retry", "[kite]") {
  // A proxy/WAF between us and Kite forwards the order POST, the gateway ACCEPTS
  // the order, and the response is then replaced with an HTML or empty 400 page.
  // Classified DoNotRetry, the dispatcher records the intent terminally Rejected
  // and owes no reconcile — over an order that is working at the broker.
  const auto secrets = make_secrets();
  const nlohmann::json params = {{"tradingsymbol", "INFY"}, {"quantity", 1}};

  SECTION("an HTML error page from something that is not Kite") {
    RecordedHttpClient http;
    http.on(HttpRequest::Method::Post, "/orders/regular",
            json_response(400, "<html><body>400 Bad Request</body></html>"));
    KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

    const auto result = client.place_order(params);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().action == SuggestedAction::ReconcileFirst);
    CHECK(result.error().action != SuggestedAction::DoNotRetry);
    CHECK(result.error().category == ErrorCategory::Unknown);
  }

  SECTION("an empty 400 body") {
    RecordedHttpClient http;
    http.on(HttpRequest::Method::Post, "/orders/regular", json_response(400, ""));
    KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

    const auto result = client.place_order(params);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().action == SuggestedAction::ReconcileFirst);
    CHECK(result.error().action != SuggestedAction::DoNotRetry);
  }

  SECTION("a 422 we could not read is treated the same way") {
    RecordedHttpClient http;
    http.on(HttpRequest::Method::Post, "/orders/regular", json_response(422, "not json {{{"));
    KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

    const auto result = client.place_order(params);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().action == SuggestedAction::ReconcileFirst);
  }

  SECTION("NON-VACUITY: a real InputException is still definitive") {
    RecordedHttpClient http;
    http.on(
        HttpRequest::Method::Post, "/orders/regular",
        json_response(
            400, R"({"status":"error","error_type":"InputException","message":"bad quantity"})"));
    KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

    const auto result = client.place_order(params);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().category == ErrorCategory::Validation);
    CHECK(result.error().action == SuggestedAction::DoNotRetry);
  }

  SECTION("NON-VACUITY: a 400 with a readable message but no error_type is still definitive") {
    RecordedHttpClient http;
    http.on(HttpRequest::Method::Post, "/orders/regular",
            json_response(400, R"({"status":"error","message":"quantity is mandatory"})"));
    KiteRestClient client(http, secrets, "kite.api_key", "kite.access_token");

    const auto result = client.place_order(params);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().category == ErrorCategory::Validation);
    CHECK(result.error().action == SuggestedAction::DoNotRetry);
  }
}

// ── IMP-24: AN UNREADABLE PAYLOAD IS NOT AN EMPTY BOOK ──────────────────────

TEST_CASE("a positions payload we cannot read FAILS the read; it is never a flat book", "[kite]") {
  // The dangerous half of this bug is that the caller cannot tell. Returned as an
  // empty SUCCESS, a reshaped `data` says "you hold nothing" while a real position
  // is on: the reconcile loop drops to its loose cadence, "absent == flat" fires a
  // false manual-close warning, and the tracked position is written to zero.
  const auto expect_refused = [](const Result<std::vector<Position>>& result) {
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().broker_code == "KITE-POSITIONS-SHAPE");
    // DataStale, not Unknown: the broker is up and answered — see
    // unreadable_payload_error and reconcile::recovery's broker_answered().
    CHECK(result.error().category == ErrorCategory::DataStale);
    CHECK(result.error().action == SuggestedAction::ReconcileFirst);
  };

  SECTION("no `net` key at all") {
    expect_refused(positions_from(R"({"status":"success","data":{}})"));
  }

  SECTION("a `day` book but no `net` book") {
    expect_refused(positions_from(R"({"status":"success","data":{"day":[]}})"));
  }

  SECTION("`net` is not an array") {
    expect_refused(positions_from(R"({"status":"success","data":{"net":{}}})"));
  }

  SECTION("`data` is not an object at all") {
    expect_refused(positions_from(R"({"status":"success","data":[]})"));
  }

  SECTION("`data` is null") {
    expect_refused(positions_from(R"({"status":"success","data":null})"));
  }

  SECTION("NON-VACUITY: a genuinely flat account is still an ok, empty snapshot") {
    const auto result = positions_from(R"({"status":"success","data":{"net":[],"day":[]}})");
    REQUIRE(result.has_value());
    CHECK(result.value().empty());
  }
}

TEST_CASE("an unreadable ORDERBOOK / TRADEBOOK / MARGINS payload fails closed too", "[kite]") {
  // Same envelope hole, same fail-open, at the three sibling reads. An orderbook
  // that reads empty loses a live order from broker truth; a tradebook that reads
  // empty hides executions; a margins payload that reads empty states a used
  // margin of ZERO, which frees headroom that does not exist.
  SECTION("the orderbook") {
    const auto result = orders_from(R"({"status":"success","data":{}})");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().broker_code == "KITE-ORDERS-SHAPE");
    CHECK(result.error().category == ErrorCategory::DataStale);
    CHECK(result.error().action == SuggestedAction::ReconcileFirst);
  }

  SECTION("the tradebook") {
    const auto result = trades_from(R"({"status":"success","data":{}})");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().broker_code == "KITE-TRADES-SHAPE");
    CHECK(result.error().category == ErrorCategory::DataStale);
  }

  SECTION("the margins payload") {
    const auto result = funds_from(R"({"status":"success","data":[]})");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().broker_code == "KITE-FUNDS-SHAPE");
    CHECK(result.error().category == ErrorCategory::DataStale);
  }

  SECTION("NON-VACUITY: the well-formed shapes still succeed") {
    const auto orders = orders_from(R"({"status":"success","data":[]})");
    REQUIRE(orders.has_value());
    CHECK(orders.value().empty());

    const auto trades = trades_from(R"({"status":"success","data":[]})");
    REQUIRE(trades.has_value());
    CHECK(trades.value().empty());

    const auto funds = funds_from(R"({"status":"success","data":{"net":"100000.00"}})");
    REQUIRE(funds.has_value());
    CHECK(funds.value().available_margin == Money::from_paise(10000000));
  }
}

// ── IMP-26: THE FALLBACK HEURISTIC MUST NOT MIS-ROUTE A CASH EQUITY ─────────

TEST_CASE("the exchange heuristic routes cash equities to NSE, not NFO", "[kite]") {
  // These are all real NSE cash symbols. Under the old SUBSTRING match each one
  // contained "CE" or "PE" and was sent with exchange=NFO — where the symbol does
  // not exist and CNC is not a valid product — earning an InputException that
  // maps to Validation/DoNotRetry. The entry was therefore dropped permanently,
  // on every attempt, with the blame pinned on the strategy's input.
  SECTION("a symbol whose name merely spells a derivative token") {
    for (const char* symbol : {"PETRONET", "CESC", "CEATLTD", "PERSISTENT", "CENTRALBK"}) {
      const std::string body = placed_body_for(symbol, Product::Delivery);
      INFO("symbol = " << symbol << " body = " << body);
      CHECK(body.find("exchange=NSE") != std::string::npos);
      CHECK(body.find("exchange=NFO") == std::string::npos);
    }
  }

  SECTION("NON-VACUITY: a real option/future still routes to NFO") {
    for (const char* symbol : {"NIFTY24JUN24000CE", "BANKNIFTY24JUN52000PE", "NIFTY24JUNFUT"}) {
      const std::string body = placed_body_for(symbol, Product::Normal);
      INFO("symbol = " << symbol << " body = " << body);
      CHECK(body.find("exchange=NFO") != std::string::npos);
    }
  }
}
