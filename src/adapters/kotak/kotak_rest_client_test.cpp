// Kotak Neo REST client tests (Story 6.1, AC-1/AC-2/AC-3).
//
// Every response body is a COMMITTED RECORDED-RESPONSE FIXTURE (VCR substrate,
// TO-6 tier-1): no network, no live credentials. The synthetic session bundle is
// token-shaped so `domain::scrub` must redact it wherever it is echoed back.

#include "broker_exec/adapters/kotak/kotak_rest_client.hpp"

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "broker_exec/adapters/kotak/kotak_session.hpp"
#include "broker_exec/adapters/kotak/kotak_transport.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/result.hpp"

using broker_exec::Result;
using broker_exec::adapters::kotak::HttpClient;
using broker_exec::adapters::kotak::HttpRequest;
using broker_exec::adapters::kotak::HttpResponse;
using broker_exec::adapters::kotak::KotakRestClient;
using broker_exec::adapters::kotak::KotakSessionBundle;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::SuggestedAction;

namespace {

namespace endpoints = broker_exec::adapters::kotak::endpoints;

constexpr const char* kAccessToken = "atACCESS0000AAAA1111BBBB2222";
constexpr const char* kToken = "ftFINAL9999GGGG0000HHHH1111";
constexpr const char* kSid = "fsSID2222IIII3333JJJJ4444";
constexpr const char* kServerId = "server3";

// ── Committed Kotak Neo response fixtures ────────────────────────────────────

constexpr const char* kPlaceOk = R"({"stat":"Ok","nOrdNo":"220101000000001","stCode":200})";
constexpr const char* kModifyOk = R"({"stat":"Ok","nOrdNo":"220101000000001","stCode":200})";
constexpr const char* kCancelOk = R"({"stat":"Ok","result":"220101000000001","stCode":200})";

constexpr const char* kOrderBookOk =
    R"({"stat":"Ok","stCode":200,"data":[{"nOrdNo":"220101000000001","ordSt":"complete","trdSym":"INFY-EQ","qty":"1"}]})";
constexpr const char* kTradeBookOk =
    R"({"stat":"Ok","stCode":200,"data":[{"nOrdNo":"220101000000001","fldQty":"1","avgPrc":"1450.25"}]})";
constexpr const char* kPositionsOk =
    R"({"stat":"Ok","stCode":200,"data":[{"trdSym":"INFY-EQ","flBuyQty":"1","flSellQty":"0"}]})";
constexpr const char* kHoldingsOk =
    R"({"stat":"Ok","data":[{"symbol":"INFY","quantity":10,"averagePrice":"1400.00"}]})";

// The funds/limits envelope — a day-one-critical-path unknown, pinned here.
// Money is text on the wire and stays text through this layer (no float).
constexpr const char* kLimitsOk =
    R"({"stat":"Ok","stCode":200,"Net":"104325.75","MarginUsed":"12500.00","CollateralValue":"0.00"})";

constexpr const char* kScripMasterText =
    "pSymbol,pTrdSymbol,pExchSeg\n11536,INFY-EQ,nse_cm\n1594,ITC-EQ,nse_cm\n";

// The Kotak trap: HTTP 200 that is really a rejection.
constexpr const char* kRejectMargin =
    R"({"stat":"Not_Ok","errMsg":"RMS:Margin Exceeds,Available margin is 1200.00","stCode":5203})";
constexpr const char* kSessionDead =
    R"({"stat":"Not_Ok","errMsg":"Invalid Session or Token expired, please re-login","stCode":10502})";
constexpr const char* kThrottled =
    R"({"fault":{"code":"900802","message":"Message throttled out"}})";
constexpr const char* kGatewayDown =
    R"({"fault":{"code":"500","message":"Internal Server Error at the gateway"}})";

// ── Test doubles ─────────────────────────────────────────────────────────────

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
    last_request_ = request;
    const auto it = script_.find(key(request.method, request.path));
    if (it == script_.end() || it->second.empty()) {
      return broker_exec::fail(broker_exec::errors::make_error(
          ErrorCategory::Unknown, "no scripted response for request", "TEST"));
    }
    HttpResponse response = it->second.front();
    it->second.pop_front();
    return response;
  }

  [[nodiscard]] const HttpRequest& last_request() const noexcept { return last_request_; }

  [[nodiscard]] std::string last_header(std::string_view name) const {
    for (const auto& [key_name, value] : last_request_.headers) {
      if (key_name == name) {
        return value;
      }
    }
    return std::string{};
  }

  [[nodiscard]] std::string last_query(std::string_view name) const {
    for (const auto& [key_name, value] : last_request_.query) {
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
  mutable HttpRequest last_request_;
};

[[nodiscard]] KotakSessionBundle make_bundle() {
  KotakSessionBundle bundle;
  bundle.access_token = kAccessToken;
  bundle.token = kToken;
  bundle.sid = kSid;
  bundle.hs_server_id = kServerId;
  return bundle;
}

// The provider is invoked PER CALL, exactly as a real caller wires it to the
// establisher's encrypted-store load().
[[nodiscard]] broker_exec::adapters::kotak::BundleProvider provider_for(KotakSessionBundle bundle) {
  return [bundle]() -> Result<KotakSessionBundle> { return bundle; };
}

[[nodiscard]] nlohmann::json sample_order() {
  return nlohmann::json{{"am", "NO"},   {"dq", "0"},    {"es", "nse_cm"}, {"mp", "0"},
                        {"pc", "MIS"},  {"pf", "N"},    {"pr", "1450.5"}, {"pt", "L"},
                        {"qt", "1"},    {"rt", "DAY"},  {"tp", "0"},      {"ts", "INFY-EQ"},
                        {"tt", "B"}};
}

}  // namespace

TEST_CASE("place_order posts jData, routes by sId, and parses the inline result", "[kotak][rest]") {
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Post, endpoints::kPlaceOrder, 200, kPlaceOk);

  KotakRestClient client(http, provider_for(make_bundle()));
  const auto result = client.place_order(sample_order());

  REQUIRE(result.has_value());
  // A mutation inlines its result beside `stat` — the whole envelope comes back.
  CHECK(result.value().at("nOrdNo").get<std::string>() == "220101000000001");

  CHECK(http.last_request().body.rfind("jData=", 0) == 0);
  CHECK(http.last_query("sId") == kServerId);
}

TEST_CASE("modify and cancel mirror the place surface", "[kotak][rest]") {
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Post, endpoints::kModifyOrder, 200, kModifyOk);
  http.script(HttpRequest::Method::Post, endpoints::kCancelOrder, 200, kCancelOk);

  KotakRestClient client(http, provider_for(make_bundle()));

  const auto modified = client.modify_order({{"no", "220101000000001"}, {"pr", "1451.0"}});
  REQUIRE(modified.has_value());
  CHECK(modified.value().at("nOrdNo").get<std::string>() == "220101000000001");

  const auto cancelled = client.cancel_order({{"on", "220101000000001"}});
  REQUIRE(cancelled.has_value());
  CHECK(cancelled.value().at("result").get<std::string>() == "220101000000001");
}

TEST_CASE("the auth headers are built at the call site from the bundle", "[kotak][rest]") {
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Get, endpoints::kOrderBook, 200, kOrderBookOk);

  KotakRestClient client(http, provider_for(make_bundle()));
  REQUIRE(client.orders().has_value());

  CHECK(http.last_header("Authorization") == std::string("Bearer ") + kAccessToken);
  CHECK(http.last_header("Auth") == kToken);
  CHECK(http.last_header("Sid") == kSid);
  CHECK(http.last_header("neo-fin-key") == "neotradeapi");
}

TEST_CASE("reads unwrap the data payload", "[kotak][rest]") {
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Get, endpoints::kOrderBook, 200, kOrderBookOk);
  http.script(HttpRequest::Method::Get, endpoints::kTradeBook, 200, kTradeBookOk);
  http.script(HttpRequest::Method::Get, endpoints::kPositions, 200, kPositionsOk);
  http.script(HttpRequest::Method::Get, endpoints::kHoldings, 200, kHoldingsOk);

  KotakRestClient client(http, provider_for(make_bundle()));

  const auto orders = client.orders();
  REQUIRE(orders.has_value());
  REQUIRE(orders.value().is_array());
  CHECK(orders.value().at(0).at("ordSt").get<std::string>() == "complete");

  const auto trades = client.trades();
  REQUIRE(trades.has_value());
  CHECK(trades.value().at(0).at("fldQty").get<std::string>() == "1");

  const auto positions = client.positions();
  REQUIRE(positions.has_value());
  CHECK(positions.value().at(0).at("trdSym").get<std::string>() == "INFY-EQ");

  const auto holdings = client.holdings();
  REQUIRE(holdings.has_value());
  CHECK(holdings.value().at(0).at("symbol").get<std::string>() == "INFY");
}

TEST_CASE("the margins/limits envelope parses (day-one critical path)", "[kotak][rest]") {
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Post, endpoints::kLimits, 200, kLimitsOk);

  KotakRestClient client(http, provider_for(make_bundle()));
  const auto limits = client.margins({{"seg", "CASH"}, {"exch", "NSE"}, {"prod", "ALL"}});

  REQUIRE(limits.has_value());
  // Money stays TEXT through this layer: no double is ever constructed here.
  CHECK(limits.value().at("Net").get<std::string>() == "104325.75");
  CHECK(limits.value().at("MarginUsed").get<std::string>() == "12500.00");
}

TEST_CASE("the scrip master returns the raw text body", "[kotak][rest]") {
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Get, endpoints::kScripMaster, 200, kScripMasterText);

  KotakRestClient client(http, provider_for(make_bundle()));
  const auto text = client.scrip_master();

  REQUIRE(text.has_value());
  CHECK(text.value().find("pTrdSymbol") != std::string::npos);
  CHECK(text.value().find("INFY-EQ") != std::string::npos);
}

TEST_CASE("the scrip master rejects a 200 ERROR ENVELOPE instead of returning it as data",
          "[kotak][rest]") {
  // Kotak answers this endpoint's failures with an HTTP 200 error envelope. A
  // status-only check hands that JSON back as instrument text, and the refdata
  // loader then parses `{"stat":"Not_Ok"...}` as a scrip file — a silently empty
  // or corrupt instrument master, which is far worse than a loud failure.
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Get, endpoints::kScripMaster, 200, kSessionDead);
  http.script(HttpRequest::Method::Get, endpoints::kScripMaster, 200, kRejectMargin);

  KotakRestClient client(http, provider_for(make_bundle()));

  const auto dead = client.scrip_master();
  REQUIRE_FALSE(dead.has_value());
  CHECK(dead.error().category == ErrorCategory::SessionExpired);

  const auto rejected = client.scrip_master();
  REQUIRE_FALSE(rejected.has_value());
  CHECK_FALSE(rejected.has_value());
}

TEST_CASE("a JSON scrip-master response with no failure envelope still passes through",
          "[kotak][rest]") {
  // The file-paths flavour of this endpoint answers with plain JSON and no
  // `stat` — that is data, not an error, and must not be rejected.
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Get, endpoints::kScripMaster, 200,
              R"({"data":{"filesPaths":["https://example.invalid/nse_cm.csv"]}})");

  KotakRestClient client(http, provider_for(make_bundle()));
  const auto text = client.scrip_master();
  REQUIRE(text.has_value());
  CHECK(text.value().find("filesPaths") != std::string::npos);
}

TEST_CASE("HTTP 200 with stat Not_Ok is a rejection, not a payload", "[kotak][rest]") {
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Post, endpoints::kPlaceOrder, 200, kRejectMargin);

  KotakRestClient client(http, provider_for(make_bundle()));
  const auto result = client.place_order(sample_order());

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::InsufficientFunds);
  CHECK(result.error().action == SuggestedAction::DoNotRetry);
}

TEST_CASE("a dead session on a read maps to re-establish-session", "[kotak][rest]") {
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Get, endpoints::kOrderBook, 200, kSessionDead);

  KotakRestClient client(http, provider_for(make_bundle()));
  const auto result = client.orders();

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::SessionExpired);
  CHECK(result.error().action == SuggestedAction::ReEstablishSession);
}

TEST_CASE("a 429 on a MUTATION never comes back safe-to-retry", "[kotak][rest]") {
  // THE DUPLICATE-ORDER HAZARD. The error mapper sees text and status only; it
  // cannot know this was a write. A throttled place may still have reached the
  // exchange, so retrying it places the order twice. The client must downgrade
  // the posture to reconcile-first on place/modify/cancel.
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Post, endpoints::kPlaceOrder, 429, kThrottled,
              {{"Retry-After", "3"}});
  http.script(HttpRequest::Method::Post, endpoints::kModifyOrder, 429, kThrottled);
  http.script(HttpRequest::Method::Post, endpoints::kCancelOrder, 429, kThrottled);

  KotakRestClient client(http, provider_for(make_bundle()));

  const auto placed = client.place_order(sample_order());
  REQUIRE_FALSE(placed.has_value());
  CHECK(placed.error().category == ErrorCategory::RateLimited);
  CHECK(placed.error().action == SuggestedAction::ReconcileFirst);
  CHECK(placed.error().action != SuggestedAction::RetrySafe);

  const auto modified = client.modify_order({{"no", "220101000000001"}});
  REQUIRE_FALSE(modified.has_value());
  CHECK(modified.error().action == SuggestedAction::ReconcileFirst);

  const auto cancelled = client.cancel_order({{"on", "220101000000001"}});
  REQUIRE_FALSE(cancelled.has_value());
  CHECK(cancelled.error().action == SuggestedAction::ReconcileFirst);
}

TEST_CASE("a 429 on a READ stays safely retryable", "[kotak][rest]") {
  // The downgrade must not blunt the throttle posture for idempotent reads —
  // margins is a POST only because Kotak takes a jData filter, but it is a read.
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Get, endpoints::kOrderBook, 429, kThrottled);
  http.script(HttpRequest::Method::Post, endpoints::kLimits, 429, kThrottled);

  KotakRestClient client(http, provider_for(make_bundle()));

  const auto orders = client.orders();
  REQUIRE_FALSE(orders.has_value());
  CHECK(orders.error().action == SuggestedAction::RetrySafe);

  const auto limits = client.margins({{"seg", "CASH"}});
  REQUIRE_FALSE(limits.has_value());
  CHECK(limits.error().action == SuggestedAction::RetrySafe);
}

TEST_CASE("429 surfaces the rate-limit signal for the throttle", "[kotak][rest]") {
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Get, endpoints::kOrderBook, 429, kThrottled,
              {{"Retry-After", "3"}, {"X-RateLimit-Remaining", "0"}, {"X-RateLimit-Limit", "10"}});

  KotakRestClient client(http, provider_for(make_bundle()));
  const auto result = client.orders();

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::RateLimited);
  CHECK(result.error().action == SuggestedAction::RetrySafe);
  CHECK(result.error().broker_code.find("Retry-After=3") != std::string::npos);

  REQUIRE(client.rate_limit().present);
  REQUIRE(client.rate_limit().retry_after_seconds.has_value());
  CHECK(client.rate_limit().retry_after_seconds.value() == 3);
  REQUIRE(client.rate_limit().remaining.has_value());
  CHECK(client.rate_limit().remaining.value() == 0);
  REQUIRE(client.rate_limit().limit.has_value());
  CHECK(client.rate_limit().limit.value() == 10);
}

TEST_CASE("a 5xx on place_order RECONCILES — the order may be live", "[kotak][rest]") {
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Post, endpoints::kPlaceOrder, 503, kGatewayDown);

  KotakRestClient client(http, provider_for(make_bundle()));
  const auto result = client.place_order(sample_order());

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::Network);
  CHECK(result.error().action == SuggestedAction::ReconcileFirst);
  // The one verdict that would be unsafe here.
  CHECK(result.error().action != SuggestedAction::DoNotRetry);
}

TEST_CASE("a transport failure on place_order also reconciles", "[kotak][rest]") {
  ScriptedHttpClient http;  // nothing scripted: the transport reports a failure

  KotakRestClient client(http, provider_for(make_bundle()));
  const auto result = client.place_order(sample_order());

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().action == SuggestedAction::ReconcileFirst);
}

TEST_CASE("a malformed success body is a typed Error, never a silent empty payload",
          "[kotak][rest]") {
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Get, endpoints::kOrderBook, 200, "not json {{{");

  KotakRestClient client(http, provider_for(make_bundle()));
  const auto result = client.orders();

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::Unknown);
}

TEST_CASE("an incomplete bundle is refused before any request is made", "[kotak][rest]") {
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Get, endpoints::kOrderBook, 200, kOrderBookOk);

  KotakSessionBundle partial = make_bundle();
  partial.hs_server_id.clear();  // cannot route an order

  KotakRestClient client(http, provider_for(partial));
  const auto result = client.orders();

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::SessionExpired);
  CHECK(result.error().action == SuggestedAction::ReEstablishSession);
}

TEST_CASE("a bundle-provider failure propagates as its own typed Error", "[kotak][rest]") {
  ScriptedHttpClient http;
  KotakRestClient client(http, []() -> Result<KotakSessionBundle> {
    return broker_exec::fail(
        broker_exec::errors::make_error(ErrorCategory::Internal, "token store unavailable"));
  });

  const auto result = client.orders();
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::Internal);
}

TEST_CASE("no session artifact reaches an Error, even when the broker echoes it",
          "[kotak][rest][scrub]") {
  const std::string hostile = std::string(R"({"stat":"Not_Ok","errMsg":"bad request for sid )") +
                              kSid + " auth " + kToken + " bearer " + kAccessToken +
                              R"(","stCode":5001})";

  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Post, endpoints::kPlaceOrder, 400, hostile);

  KotakRestClient client(http, provider_for(make_bundle()));
  const auto result = client.place_order(sample_order());

  REQUIRE_FALSE(result.has_value());
  // Each artifact is really in the fixture — asserting the absence of something
  // the fixture never contained proves nothing.
  REQUIRE(hostile.find(kAccessToken) != std::string::npos);
  const auto& error = result.error();
  CHECK(error.message.find(kSid) == std::string::npos);
  CHECK(error.message.find(kToken) == std::string::npos);
  CHECK(error.message.find(kAccessToken) == std::string::npos);
  CHECK(error.broker_code.find(kSid) == std::string::npos);
  CHECK(error.broker_code.find(kToken) == std::string::npos);
  CHECK(error.message.find("REDACTED") != std::string::npos);
}
