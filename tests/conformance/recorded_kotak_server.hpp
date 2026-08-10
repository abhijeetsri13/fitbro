#pragma once

// broker_exec::conformance::kotak_fixture — the recorded Kotak Neo HTTP endpoint
// and the owning Kotak adapter stack, EXTRACTED (Story 6.3) from
// tests/conformance/kotak_conformance_test.cpp so more than one suite can drive
// the real KotakBrokerAdapter without a live broker.
//
// WHY IT MOVED, AND WHAT DID NOT CHANGE: Story 6.3's portability proof runs the
// SAME strategy against both brokers' recorded servers. Copying this server into
// a second test file would mean two fixtures drifting apart, and a portability
// claim proven against a stale copy of one broker's fixture is worth nothing. So
// the fixture moved here VERBATIM — same fault model, same envelopes, same knobs,
// same counters — and kotak_conformance_test.cpp now includes it. That suite's
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
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "broker_exec/adapters/fake/fake_broker.hpp"  // FaultConfig (the fault selector)
#include "broker_exec/adapters/kotak/kotak_broker_adapter.hpp"
#include "broker_exec/adapters/kotak/kotak_rest_client.hpp"
#include "broker_exec/adapters/kotak/kotak_session.hpp"
#include "broker_exec/adapters/kotak/kotak_transport.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/clock_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::conformance::kotak_fixture {

namespace endpoints = broker_exec::adapters::kotak::endpoints;

using broker_exec::Result;
using broker_exec::adapters::fake::FaultConfig;
using broker_exec::adapters::kotak::HttpClient;
using broker_exec::adapters::kotak::HttpRequest;
using broker_exec::adapters::kotak::HttpResponse;
using broker_exec::adapters::kotak::KotakBrokerAdapter;
using broker_exec::adapters::kotak::KotakRestClient;
using broker_exec::adapters::kotak::KotakSessionBundle;
using json = nlohmann::json;

// ── Synthetic session bundle (NO live creds — token-shaped only) ─────────────
inline constexpr const char* kAccessToken = "atACCESS0000AAAA1111BBBB2222";
inline constexpr const char* kToken = "ftFINAL9999GGGG0000HHHH1111";
inline constexpr const char* kSid = "fsSID2222IIII3333JJJJ4444";
inline constexpr const char* kServerId = "server3";

[[nodiscard]] inline KotakSessionBundle make_bundle() {
  KotakSessionBundle bundle;
  bundle.access_token = kAccessToken;
  bundle.token = kToken;
  bundle.sid = kSid;
  bundle.hs_server_id = kServerId;
  return bundle;
}

// ── jData body helpers (decode the `jData=<url-encoded json>` form body) ─────

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

// Kotak's quick endpoints carry the whole request object in ONE form field.
// A body that is not a `jData=` frame, or whose payload is not a JSON object,
// decodes to an empty object rather than throwing.
[[nodiscard]] inline json parse_jdata(const std::string& body) {
  constexpr std::string_view kPrefix = "jData=";
  const std::string_view view(body);
  if (view.size() < kPrefix.size() || view.substr(0, kPrefix.size()) != kPrefix) {
    return json::object();
  }
  const std::string decoded = url_decode(view.substr(kPrefix.size()));
  json parsed = json::parse(decoded, nullptr, /*allow_exceptions=*/false);
  return parsed.is_object() ? parsed : json::object();
}

[[nodiscard]] inline std::string jstr(const json& obj, const char* key) {
  const auto it = obj.find(key);
  return (it != obj.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

// Route on the endpoint constants themselves, so a path typo in the REST client
// or in `endpoints::` cannot be papered over by a hand-copied literal here.
[[nodiscard]] inline bool path_is(const HttpRequest& request, std::string_view endpoint) noexcept {
  return std::string_view(request.path) == endpoint;
}

// ── The stateful, deterministic, fault-injecting recorded Kotak endpoint ─────
//
// An HttpClient-derived fake Kotak Neo server: the Kotak-HTTP analog of the
// FakeBroker and the twin of RecordedKiteServer. It models an internal order book
// at the Kotak JSON level (`{"stat":"Ok","nOrdNo":...}` on a place, `data:[...]`
// on a read) and reproduces the fault matrix the kit drives:
//   * clean / default     -> place records the order and returns {stat:Ok,nOrdNo}.
//   * ack_lost / drop_ack -> place RECORDS the order BUT answers 503 with a
//                            gateway-fault envelope (the caller sees failure);
//                            the order appears in the order book, so reconcile
//                            finds it — the headline duplicate-risk.
//   * delay_ack N         -> drop_ack until the injected clock has advanced >= N
//                            ticks, then success (recorded either way).
//   * rate_limit          -> 429 on the ORDER-MUTATION endpoints, NOTHING recorded.
//   * duplicate_fill      -> the trade book emits two rows per order; the ORDER
//                            count stays 1.
//   * out_of_order        -> order/trade arrays are reversed.
// Plus three Kotak-specific knobs the direct tests use: `hard_reject` (the Kotak
// trap: HTTP 200 carrying `stat:"Not_Ok"`), a configurable fill model (partial
// fills, including a broker that says "complete" while reporting less than the
// full quantity), and a seeded COLLIDING MANUAL ORDER.
//
// DELIBERATE DEVIATION FROM RecordedKiteServer — READS ARE NOT THROTTLED, AND
// THAT DEVIATION IS LOAD-BEARING, NOT COSMETIC. Say so plainly: modelling the
// whole-surface throttle that RecordedKiteServer uses would make the conformance
// suite FAIL, and the failure would be at the kit's infrastructure level rather
// than a real safety finding. The mechanism:
//   * KotakRestClient DOWNGRADES RetrySafe to ReconcileFirst on a mutation (a 429
//     on a place is still ambiguous about whether the order reached the exchange),
//     so unlike Kite — whose 429 maps to RetrySafe and lands on Rejected — a
//     throttled Kotak place becomes an UNKNOWN order;
//   * the kit answers an UNKNOWN by calling UnknownResolver::resolve(), which
//     reads broker truth. If the read were throttled too, resolve() returns an
//     infrastructure Error and the kit records
//     "UNKNOWN resolution failed at the infrastructure level".
// Kotak publishes PER-API rate limits, so a separate mutation budget is also the
// faithful model — but the honest statement is that it is REQUIRED here, not
// merely preferable, and a reviewer should know which way the causality runs.
//
// WHAT THIS LEAVES UNTESTED (tier-2 / follow-up): the TOTAL-OUTAGE lane, where the
// place is throttled AND the reconcile read is throttled too. That is a real
// broker condition (a full 429 storm, or a gateway brown-out) and the safety core
// must hold an UNKNOWN under an alert without progressing. Covering it needs the
// kit to tolerate a failed resolve as a legitimate fail-closed outcome rather than
// an infrastructure failure, which is a change to the shared kit. Tracked in
// docs/kotak-min-qty-smoke.md.
//
// FIXTURE PROVENANCE: every envelope this server emits is the SAME shape as the
// committed recorded-response fixtures in
// src/adapters/kotak/kotak_rest_client_test.cpp — `{"stat":"Ok","nOrdNo":…}` on a
// place, `{"stat":"Ok","result":…}` on a cancel, `{"stat":"Ok","stCode":200,
// "data":[{"nOrdNo","ordSt","trdSym","qty","fldQty","avgPrc"}]}` on a read,
// `{"stat":"Ok","Net","MarginUsed","CollateralValue"}` on limits, the verbatim
// `RMS:Margin Exceeds` Not_Ok body, and the `{"fault":{"code","message"}}`
// gateway shape. The server generates them rather than replaying literals only
// because the fault matrix requires STATE; the wire vocabulary is unchanged, so
// every Kotak `unknown` the kit exercises is still pinned to a committed fixture.
//
// Deterministic: injected ClockPort + internal counters; no real time, no rand,
// no `#ifdef`.
class RecordedKotakServer final : public HttpClient {
 private:
  // One row of broker truth. Declared first so the seeding helper below can name
  // it without relying on complete-class lookup.
  struct Record {
    std::string order_id;
    std::string symbol;
    std::string side;  // Kotak `trnsTp`: "B" / "S"
    std::int64_t qty = 0;
    std::int64_t filled = 0;
    std::string price;       // rupee-decimal TEXT, exactly as Kotak sends it
    std::string trigger;     // jData `tp`, echoed on a read as `trgPrc` (see below)
    std::string price_type;  // jData `pt`, echoed on a read as `prcTp` (ditto)
    std::string product;     // jData `pc`, echoed on a read as `prod` (ditto again)
    std::string segment;     // jData `es`, echoed on a read as `exSeg` (ditto again)
    std::string status;
  };

  // The terminal `ordSt` values, for the cancel model. A cancel only acts on a
  // still-working order; against a terminal one it is a no-op (or a refusal).
  // Compared case-insensitively on the exact spellings this fixture emits.
  [[nodiscard]] static bool is_terminal_status(const std::string& status) noexcept {
    return status == "complete" || status == "rejected" || status == "cancelled";
  }

 public:
  RecordedKotakServer(broker_exec::ports::ClockPort& clock, FaultConfig fault)
      : clock_(clock), fault_(fault), start_steady_(clock.now_steady()) {}

  [[nodiscard]] Result<HttpResponse> send(const HttpRequest& request) const override {
    using M = HttpRequest::Method;
    if (request.method == M::Post && path_is(request, endpoints::kPlaceOrder)) {
      return place(request);
    }
    if (request.method == M::Post && path_is(request, endpoints::kModifyOrder)) {
      return modify(request);
    }
    if (request.method == M::Post && path_is(request, endpoints::kCancelOrder)) {
      return cancel(request);
    }
    if (request.method == M::Get && path_is(request, endpoints::kOrderBook)) {
      return orderbook();
    }
    if (request.method == M::Get && path_is(request, endpoints::kTradeBook)) {
      return tradebook();
    }
    if (request.method == M::Get && path_is(request, endpoints::kPositions)) {
      return positions();
    }
    if (request.method == M::Post && path_is(request, endpoints::kLimits)) {
      return limits();
    }
    return not_ok_response(404, "Invalid endpoint", "404");
  }

  // ── Kotak-specific fault knobs (set before use; not part of FaultConfig) ──

  // The Kotak trap: answer a place with HTTP 200 + `stat:"Not_Ok"` and record
  // NOTHING. A status-code-only client would read this as success.
  void set_hard_reject(bool on) noexcept { hard_reject_ = on; }

  // Override the immediate-fill model a placed order is recorded with. `filled`
  // is the executed quantity (< 0 means "fill the whole order"), `status` the
  // `ordSt` string the order book reports. The value is NOT clamped to the order
  // quantity on purpose — a fault injector must be able to emit the impossible
  // `fldQty > qty` a real broker can, so the ADAPTER's clamp is what gets tested.
  void set_fill_model(std::int64_t filled, std::string status) {
    fill_qty_ = filled;
    fill_status_ = std::move(status);
  }

  // Report every order row under a DIFFERENT `prcTp` than the one that was
  // placed. Kotak's field VALUES are as unverified as its field names, so "the
  // report names a price type we do not know" is a first-class hazard: the
  // adapter must fall closed to Market AND suppress the trigger, because a Market
  // carrying a trigger is a shape the validation gate refuses outright.
  void set_price_type_override(std::string type) { price_type_override_ = std::move(type); }

  // Additionally emit `tp` (the REQUEST spelling for the trigger) on order-book
  // rows, with an arbitrary value. A real report may well carry this key holding
  // something that is not a number; the adapter must NOT read it, because a
  // present-but-unparseable money field fails the whole row closed to Unknown —
  // and an all-Unknown book freezes entries. Empty (default) => not emitted.
  void set_report_tp(std::string value) { report_tp_ = std::move(value); }

  // Emit the order TOTAL under a different JSON key. Kotak's field spellings are
  // an unverified tier-2 assumption, so "the total arrives under a name we did not
  // anticipate" is a first-class hazard, not a hypothetical: it is the difference
  // between "absent" and "zero", and getting it wrong turns a live working order
  // into a terminal Filled.
  void set_total_field(std::string key) { total_field_ = std::move(key); }

  // Answer a cancel of an ALREADY-TERMINAL order with a Kotak Not_Ok refusal
  // instead of a cheerful echo. The flatten must TOLERATE that outcome — it means
  // the remainder reached the state we were cancelling it into — rather than
  // abandoning the square-off on it. (IMP-13; default off, so nothing that
  // predates it changes.)
  //
  // REACHING THIS KNOB NEEDS A PARTIAL FILL, exactly as on the Kite twin: under
  // the default fill model every order is recorded `complete` for its full
  // quantity, so the adapter reads a terminal parent, SKIPS the cancel, and this
  // never fires. Drive it from `set_fill_model(30, "complete")` — the adapter's
  // quantity-first reading makes that a PARTIAL, so the cancel is issued, while
  // broker truth here still holds a terminal row.
  void set_cancel_rejects_terminal(bool on) noexcept { cancel_rejects_terminal_ = on; }

  // Grow (or set) the executed quantity of ONE recorded order WITHOUT touching its
  // status — the fixture-only way to model the race the flatten's size guard
  // exists for: the working remainder fills in the window between the square-off
  // that measured it and the replay that re-measures it. Targeted by id so a test
  // can move the PARENT without moving the exit. No adapter code can see this.
  void grow_fill(const std::string& order_id, std::int64_t filled) {
    for (Record& rec : book_) {
      if (rec.order_id == order_id) {
        rec.filled = filled;
        return;
      }
    }
  }

  // How many cancels this server REFUSED as already-terminal — the non-vacuity
  // probe for the knob above (a test asserting only "the square-off succeeded"
  // passes just as well when no cancel was ever issued).
  [[nodiscard]] std::size_t cancel_refusals() const noexcept { return cancel_refusals_; }

  // Set the reported `ordSt` of ONE recorded order. The fill-model status applies
  // to every row, which cannot express the case the flatten's exit guard turns on:
  // a healthy PARENT next to an exit the broker rejected or cancelled.
  // Fixture-only, targeted by id.
  void set_order_status(const std::string& order_id, std::string status) {
    for (Record& rec : book_) {
      if (rec.order_id == order_id) {
        rec.status = std::move(status);
        return;
      }
    }
  }

  // One recorded order as BROKER TRUTH holds it, so a test can assert what the
  // adapter actually SENT (the exit's side, size, product and segment) rather than
  // inferring it from a round-tripped read. (IMP-13.)
  struct PlacedOrder {
    std::string order_id;
    std::string symbol;
    std::string side;
    std::int64_t qty = 0;
    std::int64_t filled = 0;
    std::string price;
    std::string price_type;
    std::string product;
    std::string segment;
    std::string status;
  };

  [[nodiscard]] std::vector<PlacedOrder> placed_orders() const {
    std::vector<PlacedOrder> out;
    out.reserve(book_.size());
    for (const Record& rec : book_) {
      out.push_back(PlacedOrder{rec.order_id, rec.symbol, rec.side, rec.qty, rec.filled, rec.price,
                                rec.price_type, rec.product, rec.segment, rec.status});
    }
    return out;
  }

  // Seed a pre-existing order the ADAPTER never placed — an operator's manual
  // order, or another process's — with an attribute shape that collides with our
  // intent. Attribute corroboration must refuse to claim either order.
  void seed_manual_order(std::string symbol, std::string side, std::int64_t qty,
                         std::string price) {
    Record rec;
    rec.order_id = "MANUAL" + std::to_string(next_id_++);
    rec.symbol = std::move(symbol);
    rec.side = std::move(side);
    rec.qty = qty;
    rec.filled = 0;
    rec.price = std::move(price);
    rec.status = "open";
    book_.push_back(std::move(rec));
  }

  // ── Observation hooks ──
  // Every POST to the place endpoint, counted BEFORE any fault short-circuit, so
  // a test can assert the adapter issues exactly ONE wire place per intent (the
  // no-blind-retry property; the kit's own probe is FakeBroker-only).
  [[nodiscard]] std::size_t place_count() const noexcept { return place_count_; }

  // The raw request bodies seen on the place endpoint — used to prove the
  // client_ref was never transmitted (so recovery cannot be an echo).
  [[nodiscard]] const std::vector<std::string>& place_bodies() const noexcept {
    return place_bodies_;
  }

  // How many orders broker truth holds, regardless of what the caller observed.
  [[nodiscard]] std::size_t book_size() const noexcept { return book_.size(); }

 private:
  // Ticks elapsed on the injected steady clock since construction (mirrors the
  // FakeBroker tick model). With a non-advancing TestClock this stays 0, so
  // delay_ack behaves like drop_ack — recorded and recoverable either way.
  [[nodiscard]] std::int64_t elapsed_ticks() const noexcept {
    const auto tick = static_cast<std::int64_t>(
        fault_.tick_duration.count() > 0 ? fault_.tick_duration.count() : 1);
    const auto elapsed = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(clock_.now_steady() - start_steady_)
            .count());
    return elapsed > 0 ? elapsed / tick : 0;
  }

  // The MUTATION rate-limit budget (see the class comment for why reads are
  // exempt). A throttled request is not counted against the budget.
  [[nodiscard]] bool mutation_throttled() const noexcept {
    if (fault_.rate_limit_after >= 0 &&
        mutation_count_ >= static_cast<std::size_t>(fault_.rate_limit_after)) {
      return true;
    }
    ++mutation_count_;
    return false;
  }

  // ── Kotak envelope builders ──

  // `{"stat":"Ok", ...fields}` at HTTP 200 — the shape a mutation answers with.
  [[nodiscard]] static HttpResponse ok_response(json fields) {
    HttpResponse resp;
    resp.status_code = 200;
    fields["stat"] = "Ok";
    fields["stCode"] = 200;
    resp.body = fields.dump();
    return resp;
  }

  // `{"stat":"Ok","stCode":200,"data":[...]}` — the shape a read answers with.
  [[nodiscard]] static HttpResponse ok_data_response(const json& data) {
    json envelope = json::object();
    envelope["data"] = data;
    return ok_response(std::move(envelope));
  }

  // THE KOTAK TRAP: an HTTP 200 that is really a rejection.
  [[nodiscard]] static HttpResponse not_ok_response(long status, std::string_view message,
                                                    std::string_view code) {
    HttpResponse resp;
    resp.status_code = status;
    json envelope = json::object();
    envelope["stat"] = "Not_Ok";
    envelope["errMsg"] = std::string(message);
    envelope["stCode"] = std::string(code);
    resp.body = envelope.dump();
    return resp;
  }

  // An API-gateway fault (the second of Kotak's failure shapes).
  [[nodiscard]] static HttpResponse fault_response(long status, std::string_view code,
                                                   std::string_view message) {
    HttpResponse resp;
    resp.status_code = status;
    if (status == 429) {
      resp.headers.emplace_back("Retry-After", "1");
    }
    json fault = json::object();
    fault["code"] = std::string(code);
    fault["message"] = std::string(message);
    json envelope = json::object();
    envelope["fault"] = fault;
    resp.body = envelope.dump();
    return resp;
  }

  // ── Endpoints ──

  [[nodiscard]] HttpResponse place(const HttpRequest& request) const {
    ++place_count_;  // every attempt, before any fault path
    place_bodies_.push_back(request.body);

    if (mutation_throttled()) {
      // Nothing is recorded: the request never reached the book.
      return fault_response(429, "900802", "Message throttled out");
    }
    if (hard_reject_) {
      // HTTP 200 + Not_Ok: the broker gave a verdict, nothing is live.
      return not_ok_response(200, "RMS:Margin Exceeds,Available margin is 1200.00", "5203");
    }

    const json params = parse_jdata(request.body);

    Record rec;
    rec.order_id = "KOT" + std::to_string(next_id_++);
    rec.symbol = jstr(params, "ts");
    rec.side = jstr(params, "tt");
    rec.qty = to_int(jstr(params, "qt"));
    rec.price = jstr(params, "pr");
    rec.trigger = jstr(params, "tp");     // the REQUEST spelling; the read echoes trgPrc
    rec.price_type = jstr(params, "pt");  // ditto: `pt` on the way in, `prcTp` on the way out
    // Recorded under their EXACT request names so a test can assert under which
    // PRODUCT and on which SEGMENT a square-off exit went out — exiting an NRML
    // position with an MIS order opens a second position instead of closing the
    // first, and the read echoes both under Kotak's report-side spellings.
    rec.product = jstr(params, "pc");
    rec.segment = jstr(params, "es");
    rec.filled = fill_qty_ < 0 ? rec.qty : fill_qty_;  // deliberately un-clamped
    rec.status = fill_status_;
    book_.push_back(rec);

    // The order is recorded above REGARDLESS; only whether the CALLER observes the
    // ack varies — exactly the dangerous shape the safety core must reconcile.
    const bool record_but_fail =
        fault_.ack_lost_but_placed || fault_.drop_ack ||
        (fault_.delay_ack_ticks > 0 && elapsed_ticks() < fault_.delay_ack_ticks);
    if (record_but_fail) {
      return fault_response(503, "500", "Gateway timeout; ack lost but order placed");
    }

    json fields = json::object();
    fields["nOrdNo"] = rec.order_id;
    return ok_response(std::move(fields));
  }

  [[nodiscard]] HttpResponse modify(const HttpRequest& request) const {
    if (mutation_throttled()) {
      return fault_response(429, "900802", "Message throttled out");
    }
    const json params = parse_jdata(request.body);
    json fields = json::object();
    fields["nOrdNo"] = jstr(params, "no");
    return ok_response(std::move(fields));
  }

  // A CANCEL THAT ACTUALLY CANCELS (IMP-13). It used to be an echo, which made the
  // square-off's "cancel the working remainder, then place the exit" sequence
  // untestable: the book never moved, so a test could not tell a flatten that
  // cancelled from one that did not. Under the DEFAULT fill model every order is
  // recorded `complete` — already terminal — so this is a no-op there and nothing
  // that predates IMP-13 changes.
  [[nodiscard]] HttpResponse cancel(const HttpRequest& request) const {
    if (mutation_throttled()) {
      return fault_response(429, "900802", "Message throttled out");
    }
    const json params = parse_jdata(request.body);
    const std::string id = jstr(params, "on");
    for (Record& rec : book_) {
      if (rec.order_id != id) {
        continue;
      }
      if (is_terminal_status(rec.status)) {
        if (cancel_rejects_terminal_) {
          // The phrasing matters, not just the envelope: `map_kotak_error` routes
          // an HTTP-200 Not_Ok through the canonical rejection classifier, and only
          // an "order not found"-shaped message resolves to the OrderNotFound
          // category the flatten's AC-1b tolerance keys on. "Order is not open"
          // classified as an unrecognized reject (BrokerRejected/ReconcileFirst),
          // which would ABORT the square-off — the fixture would have been testing
          // the opposite of what it claimed.
          ++cancel_refusals_;
          return not_ok_response(200, "Order not found: no open order for that id", "5204");
        }
        break;  // already terminal: nothing to cancel, and no complaint either
      }
      // A cancel keeps whatever was already filled — that partial fill is exactly
      // what the exit must then be sized off.
      rec.status = "cancelled";
      break;
    }
    json fields = json::object();
    fields["result"] = id;  // Kotak echoes the cancelled id here
    return ok_response(std::move(fields));
  }

  [[nodiscard]] HttpResponse orderbook() const {
    json arr = json::array();
    for (const Record& rec : book_) {
      json o = json::object();
      o["nOrdNo"] = rec.order_id;
      o["ordSt"] = rec.status;
      o["trdSym"] = rec.symbol;
      o["trnsTp"] = rec.side;
      o[total_field_] = std::to_string(rec.qty);  // Kotak sends quantities as TEXT
      o["fldQty"] = std::to_string(rec.filled);   // ditto
      o["prc"] = rec.price;                       // rupee-decimal TEXT; no float in the fixture
      // THE ASYMMETRY IS DELIBERATE AND IS THE POINT: the order was PLACED with
      // the trigger under jData `tp`, and Kotak's order REPORT spells the same
      // datum `trgPrc`. Echoing it back under the request's key would let an
      // adapter that only knows `tp` pass a round-trip it would fail live.
      o["trgPrc"] = rec.trigger.empty() ? std::string("0") : rec.trigger;
      // Same asymmetry, same reason: `pt` on the request, `prcTp` on the report.
      // The type is what tells a reconciler the row IS a stop — without it a
      // recovered stop comes back as a Market order carrying a trigger, a shape
      // the validation gate refuses outright.
      o["prcTp"] = price_type_override_.empty()
                       ? (rec.price_type.empty() ? std::string("MKT") : rec.price_type)
                       : price_type_override_;
      // Same asymmetry once more: the request spells them `pc` / `es`, the report
      // spells them `prod` / `exSeg`. A FLATTEN needs both — the product to land the
      // exit on the same position rather than opening a second one, the segment to
      // route it where the position actually is instead of re-guessing from the
      // symbol shape. An adapter that only knew the request spellings would fail
      // here rather than pass on a fixture that flattered it.
      o["prod"] = rec.product;
      o["exSeg"] = rec.segment;
      if (!report_tp_.empty()) {
        o["tp"] = report_tp_;  // the request-side spelling, on a REPORT (see set_report_tp)
      }
      o["avgPrc"] = rec.filled > 0 ? rec.price : std::string("0.00");
      arr.push_back(o);
    }
    if (fault_.out_of_order_events) {
      std::reverse(arr.begin(), arr.end());
    }
    return ok_data_response(arr);
  }

  [[nodiscard]] HttpResponse tradebook() const {
    json arr = json::array();
    for (const Record& rec : book_) {
      if (rec.filled <= 0) {
        continue;
      }
      json t = json::object();
      t["trdNo"] = "T" + rec.order_id;
      t["nOrdNo"] = rec.order_id;
      t["trdSym"] = rec.symbol;
      t["trnsTp"] = rec.side;
      t["fldQty"] = std::to_string(rec.filled);
      t["avgPrc"] = rec.price;
      arr.push_back(t);
      if (fault_.duplicate_fill) {
        arr.push_back(t);  // the broker double-reported the SAME fill (byte-identical)
      }
    }
    if (fault_.out_of_order_events) {
      std::reverse(arr.begin(), arr.end());
    }
    return ok_data_response(arr);
  }

  // POSITIONS ARE DERIVED FROM THE BOOK, NOT HARDCODED EMPTY.
  //
  // An always-empty position book makes any test that compares position state
  // pass for free — two brokers that both return nothing compare equal, and a
  // read that silently broke would look exactly like a read that worked. Deriving
  // the net book from the orders this server actually accepted keeps the fixture
  // self-consistent and gives such a comparison something to discriminate on.
  //
  // Kotak reports BUCKETS (day buy/sell quantities), not a signed net — the
  // adapter is what nets them — so the fixture emits buckets, exercising that
  // conversion rather than papering over it.
  [[nodiscard]] HttpResponse positions() const {
    json arr = json::array();
    for (const Record& rec : book_) {
      const bool is_sell = rec.side == "S";
      // A POSITION IS WHAT EXECUTED, not what was ordered — and the Kotak field
      // names say so themselves (`fl` = filled). Under the default fill-everything
      // model these are the same number, so nothing that predates IMP-13 changes;
      // for a flatten test the distinction is the whole point, since a correctly
      // sized exit must net this book to ZERO.
      json p = json::object();
      p["trdSym"] = rec.symbol;
      p["flBuyQty"] = std::to_string(is_sell ? 0 : rec.filled);  // TEXT, as Kotak sends it
      p["flSellQty"] = std::to_string(is_sell ? rec.filled : 0);
      p["avgPrc"] = rec.price;  // rupee-decimal TEXT; no float in the fixture
      arr.push_back(p);
    }
    return ok_data_response(arr);
  }

  [[nodiscard]] HttpResponse limits() const {
    json fields = json::object();
    fields["Net"] = "100000.00";
    fields["MarginUsed"] = "0.00";
    fields["CollateralValue"] = "0.00";
    return ok_response(std::move(fields));
  }

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

  broker_exec::ports::ClockPort& clock_;
  FaultConfig fault_;
  std::chrono::steady_clock::time_point start_steady_;

  // Kotak-specific knobs.
  bool hard_reject_ = false;
  bool cancel_rejects_terminal_ = false;   // see set_cancel_rejects_terminal (IMP-13)
  std::int64_t fill_qty_ = -1;             // < 0 -> fill the whole order
  std::string fill_status_ = "complete";   // the `ordSt` a recorded order reports
  std::string total_field_ = "qty";        // which key carries the order total
  std::string price_type_override_;        // empty => report each row's placed `pt`
  std::string report_tp_;                  // empty => no `tp` key on report rows

  // Stateful broker truth; mutable because HttpClient::send() is const.
  mutable std::vector<Record> book_;
  mutable std::vector<std::string> place_bodies_;
  mutable std::int64_t next_id_ = 1;
  mutable std::size_t mutation_count_ = 0;
  mutable std::size_t place_count_ = 0;
  mutable std::size_t cancel_refusals_ = 0;  // terminal-cancel refusals (see cancel_refusals())
};

// BROKER-TRUTH TELEMETRY, COLLECTED AT SCENARIO TEARDOWN.
//
// The conformance kit's duplicate metric counts broker rows whose `client_ref`
// equals the signal's — which is a CORRELATION measurement, not a duplicate
// measurement. For an adapter with a perfect tag echo those coincide; for Kotak
// they do NOT, and the gap is exactly the wrong way round: two duplicate ack-lost
// orders make the attribute key AMBIGUOUS, so the adapter (correctly) refuses to
// name either row, both come back ref-empty, and the metric reads ZERO duplicates
// for the very scenario that produced two live orders. A correlation failure would
// mask a duplicate-order catastrophe.
//
// So the conformance suite measures the thing itself, from the recorded broker's
// own state: how many orders the BOOK holds, and how many places actually crossed
// the wire.
//
// Collected in the destructor because the kit destroys each scenario's stack
// before the next one starts, so a raw server pointer would dangle by the time
// run_conformance() returns. Members are destroyed AFTER the destructor body, so
// `server` is still alive there.
struct ConformanceTally {
  std::vector<std::size_t> place_counts;  // wire places per scenario
  std::vector<std::size_t> book_sizes;    // orders in BROKER TRUTH per scenario
};

// Owns the whole Kotak stack behind a single BrokerPort so the kit can hold one
// unique_ptr<BrokerPort> while the adapter's referenced collaborators stay alive
// for the scenario. Member DECLARATION ORDER is the construction order and is
// load-bearing: the server must exist before the REST client (which references
// it), and the REST client before the adapter (which references it). The session
// bundle is captured BY VALUE inside the provider lambda, so nothing dangles.
struct OwningKotakAdapter final : broker_exec::ports::BrokerPort {
  std::unique_ptr<RecordedKotakServer> server;
  KotakRestClient rest;
  KotakBrokerAdapter adapter;
  ConformanceTally* tally = nullptr;

  OwningKotakAdapter(broker_exec::ports::ClockPort& clock, FaultConfig fault,
                     ConformanceTally* tally_sink = nullptr)
      : server(std::make_unique<RecordedKotakServer>(clock, fault)),
        rest(*server,
             [bundle = make_bundle()]() -> Result<KotakSessionBundle> { return bundle; }),
        adapter(rest),
        tally(tally_sink) {}

  ~OwningKotakAdapter() override {
    if (tally != nullptr && server) {
      tally->place_counts.push_back(server->place_count());
      tally->book_sizes.push_back(server->book_size());
    }
  }

  OwningKotakAdapter(const OwningKotakAdapter&) = delete;
  OwningKotakAdapter& operator=(const OwningKotakAdapter&) = delete;

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

}  // namespace broker_exec::conformance::kotak_fixture
