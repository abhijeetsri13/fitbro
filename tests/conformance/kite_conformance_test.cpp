// Kite adapter conformance (Story 2.14, AC-1; FR-37, NFR-3, TO-6). The CAPSTONE
// of Epic 2: it runs the SAME broker-agnostic conformance kit that certifies the
// FakeBroker (Epic 1) against the REAL KiteBrokerAdapter, driven by a stateful,
// fault-injecting recorded Kite HTTP endpoint. The kit is reused VERBATIM — only
// the BrokerFactory differs — proving the zero-duplicate invariant holds for Kite.
//
// Tier-1 certification: this runs in CI with a fake SecretProvider + the in-memory
// RecordedKiteServer (NO network, NO live credentials). Tier-2 (live-SDK
// verification capturing real `unknown` payloads as VCR fixtures + the live
// min-qty smoke) is the operator-run production-checklist step (see
// docs/kite-min-qty-smoke.md, architecture TO-6).
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`, no float.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "broker_exec/adapters/fake/fake_broker.hpp"  // FaultConfig (the fault selector)
#include "broker_exec/adapters/kite/http_client.hpp"
#include "broker_exec/adapters/kite/kite_broker_adapter.hpp"
#include "broker_exec/adapters/kite/kite_rest_client.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/clock_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/ports/secret_provider.hpp"
#include "broker_exec/result.hpp"

#include "conformance_kit.hpp"

namespace conf = broker_exec::conformance;

namespace {

using broker_exec::Result;
using broker_exec::adapters::fake::FaultConfig;
using broker_exec::adapters::kite::HttpClient;
using broker_exec::adapters::kite::HttpRequest;
using broker_exec::adapters::kite::HttpResponse;
using broker_exec::adapters::kite::KiteBrokerAdapter;
using broker_exec::adapters::kite::KiteRestClient;
using broker_exec::ports::SecretProvider;
using json = nlohmann::json;

// ── Synthetic credentials (NO live creds — token-shaped only for scrub tests) ──
constexpr const char* kApiKey = "apikeyAAAA1111BBBB2222CCCC";
constexpr const char* kAccessToken = "accesstoken0000ZZZZ9999YYYY8888";

// A fake secret store seeded with the synthetic api_key + access_token (mirrors
// the pattern in kite_rest_client_test.cpp).
class FakeSecretProvider final : public SecretProvider {
 public:
  std::map<std::string, std::string> values;

  [[nodiscard]] Result<std::string> get(std::string_view key) const override {
    const auto it = values.find(std::string(key));
    if (it == values.end()) {
      return broker_exec::fail(broker_exec::errors::make_error(
          broker_exec::errors::ErrorCategory::Auth, "missing secret", "TEST"));
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

// ── Tiny form-body helpers (decode the x-www-form-urlencoded place body) ──────
[[nodiscard]] std::string url_decode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '+') {
      out.push_back(' ');
    } else if (c == '%' && i + 2 < s.size()) {
      const auto hex = [](char h) -> int {
        if (h >= '0' && h <= '9') {
          return h - '0';
        }
        if (h >= 'a' && h <= 'f') {
          return h - 'a' + 10;
        }
        if (h >= 'A' && h <= 'F') {
          return h - 'A' + 10;
        }
        return 0;
      };
      out.push_back(static_cast<char>((hex(s[i + 1]) << 4) | hex(s[i + 2])));
      i += 2;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

[[nodiscard]] std::map<std::string, std::string> parse_form(std::string_view body) {
  std::map<std::string, std::string> out;
  std::size_t pos = 0;
  while (pos < body.size()) {
    const std::size_t amp = body.find('&', pos);
    const std::string_view pair =
        body.substr(pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
    const std::size_t eq = pair.find('=');
    if (eq != std::string_view::npos) {
      out[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
    }
    if (amp == std::string_view::npos) {
      break;
    }
    pos = amp + 1;
  }
  return out;
}

[[nodiscard]] std::string field(const std::map<std::string, std::string>& form, const char* key) {
  const auto it = form.find(key);
  return it == form.end() ? std::string{} : it->second;
}

// ── The stateful, deterministic, fault-injecting recorded Kite endpoint ───────
//
// A HttpClient-derived fake Kite server: the Kite-HTTP analog of the FakeBroker.
// It models an internal orderbook at the JSON level and reproduces the fault
// matrix the kit drives, faithfully MIRRORING FakeBroker's observable behavior so
// the same kit certifies the Kite adapter:
//   * clean / default     -> place records COMPLETE + returns {data:{order_id}}.
//   * ack_lost / drop_ack -> place RECORDS the order (COMPLETE, with its tag) BUT
//                            returns 503 (caller sees failure -> dispatcher UNKNOWN);
//                            the order appears in /orders so reconcile finds it
//                            (the headline duplicate-risk -> zero duplicates).
//   * delay_ack N         -> behaves like drop_ack until the injected clock has
//                            advanced >= N ticks, then success (recorded either way).
//   * rate_limit (==0)    -> EVERY request 429, NOTHING recorded (mirrors FakeBroker
//                            throttling the whole port surface).
//   * duplicate_fill      -> /trades emits two rows per order; ORDER count stays 1.
//   * out_of_order        -> /orders and /trades arrays are reversed.
// In every recorded case the order carries the request's `tag`, so the adapter
// recovers the client_ref. Deterministic: injected ClockPort + internal counters,
// no real time / rand / #ifdef.
class RecordedKiteServer final : public HttpClient {
 public:
  RecordedKiteServer(broker_exec::ports::ClockPort& clock, FaultConfig fault)
      : clock_(clock), fault_(fault), start_steady_(clock.now_steady()) {}

  [[nodiscard]] Result<HttpResponse> send(const HttpRequest& request) const override {
    using M = HttpRequest::Method;
    if (request.method == M::Post && request.path == "/orders/regular") {
      return place(request);
    }
    if (request.method == M::Put && starts_with(request.path, "/orders/regular/")) {
      return modify_or_cancel(request.path);
    }
    if (request.method == M::Delete && starts_with(request.path, "/orders/regular/")) {
      return modify_or_cancel(request.path);
    }
    if (request.method == M::Get && request.path == "/orders") {
      return orderbook();
    }
    if (request.method == M::Get && request.path == "/trades") {
      return tradebook();
    }
    if (request.method == M::Get && request.path == "/portfolio/positions") {
      return positions();
    }
    if (request.method == M::Get && starts_with(request.path, "/user/margins/")) {
      return margins();
    }
    return error_response(404, "GeneralException", "unknown endpoint");
  }

  // How many POST /orders/regular calls this server has seen — every place
  // ATTEMPT, counted before any fault short-circuit. Lets a test assert the
  // adapter issues exactly ONE broker place per place() call (no internal retry
  // hidden inside the adapter; the no-blind-retry property the kit otherwise
  // probes only via the FakeBroker's request counter).
  [[nodiscard]] std::size_t place_count() const noexcept { return place_count_; }

 private:
  struct Record {
    std::string order_id;
    std::string tag;
    std::string status;
    std::string qty;
    std::string price;
    std::string symbol;
  };

  [[nodiscard]] static bool starts_with(const std::string& s, std::string_view prefix) {
    return s.size() >= prefix.size() && std::string_view(s).substr(0, prefix.size()) == prefix;
  }

  // Ticks elapsed on the injected steady clock since construction (mirrors the
  // FakeBroker tick model). With a non-advancing TestClock this is 0 throughout,
  // so delay_ack behaves like drop_ack — recorded and recoverable either way.
  [[nodiscard]] std::int64_t elapsed_ticks() const noexcept {
    const auto tick = static_cast<std::int64_t>(
        fault_.tick_duration.count() > 0 ? fault_.tick_duration.count() : 1);
    const auto elapsed = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(clock_.now_steady() - start_steady_)
            .count());
    return elapsed > 0 ? elapsed / tick : 0;
  }

  // Mirror FakeBroker::rate_limited(): throttle the WHOLE port surface after
  // `rate_limit_after` accepted requests; a throttled request is not counted.
  [[nodiscard]] bool rate_limited() const noexcept {
    if (fault_.rate_limit_after >= 0 &&
        request_count_ >= static_cast<std::size_t>(fault_.rate_limit_after)) {
      return true;
    }
    ++request_count_;
    return false;
  }

  [[nodiscard]] static HttpResponse success_response(const json& data) {
    HttpResponse resp;
    resp.status_code = 200;
    json envelope = json::object();
    envelope["status"] = "success";
    envelope["data"] = data;
    resp.body = envelope.dump();
    return resp;
  }

  [[nodiscard]] static HttpResponse error_response(long status, std::string_view error_type,
                                                   std::string_view message) {
    HttpResponse resp;
    resp.status_code = status;
    if (status == 429) {
      resp.headers.emplace_back("Retry-After", "1");
    }
    json envelope = json::object();
    envelope["status"] = "error";
    envelope["error_type"] = std::string(error_type);
    envelope["message"] = std::string(message);
    resp.body = envelope.dump();
    return resp;
  }

  [[nodiscard]] HttpResponse place(const HttpRequest& request) const {
    ++place_count_;  // every POST /orders/regular attempt, before any fault path
    if (rate_limited()) {
      return error_response(429, "TooManyRequests", "Too many requests");  // record NOTHING
    }
    const auto form = parse_form(request.body);

    Record rec;
    rec.order_id = "O" + std::to_string(next_id_++);
    rec.tag = field(form, "tag");
    rec.symbol = field(form, "tradingsymbol");
    rec.qty = field(form, "quantity");
    rec.price = field(form, "price");
    rec.status = "COMPLETE";  // the fake's deterministic immediate-fill model
    book_.push_back(rec);

    // The order is recorded above REGARDLESS; only whether the CALLER observes the
    // ack varies — exactly the dangerous shape the safety core must reconcile.
    const bool record_but_fail =
        fault_.ack_lost_but_placed || fault_.drop_ack ||
        (fault_.delay_ack_ticks > 0 && elapsed_ticks() < fault_.delay_ack_ticks);
    if (record_but_fail) {
      return error_response(503, "NetworkException", "ack lost but order placed at broker");
    }

    json data = json::object();
    data["order_id"] = rec.order_id;
    return success_response(data);
  }

  [[nodiscard]] HttpResponse modify_or_cancel(const std::string& path) const {
    if (rate_limited()) {
      return error_response(429, "TooManyRequests", "Too many requests");
    }
    const std::size_t slash = path.find_last_of('/');
    const std::string id = slash == std::string::npos ? std::string{} : path.substr(slash + 1);
    json data = json::object();
    data["order_id"] = id;
    return success_response(data);
  }

  [[nodiscard]] HttpResponse orderbook() const {
    if (rate_limited()) {
      return error_response(429, "TooManyRequests", "Too many requests");
    }
    json arr = json::array();
    for (const Record& rec : book_) {
      json o = json::object();
      o["order_id"] = rec.order_id;
      o["tag"] = rec.tag;
      o["status"] = rec.status;
      o["tradingsymbol"] = rec.symbol;
      o["filled_quantity"] = rec.qty;     // string form; the adapter parses either
      o["average_price"] = rec.price;     // rupee-decimal string; no float in fixture
      arr.push_back(o);
    }
    if (fault_.out_of_order_events) {
      std::reverse(arr.begin(), arr.end());
    }
    return success_response(arr);
  }

  [[nodiscard]] HttpResponse tradebook() const {
    if (rate_limited()) {
      return error_response(429, "TooManyRequests", "Too many requests");
    }
    json arr = json::array();
    for (const Record& rec : book_) {
      if (rec.status != "COMPLETE") {
        continue;
      }
      json t = json::object();
      t["trade_id"] = "T" + rec.order_id;
      t["order_id"] = rec.order_id;
      t["tag"] = rec.tag;
      t["tradingsymbol"] = rec.symbol;
      t["quantity"] = rec.qty;
      t["average_price"] = rec.price;
      arr.push_back(t);
      if (fault_.duplicate_fill) {
        arr.push_back(t);  // the broker double-reported the SAME fill (byte-identical)
      }
    }
    if (fault_.out_of_order_events) {
      std::reverse(arr.begin(), arr.end());
    }
    return success_response(arr);
  }

  [[nodiscard]] HttpResponse positions() const {
    if (rate_limited()) {
      return error_response(429, "TooManyRequests", "Too many requests");
    }
    json data = json::object();
    data["net"] = json::array();
    data["day"] = json::array();
    return success_response(data);
  }

  [[nodiscard]] HttpResponse margins() const {
    if (rate_limited()) {
      return error_response(429, "TooManyRequests", "Too many requests");
    }
    json available = json::object();
    available["live_balance"] = "100000.00";
    json utilised = json::object();
    utilised["debits"] = "0.00";
    json data = json::object();
    data["available"] = available;
    data["utilised"] = utilised;
    data["net"] = "100000.00";
    return success_response(data);
  }

  broker_exec::ports::ClockPort& clock_;
  FaultConfig fault_;
  std::chrono::steady_clock::time_point start_steady_;

  // Stateful broker truth; mutable because HttpClient::send() is const.
  mutable std::vector<Record> book_;
  mutable std::int64_t next_id_ = 1;
  mutable std::size_t request_count_ = 0;
  mutable std::size_t place_count_ = 0;  // POST /orders/regular attempts (see place_count())
};

// Owns the whole Kite stack behind a single BrokerPort so the kit can hold one
// unique_ptr<BrokerPort> while the adapter's referenced collaborators (the server,
// the REST client, the secret provider) stay alive for the scenario. Member
// DECLARATION ORDER is the construction order and is load-bearing: the server and
// secrets must exist before the REST client (which references them), and the REST
// client before the adapter (which references it).
struct OwningKiteAdapter final : broker_exec::ports::BrokerPort {
  std::unique_ptr<RecordedKiteServer> server;
  FakeSecretProvider secrets;
  KiteRestClient rest;
  KiteBrokerAdapter adapter;

  OwningKiteAdapter(broker_exec::ports::ClockPort& clock, FaultConfig fault)
      : server(std::make_unique<RecordedKiteServer>(clock, fault)),
        secrets(make_secrets()),
        rest(*server, secrets, "kite.api_key", "kite.access_token"),
        adapter(rest) {}

  [[nodiscard]] Result<broker_exec::ports::BrokerAck> place(
      const broker_exec::domain::OrderIntent& intent) override {
    return adapter.place(intent);
  }
  [[nodiscard]] Result<broker_exec::ports::BrokerAck> modify(
      const std::string& broker_order_id,
      const broker_exec::domain::OrderIntent& intent) override {
    return adapter.modify(broker_order_id, intent);
  }
  [[nodiscard]] Result<broker_exec::ports::Ok> cancel(
      const std::string& broker_order_id) override {
    return adapter.cancel(broker_order_id);
  }
  [[nodiscard]] Result<broker_exec::ports::Ok> square_off(
      const std::string& broker_order_id) override {
    return adapter.square_off(broker_order_id);
  }
  [[nodiscard]] Result<std::vector<broker_exec::domain::Order>> fetch_orders() override {
    return adapter.fetch_orders();
  }
  [[nodiscard]] Result<std::vector<broker_exec::domain::Trade>> fetch_trades() override {
    return adapter.fetch_trades();
  }
  [[nodiscard]] Result<std::vector<broker_exec::domain::Position>> fetch_positions() override {
    return adapter.fetch_positions();
  }
  [[nodiscard]] Result<broker_exec::ports::FundsSnapshot> fetch_funds() override {
    return adapter.fetch_funds();
  }
};

// The Kite BrokerFactory: per scenario, build a fresh recorded server for the
// FaultConfig, a KiteRestClient over it + a fake SecretProvider, and a
// KiteBrokerAdapter — all owned by the returned BrokerPort.
conf::BrokerFactory kite_factory() {
  return [](broker_exec::ports::ClockPort& clock,
            FaultConfig fault) -> std::unique_ptr<broker_exec::ports::BrokerPort> {
    return std::make_unique<OwningKiteAdapter>(clock, fault);
  };
}

}  // namespace

TEST_CASE("conformance: the Kite adapter passes the full fault matrix with zero duplicates",
          "[conformance][kite]") {
  const conf::ConformanceReport report = conf::run_conformance(kite_factory());

  // Surface every failure line so a regression names the exact scenario+property.
  for (const std::string& f : report.failures) {
    UNSCOPED_INFO("kite conformance failure: " << f);
  }

  // The whole matrix ran, the headline zero-duplicate invariant held, and every
  // scenario passed all three properties (no-blind-retry, UNKNOWN handling, zero
  // duplicates) — i.e. the SAME kit that certifies the FakeBroker now gates Kite.
  CHECK(report.scenarios_run > 0);
  CHECK(report.duplicate_orders == 0);
  CHECK(report.scenarios_passed == report.scenarios_run);
  CHECK(report.ok());
}

TEST_CASE("conformance: every scenario in the matrix is exercised against Kite",
          "[conformance][kite]") {
  const conf::ConformanceReport report = conf::run_conformance(kite_factory());
  CHECK(report.scenarios_run == 7);
}

// Directly exercise the adapter (NOT via run_conformance) to prove two things the
// kit cannot prove for Kite by itself:
//   * Fix 1 — the tag->client_ref / id->client_ref recovery actually round-trips a
//     KNOWN, non-empty client_ref. The kit counts duplicates via
//     `o.intent.client_ref == client_ref`; an adapter that recovered an EMPTY
//     client_ref would report zero duplicates VACUOUSLY (an empty ref matches no
//     signal). Asserting the exact minted ref comes back closes that hole.
//   * Fix 2 — a single place() issues exactly ONE broker POST (no retry hidden
//     inside the adapter). The kit's no-blind-retry probe is gated on
//     `dynamic_cast<FakeBroker*>` and is SKIPPED for Kite; place_count() restores
//     that coverage at the adapter level.
TEST_CASE("[conformance][kite][recovery] ack-lost order is recovered with its client_ref",
          "[conformance][kite][recovery]") {
  // A KNOWN, non-empty client_ref in the canonical "<strategy>-<sig8>-<uuid>" shape
  // the dispatcher mints (Story 1.7). The recovery must hand this exact ref back.
  const std::string kClientRef = "alpha-1a2b3c4d-deadbeefcafebabe0123456789abcdef";

  const auto make_intent = [&] {
    broker_exec::domain::OrderIntent intent;
    intent.client_ref = kClientRef;
    intent.symbol = "NIFTY24JUN24000CE";
    intent.side = broker_exec::domain::Side::Sell;
    intent.quantity = broker_exec::domain::Quantity::of(50);
    intent.price = broker_exec::domain::Price::from_rupees(123, 50);
    intent.order_type = broker_exec::domain::OrderType::Limit;
    intent.product = broker_exec::domain::Product::Intraday;
    intent.strategy = "alpha";
    return intent;
  };

  SECTION("ack-lost place is recovered from the tag -> client_ref map") {
    // ack_lost_but_placed: the order IS live at the broker but the caller sees a
    // transport failure — the headline duplicate-risk the safety core must survive.
    broker_exec::clock::TestClock clock;
    OwningKiteAdapter owner(clock, FaultConfig{.ack_lost_but_placed = true});

    const broker_exec::domain::OrderIntent intent = make_intent();

    // place() returns an Error (ack lost). The dispatcher would mark this UNKNOWN
    // and reconcile — never blindly retry.
    auto placed = owner.adapter.place(intent);
    REQUIRE_FALSE(placed.has_value());

    // Fix 2: exactly ONE broker POST per place() call — the adapter does not retry
    // internally (the only legitimate re-fire is the dispatcher's, after reserve()).
    CHECK(owner.server->place_count() == 1);

    // Fix 1: fetch_orders() recovers the live order AND its originating client_ref
    // from the echoed tag. A non-empty, EXACT match proves the zero-duplicate pass
    // is not vacuous.
    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    const broker_exec::domain::Order& recovered = orders.value().front();
    CHECK_FALSE(recovered.intent.client_ref.empty());
    CHECK(recovered.intent.client_ref == kClientRef);
  }

  SECTION("clean place is recovered from the broker_order_id -> client_ref map") {
    broker_exec::clock::TestClock clock;
    OwningKiteAdapter owner(clock, FaultConfig{});

    const broker_exec::domain::OrderIntent intent = make_intent();

    // A clean place returns an ack carrying the broker_order_id and the echoed ref.
    auto placed = owner.adapter.place(intent);
    REQUIRE(placed.has_value());
    CHECK_FALSE(placed.value().broker_order_id.empty());
    CHECK(placed.value().client_ref == kClientRef);
    CHECK(owner.server->place_count() == 1);  // still exactly one POST

    // The same ref is recovered on reconcile — here via the (broker_order_id ->
    // client_ref) map populated by the returned ack.
    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    const broker_exec::domain::Order& recovered = orders.value().front();
    CHECK(recovered.intent.client_ref == kClientRef);
    CHECK(recovered.broker_order_id == placed.value().broker_order_id);
  }
}
