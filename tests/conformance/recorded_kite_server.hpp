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
#include <optional>
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
//
// IMP-13 EXTENSIONS (additive — every default reproduces the pre-IMP-13 behavior
// exactly, so no suite that predates them changes):
//   * order rows now carry `quantity`, `transaction_type`, `product` and
//     `exchange`, which a real Kite row has always carried and a FLATTEN needs;
//   * a cancel now actually cancels a still-working order (under the default
//     fill-everything model every order is already COMPLETE, so it is a no-op
//     there), optionally refusing an already-terminal one;
//   * `set_fill_model` / `set_status_override` inject a partial fill or an
//     unreadable status — the two conditions the flatten must fail closed on;
//   * `placed_orders()` exposes broker truth so a test can assert what the adapter
//     SENT (the exit's side/size/product/exchange), not just what it read back.
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
      return cancel_order(request.path);
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

  // ── IMP-13 knobs (all default to the pre-IMP-13 behavior) ────────────────

  // Report every order row under a DIFFERENT `status` than the one recorded. Kite's
  // status vocabulary is not frozen either, and a status the adapter cannot read is
  // the trigger for the flatten's fail-closed branch: an UNKNOWN leg must produce a
  // ReconcileFirst error and NO exit order.
  void set_status_override(std::string status) { status_override_ = std::move(status); }

  // Override the immediate-fill model a placed order is recorded with (the twin of
  // RecordedKotakServer::set_fill_model). `filled` < 0 means "fill the whole
  // order"; `status` is the Kite `status` string the order book reports. Deliberately
  // NOT clamped to the order quantity — a fault injector must be able to emit the
  // impossible `filled_quantity > quantity` a real broker can, so the ADAPTER's
  // clamp is what gets tested.
  void set_fill_model(std::int64_t filled, std::string status) {
    fill_qty_ = filled;
    fill_status_ = std::move(status);
  }

  // Answer a cancel of an ALREADY-TERMINAL order with a refusal instead of a
  // cheerful echo. The flatten must TOLERATE this outcome — it means the remainder
  // reached the state we were cancelling it into — rather than aborting the
  // square-off on it (AC-1b).
  //
  // REACHING THIS KNOB NEEDS A PARTIAL FILL. Under the default fill model every
  // order is recorded COMPLETE for its full quantity, so the adapter sees a
  // terminal parent, SKIPS the cancel, and the refusal never happens. That is how
  // this stayed a dead knob (and AC-1b's tolerance stayed uncovered) through a
  // test that looked like it exercised it. Drive it from `set_fill_model(30,
  // "COMPLETE")`: the adapter's quantity-first reading makes that a PARTIAL, so it
  // issues the cancel, while broker truth here still holds a terminal row.
  void set_cancel_rejects_terminal(bool on) noexcept { cancel_rejects_terminal_ = on; }

  // Grow (or set) the executed quantity of ONE recorded order WITHOUT touching its
  // status — the fixture-only way to model the race the flatten's size guard
  // exists for: the working remainder fills in the window between the square-off
  // that measured it and the replay that re-measures it. Deliberately un-clamped
  // and deliberately targeted by id, so a test can move the PARENT without moving
  // the exit. No adapter code can see this; a real broker does it by itself.
  void grow_fill(const std::string& order_id, std::int64_t filled) {
    for (Record& rec : book_) {
      if (rec.order_id == order_id) {
        rec.filled = std::to_string(filled);
        return;
      }
    }
  }

  // How many cancels this server REFUSED as already-terminal. The non-vacuity
  // probe for the knob above: a test asserting only "the square-off still
  // succeeded" passes just as well when no cancel was ever issued.
  [[nodiscard]] std::size_t cancel_refusals() const noexcept { return cancel_refusals_; }

  // ── IMP-14 knobs: inject a MONEY / COUNT field the adapter cannot read ────
  //
  // A recorded server that only ever emits well-formed numbers cannot tell a
  // fail-CLOSED money parser from a fail-OPEN one — both look identical on clean
  // input, and the fail-open one only reveals itself on the garbage a real broker
  // occasionally sends (a localized thousands separator, a "N/A" placeholder, a
  // float rendered in scientific notation, an oversized identifier landing in a
  // price column). These two knobs put that garbage on the wire.

  // Rewrite the `average_price` EVERY row reports. `std::nullopt` OMITS the key
  // entirely — the ABSENT case, which must keep reading as zero so the fail-closed
  // change does not over-reach onto orders that simply have not traded. Leaving
  // the knob untouched reports each row's own recorded price (the default).
  void set_average_price_override(std::optional<std::string> text) {
    avg_price_override_ = std::move(text);
    avg_price_overridden_ = true;
  }

  // Rewrite the `quantity` EVERY row reports — the ORDER TOTAL on the orderbook,
  // the executed size on a trade row, and the signed net on a position. This is
  // the number a square-off sizes its exit against, so a fail-open read of it is
  // the most expensive one on the adapter: "1,450" truncated to 1 turns the
  // flatten of a 30-lot position into a 1-lot order that reports success.
  //
  // IT REACHES ALL THREE READERS DELIBERATELY. It used to rewrite the ORDERBOOK
  // only, so `fetch_trades` and `fetch_positions` — which parse a quantity with
  // exactly the same reader — were never once handed a garbled one. The knob
  // looked like it covered the count path and covered a third of it.
  void set_quantity_override(std::string qty) { quantity_override_ = std::move(qty); }

  // Rewrite the AVAILABLE MARGIN the margins endpoint reports, in BOTH spellings
  // the adapter reads. Funds are the number the margin gate sizes real risk
  // against: a garbled balance read as zero blocks every entry, and a garbled
  // UTILISED figure read as zero frees headroom that does not exist. Empty => the
  // default well-formed payload for that spelling.
  void set_margin_override(std::string balance) {
    margin_override_ = balance;
    margin_net_override_ = std::move(balance);
  }

  // THE TWO SPELLINGS, INDEPENDENTLY. `available.live_balance` is the adapter's
  // first choice and `net` is its fallback, and the ONE-ARGUMENT form above writes
  // the same text into both — which cannot express the case the fallback exists
  // for: a live_balance the adapter cannot read next to a `net` that is perfectly
  // fine. A fixture that can only garble them together makes "the fallback is
  // poisoned by the garbage it routes around" an untestable bug.
  void set_margin_override(std::string live_balance, std::string net) {
    margin_override_ = std::move(live_balance);
    margin_net_override_ = std::move(net);
  }

  // ── M3: EMIT GENUINE JSON NUMBERS, NOT STRINGS ────────────────────────────
  //
  // This server has always spelled every number as a JSON STRING. Real Kite
  // Connect v3 does not: `average_price`, `quantity`, `filled_quantity`,
  // `trigger_price` and the margin figures come back as JSON NUMBERS. The adapter
  // has a whole branch for that — `numeric_text` takes `is_number()` to
  // `json::dump()` (the shortest round-trip TEXT) and hands THAT to the exact
  // decimal parser — and it is the branch that runs on EVERY live read, while a
  // string-only fixture gave it zero coverage.
  //
  // Turning this on re-renders every numeric field as a real JSON number. Text
  // that is not a number at all (an injected garbage override) stays a string, so
  // the fail-closed cases above keep working in either mode.
  void set_numeric_payload_mode(bool on) noexcept { numeric_payload_ = on; }

  // Set the reported status of ONE recorded order. `set_status_override` rewrites
  // EVERY row, which cannot express the case the flatten's exit guard turns on:
  // a healthy PARENT next to an exit that was rejected, cancelled, or is reporting
  // a status from a vocabulary we do not know. Fixture-only, targeted by id.
  void set_order_status(const std::string& order_id, std::string status) {
    for (Record& rec : book_) {
      if (rec.order_id == order_id) {
        rec.status = std::move(status);
        return;
      }
    }
  }

  // Set the reported `quantity` of ONE recorded order. The same argument as
  // set_order_status, for the same guard's other half: `set_quantity_override`
  // garbles EVERY row, which cannot express a READABLE parent standing next to an
  // exit whose size we cannot read — and that is the only shape in which the
  // flatten's exit-malformed branch is reachable at all (a garbled parent is
  // refused one guard earlier). Fixture-only, targeted by id, un-clamped.
  void set_order_quantity(const std::string& order_id, std::string qty) {
    for (Record& rec : book_) {
      if (rec.order_id == order_id) {
        rec.qty = std::move(qty);
        return;
      }
    }
  }

  // ── Observation hooks ──

  // One recorded order, as BROKER TRUTH holds it. Lets a test assert what an
  // adapter actually SENT (side, quantity, order type, exchange, product, tag)
  // rather than inferring it from a round-tripped read.
  struct PlacedOrder {
    std::string order_id;
    std::string tag;
    std::string symbol;
    std::string side;
    std::string qty;
    std::string filled;  // executed quantity, as broker truth holds it
    std::string price;
    std::string order_type;
    std::string product;
    std::string exchange;
    std::string status;
  };

  [[nodiscard]] std::vector<PlacedOrder> placed_orders() const {
    std::vector<PlacedOrder> out;
    out.reserve(book_.size());
    for (const Record& rec : book_) {
      out.push_back(PlacedOrder{rec.order_id, rec.tag, rec.symbol, rec.side, rec.qty, rec.filled,
                                rec.price, rec.order_type, rec.product, rec.exchange, rec.status});
    }
    return out;
  }

 private:
  struct Record {
    std::string order_id;
    std::string tag;
    std::string status;
    std::string qty;
    std::string filled;      // executed quantity (see set_fill_model); TEXT, as Kite sends it
    std::string price;
    std::string symbol;
    std::string side;        // Kite `transaction_type`: "BUY" / "SELL"
    std::string trigger;     // Kite `trigger_price`; EMPTY when the form omitted it
    std::string order_type;  // Kite `order_type`: MARKET / LIMIT / SL / SL-M
    std::string product;     // Kite `product`: MIS / CNC / NRML
    std::string exchange;    // Kite `exchange`: NSE / NFO / BSE / ...
  };

  // The terminal Kite statuses, for the cancel model below. A cancel only acts on
  // a still-working order; against a terminal one it is a no-op (or, with
  // set_cancel_rejects_terminal, a refusal).
  [[nodiscard]] static bool is_terminal_status(const std::string& status) noexcept {
    return status == "COMPLETE" || status == "REJECTED" || status == "CANCELLED" ||
           status == "CANCELLED AMO";
  }

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

  // Render ONE numeric field the way the selected payload mode spells it.
  //
  // Default (recorded) mode: a JSON STRING, which is how this fixture has always
  // written numbers and how several Kite endpoints really do.
  //
  // NUMERIC mode (set_numeric_payload_mode): the SAME text, re-emitted as a
  // genuine JSON number — which is what Kite Connect v3 actually sends for
  // average_price / quantity / filled_quantity / trigger_price and the margins.
  // The text is round-tripped through the JSON parser rather than through any
  // arithmetic of ours, so no float literal is written here and the emitted value
  // is exactly the number the wire would carry.
  //
  // Text that does not parse as a number (every garbage override in the suite)
  // stays a STRING: a broker sending "N/A" sends it as a string too, and the
  // fail-closed assertions must hold in both modes.
  [[nodiscard]] nlohmann::json number_field(const std::string& text) const {
    if (!numeric_payload_) {
      return text;
    }
    nlohmann::json parsed = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    return parsed.is_number() ? parsed : nlohmann::json(text);
  }

  // Write `average_price` onto one emitted row, honouring the IMP-14 knob. ONE
  // helper for all three readers that carry the field (orders, trades, positions)
  // so a single test knob exercises every money path the adapter has.
  void put_average_price(nlohmann::json& row, const std::string& recorded) const {
    if (!avg_price_overridden_) {
      row["average_price"] = number_field(recorded);
      return;
    }
    if (avg_price_override_.has_value()) {
      row["average_price"] = number_field(*avg_price_override_);
    }
    // Overridden with nullopt: the key is OMITTED ENTIRELY. That is the ABSENT
    // case — distinct from an empty string, and it must still read as zero.
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
    // Recorded under their EXACT wire names so a test can assert WHERE an order was
    // routed (the exchange resolver vs the symbol-shape heuristic) and under which
    // product a square-off exit went out — exiting an NRML position with an MIS
    // order opens a second position instead of closing the first.
    rec.product = field(form, "product");
    rec.exchange = field(form, "exchange");
    // The deterministic immediate-fill model: COMPLETE for the whole quantity
    // unless a test injected another (see set_fill_model).
    rec.status = fill_status_;
    rec.filled = fill_qty_ < 0 ? rec.qty : std::to_string(fill_qty_);
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

  [[nodiscard]] static std::string id_from_path(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string{} : path.substr(slash + 1);
  }

  [[nodiscard]] HttpResponse modify_or_cancel(const std::string& path) const {
    if (rate_limited()) {
      return error_response(429, "TooManyRequests", "Too many requests");
    }
    nlohmann::json data = nlohmann::json::object();
    data["order_id"] = id_from_path(path);
    return success_response(data);
  }

  // A CANCEL THAT ACTUALLY CANCELS (IMP-13). It used to be an echo, which made the
  // square-off's "cancel the working remainder, then place the exit" sequence
  // untestable: the book never moved, so a test could not tell a flatten that
  // cancelled from one that did not.
  //
  // Under the DEFAULT fill model every order is recorded COMPLETE, i.e. already
  // terminal, so this is a no-op there and no pre-IMP-13 behavior changes.
  [[nodiscard]] HttpResponse cancel_order(const std::string& path) const {
    if (rate_limited()) {
      return error_response(429, "TooManyRequests", "Too many requests");
    }
    const std::string id = id_from_path(path);
    for (Record& rec : book_) {
      if (rec.order_id != id) {
        continue;
      }
      if (is_terminal_status(rec.status)) {
        if (cancel_rejects_terminal_) {
          // "There is no OPEN order with that id." Emitted as a 404 because that
          // is the status whose typed category (OrderNotFound) actually says so —
          // the flatten's AC-1b tolerance keys on the CATEGORY, and a refusal the
          // mapper reads as a plain input rejection would abort the square-off
          // instead of being shrugged off.
          ++cancel_refusals_;
          return error_response(404, "InputException", "order not found or already terminal");
        }
        break;  // already terminal: nothing to cancel, and no complaint either
      }
      // A cancel keeps whatever was already filled — that partial fill is exactly
      // what the exit must then be sized off.
      rec.status = "CANCELLED";
      break;
    }
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
      o["status"] = status_override_.empty() ? rec.status : status_override_;
      o["tradingsymbol"] = rec.symbol;
      // String by default, a genuine JSON number in numeric mode; the adapter
      // parses either, and both spellings are things Kite really sends.
      o["filled_quantity"] = number_field(rec.filled);
      put_average_price(o, rec.price);  // rupee-decimal TEXT; no float in fixture
      // THE ORDER TOTAL, THE SIDE, THE PRODUCT AND THE EXCHANGE — all four are on a
      // real Kite order row, and a FLATTEN needs every one of them: the total to
      // normalize the fill (absent is not zero), the side to flip it, the product
      // and exchange to land the exit on the same position. Omitting them here
      // would let an adapter that guessed them pass a fixture that never asked.
      o["quantity"] = number_field(quantity_override_.empty() ? rec.qty : quantity_override_);
      o["transaction_type"] = rec.side;
      o["product"] = rec.product;
      o["exchange"] = rec.exchange;
      // Kite reports `trigger_price` on EVERY order row, sending "0.00" for a
      // non-stop order rather than omitting the key — so the fixture does the
      // same. That "0 means no trigger" case is exactly what the adapter's
      // optional parse has to collapse to nullopt.
      o["trigger_price"] = number_field(rec.trigger.empty() ? std::string("0.00") : rec.trigger);
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
      if (to_int(rec.filled) <= 0) {
        continue;  // a trade row exists only where something actually executed
      }
      nlohmann::json t = nlohmann::json::object();
      t["trade_id"] = "T" + rec.order_id;
      t["order_id"] = rec.order_id;
      t["tag"] = rec.tag;
      t["tradingsymbol"] = rec.symbol;
      // The trade quantity honours the SAME override as the orderbook total: a
      // trade quantity is read by the same reader and clamped by the same guard.
      t["quantity"] = number_field(quantity_override_.empty() ? rec.filled : quantity_override_);
      put_average_price(t, rec.price);
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
      // A POSITION IS WHAT EXECUTED, not what was ordered. Under the default
      // fill-everything model these are the same number, so nothing changes for the
      // suites that predate IMP-13 — but a flatten test needs the distinction: a
      // square-off that placed a correctly-sized exit must net this book to ZERO,
      // and an exit sized off the ORDERED quantity of a partially-filled parent
      // must not be able to hide behind a fixture that reports the ordered size.
      const std::int64_t qty = to_int(rec.filled);
      const std::int64_t signed_qty = rec.side == "SELL" ? -qty : qty;
      nlohmann::json p = nlohmann::json::object();
      p["tradingsymbol"] = rec.symbol;
      // DEFAULT: a genuine signed JSON INTEGER, which is what Kite sends here and
      // what this fixture has always emitted — keep it, it is real coverage of the
      // adapter's is_number path. The override substitutes the injected text (see
      // set_quantity_override, which now reaches this reader too).
      p["quantity"] = quantity_override_.empty() ? nlohmann::json(signed_qty)
                                                 : number_field(quantity_override_);
      put_average_price(p, rec.price);  // rupee-decimal TEXT; no float
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
    // The two spellings are written INDEPENDENTLY (see set_margin_override): the
    // adapter reads `available.live_balance` first and falls back to `net`, and a
    // fixture that could only ever write the same text into both could not
    // exercise the fallback at all.
    const std::string balance =
        margin_override_.empty() ? std::string("100000.00") : margin_override_;
    const std::string net =
        margin_net_override_.empty() ? std::string("100000.00") : margin_net_override_;
    nlohmann::json available = nlohmann::json::object();
    available["live_balance"] = number_field(balance);
    nlohmann::json utilised = nlohmann::json::object();
    utilised["debits"] = number_field("0.00");
    nlohmann::json data = nlohmann::json::object();
    data["available"] = available;
    data["utilised"] = utilised;
    data["net"] = number_field(net);
    return success_response(data);
  }

  broker_exec::ports::ClockPort& clock_;
  FaultConfig fault_;
  std::chrono::steady_clock::time_point start_steady_;

  // Empty => report each row's own placed type (see set_order_type_override).
  std::string order_type_override_;
  // Empty => report each row's own status (see set_status_override).
  std::string status_override_;
  // false => report each row's own price; true + nullopt => OMIT the key entirely;
  // true + a string => report that exact text (see set_average_price_override).
  bool avg_price_overridden_ = false;
  std::optional<std::string> avg_price_override_;
  // Empty => report each row's own order total (see set_quantity_override).
  std::string quantity_override_;
  // Empty => the default well-formed figure for that spelling. `margin_override_`
  // is `available.live_balance`; `margin_net_override_` is the `net` fallback.
  // They are separate so a test can garble one and leave the other readable.
  std::string margin_override_;
  std::string margin_net_override_;
  // false => every number goes out as a JSON string (the recorded default);
  // true => as a genuine JSON number, as Kite Connect v3 sends them.
  bool numeric_payload_ = false;
  // The immediate-fill model (see set_fill_model): < 0 means "fill the whole order".
  std::int64_t fill_qty_ = -1;
  std::string fill_status_ = "COMPLETE";
  // Whether cancelling an already-terminal order is a refusal (see the setter).
  bool cancel_rejects_terminal_ = false;

  // Stateful broker truth; mutable because HttpClient::send() is const.
  mutable std::vector<Record> book_;
  mutable std::int64_t next_id_ = 1;
  mutable std::size_t request_count_ = 0;
  mutable std::size_t place_count_ = 0;  // POST /orders/regular attempts (see place_count())
  mutable std::size_t cancel_refusals_ = 0;  // terminal-cancel refusals (see cancel_refusals())
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
