#pragma once

// broker_exec::conformance::kite_fixture — the recorded Kite HTTP endpoint and
// the owning Kite adapter stack, EXTRACTED (Story 6.3) from
// tests/conformance/kite_conformance_test.cpp so more than one suite can drive
// the real KiteBrokerAdapter without a live broker.
//
// WHY IT MOVED, AND WHAT DID NOT CHANGE: Story 6.3's portability proof runs the
// SAME strategy against both brokers' recorded servers. Copying this server into
// a second test file would mean two fixtures drifting apart, and a portability
// claim proven against a stale copy of one broker's fixture is worth nothing. So
// the fixture moved here VERBATIM — same fault model, same envelopes, same
// counters — and kite_conformance_test.cpp now includes it. That suite's
// behavior is unchanged.
//
// Header-only test support: it pulls only public library headers plus
// nlohmann_json, and is included by exactly one TU per test target.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`, no float.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
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

namespace broker_exec::conformance::kite_fixture {

using broker_exec::Result;
using broker_exec::adapters::fake::FaultConfig;
using broker_exec::adapters::kite::HttpClient;
using broker_exec::adapters::kite::HttpRequest;
using broker_exec::adapters::kite::HttpResponse;
using broker_exec::adapters::kite::KiteBrokerAdapter;
using broker_exec::adapters::kite::KiteRestClient;
using broker_exec::ports::SecretProvider;

// ── Synthetic credentials (NO live creds — token-shaped only for scrub tests) ──
inline constexpr const char* kApiKey = "apikeyAAAA1111BBBB2222CCCC";
inline constexpr const char* kAccessToken = "accesstoken0000ZZZZ9999YYYY8888";

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

[[nodiscard]] inline FakeSecretProvider make_secrets() {
  FakeSecretProvider secrets;
  secrets.values["kite.api_key"] = kApiKey;
  secrets.values["kite.access_token"] = kAccessToken;
  return secrets;
}

// ── Tiny form-body helpers (decode the x-www-form-urlencoded place body) ──────
[[nodiscard]] inline std::string url_decode(std::string_view s) {
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

[[nodiscard]] inline std::map<std::string, std::string> parse_form(std::string_view body) {
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

[[nodiscard]] inline std::string field(const std::map<std::string, std::string>& form,
                                       const char* key) {
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

  // How many orders BROKER TRUTH holds, regardless of what the caller observed.
  [[nodiscard]] std::size_t book_size() const noexcept { return book_.size(); }

  // Report every order row under a DIFFERENT `order_type` than the one that was
  // placed. Kite's order-type vocabulary is not frozen (cover orders, iceberg and
  // AMO variants have all arrived over time), so "the broker names a type we do
  // not know" is a real condition, not a hypothetical — and the adapter's answer
  // to it is load-bearing: an unrecognized type must fall closed to Market AND
  // suppress the trigger, because publishing a Market that carries a trigger
  // produces a shape the validation gate refuses outright.
  void set_order_type_override(std::string type) { order_type_override_ = std::move(type); }

 private:
  struct Record {
    std::string order_id;
    std::string tag;
    std::string status;
    std::string qty;
    std::string price;
    std::string symbol;
    std::string side;        // Kite `transaction_type`: "BUY" / "SELL"
    std::string trigger;     // Kite `trigger_price`; EMPTY when the form omitted it
    std::string order_type;  // Kite `order_type`: MARKET / LIMIT / SL / SL-M
  };

  // Quantities arrive as TEXT on this wire. Digits-only, stops at the first
  // non-digit — enough for a fixture, and never a float.
  [[nodiscard]] static std::int64_t to_int(std::string_view s) noexcept {
    std::int64_t value = 0;
    for (const char c : s) {
      if (c < '0' || c > '9') {
        break;
      }
      value = value * 10 + static_cast<std::int64_t>(c - '0');
    }
    return value;
  }

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

  [[nodiscard]] static HttpResponse success_response(const nlohmann::json& data) {
    HttpResponse resp;
    resp.status_code = 200;
    nlohmann::json envelope = nlohmann::json::object();
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
    nlohmann::json envelope = nlohmann::json::object();
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
    rec.side = field(form, "transaction_type");
    // Recorded under the EXACT wire name Kite uses. An adapter that emitted the
    // trigger under any other key (or duplicated `price` into it) leaves this
    // empty, so the orderbook echo below cannot round-trip — which is what makes
    // the round-trip assertion a real test of the field name.
    rec.trigger = field(form, "trigger_price");
    rec.order_type = field(form, "order_type");
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

    nlohmann::json data = nlohmann::json::object();
    data["order_id"] = rec.order_id;
    return success_response(data);
  }

  [[nodiscard]] HttpResponse modify_or_cancel(const std::string& path) const {
    if (rate_limited()) {
      return error_response(429, "TooManyRequests", "Too many requests");
    }
    const std::size_t slash = path.find_last_of('/');
    const std::string id = slash == std::string::npos ? std::string{} : path.substr(slash + 1);
    nlohmann::json data = nlohmann::json::object();
    data["order_id"] = id;
    return success_response(data);
  }

  [[nodiscard]] HttpResponse orderbook() const {
    if (rate_limited()) {
      return error_response(429, "TooManyRequests", "Too many requests");
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const Record& rec : book_) {
      nlohmann::json o = nlohmann::json::object();
      o["order_id"] = rec.order_id;
      o["tag"] = rec.tag;
      o["status"] = rec.status;
      o["tradingsymbol"] = rec.symbol;
      o["filled_quantity"] = rec.qty;  // string form; the adapter parses either
      o["average_price"] = rec.price;  // rupee-decimal string; no float in fixture
      // Kite reports `trigger_price` on EVERY order row, sending "0.00" for a
      // non-stop order rather than omitting the key — so the fixture does the
      // same. That "0 means no trigger" case is exactly what the adapter's
      // optional parse has to collapse to nullopt.
      o["trigger_price"] = rec.trigger.empty() ? std::string("0.00") : rec.trigger;
      // Kite reports the order type on every row. It is what tells a reconciler
      // that a row IS a stop — without it a recovered stop would come back looking
      // like a Market order that happens to carry a trigger, which the validation
      // gate refuses outright.
      o["order_type"] = order_type_override_.empty()
                            ? (rec.order_type.empty() ? std::string("MARKET") : rec.order_type)
                            : order_type_override_;
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
    nlohmann::json arr = nlohmann::json::array();
    for (const Record& rec : book_) {
      if (rec.status != "COMPLETE") {
        continue;
      }
      nlohmann::json t = nlohmann::json::object();
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

  // POSITIONS ARE DERIVED FROM THE BOOK, NOT HARDCODED EMPTY.
  //
  // An always-empty position book makes any test that compares position state
  // pass for free — two brokers that both return nothing compare equal, and a
  // read that silently broke would look exactly like a read that worked. Deriving
  // the net book from the orders this server actually accepted keeps the fixture
  // self-consistent (a BUY shows long, a SELL shows short) and gives such a
  // comparison something to discriminate on.
  [[nodiscard]] HttpResponse positions() const {
    if (rate_limited()) {
      return error_response(429, "TooManyRequests", "Too many requests");
    }
    nlohmann::json net = nlohmann::json::array();
    for (const Record& rec : book_) {
      const std::int64_t qty = to_int(rec.qty);
      nlohmann::json p = nlohmann::json::object();
      p["tradingsymbol"] = rec.symbol;
      p["quantity"] = rec.side == "SELL" ? -qty : qty;  // signed net, integer only
      p["average_price"] = rec.price;                   // rupee-decimal TEXT; no float
      net.push_back(p);
    }
    nlohmann::json data = nlohmann::json::object();
    data["net"] = net;
    data["day"] = net;
    return success_response(data);
  }

  [[nodiscard]] HttpResponse margins() const {
    if (rate_limited()) {
      return error_response(429, "TooManyRequests", "Too many requests");
    }
    nlohmann::json available = nlohmann::json::object();
    available["live_balance"] = "100000.00";
    nlohmann::json utilised = nlohmann::json::object();
    utilised["debits"] = "0.00";
    nlohmann::json data = nlohmann::json::object();
    data["available"] = available;
    data["utilised"] = utilised;
    data["net"] = "100000.00";
    return success_response(data);
  }

  broker_exec::ports::ClockPort& clock_;
  FaultConfig fault_;
  std::chrono::steady_clock::time_point start_steady_;

  // Empty => report each row's own placed type (see set_order_type_override).
  std::string order_type_override_;

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

  // `rest` references `secrets` and `*server`, and `adapter` references `rest` —
  // sibling members pointing at each other. A copy would leave the duplicate's
  // adapter wired to the ORIGINAL's REST client, which outlives nothing in
  // particular. Deleted rather than left to the implicit definition (the Kotak
  // twin does the same).
  OwningKiteAdapter(const OwningKiteAdapter&) = delete;
  OwningKiteAdapter& operator=(const OwningKiteAdapter&) = delete;

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

}  // namespace broker_exec::conformance::kite_fixture
