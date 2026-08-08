// Kotak Neo adapter conformance (Story 6.2, AC-1; FR-1/FR-37, NFR-3, TO-6). The
// twin of tests/conformance/kite_conformance_test.cpp: it runs the SAME
// broker-agnostic conformance kit that certifies the FakeBroker (Epic 1) and the
// Kite adapter (Epic 2) against the REAL KotakBrokerAdapter, driven by a stateful,
// fault-injecting recorded Kotak HTTP endpoint. The kit is reused VERBATIM — only
// the BrokerFactory differs — proving the zero-duplicate invariant holds for Kotak.
//
// TIER-1 ONLY, AND THAT IS THE POINT: this runs in CI with a synthetic session
// bundle + the in-memory RecordedKotakServer (NO network, NO live credentials).
// Passing here does NOT flip any entry in `kotak_capabilities()` — every one of
// them stays `Unknown`, because a fixture we authored ourselves cannot certify an
// endpoint we have never contacted. The tier-2 gate that DOES flip them is the
// operator-run live min-qty smoke in docs/kotak-min-qty-smoke.md (architecture TO-6).
//
// WHY THE NON-KIT ASSERTIONS AT THE BOTTOM MATTER: Kotak has no verified client-tag
// echo, so the adapter recovers an ack-lost order by ATTRIBUTE CORROBORATION. Two
// hazards follow, and both are asserted explicitly rather than assumed:
//   * VACUITY — the kit counts duplicates via `o.intent.client_ref == client_ref`,
//     so an adapter that recovered an EMPTY client_ref would report zero duplicates
//     for free. We assert the exact minted ref comes back, AND that it never went
//     out on the wire (so the recovery is genuinely corroboration, not an echo).
//   * FALSE POSITIVES — attribute corroboration must claim NOTHING when the
//     pairing is ambiguous. A colliding manual order proves the fail-closed side.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`, no float.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "broker_exec/adapters/fake/fake_broker.hpp"  // FaultConfig (the fault selector)
#include "broker_exec/adapters/kotak/kotak_broker_adapter.hpp"
#include "broker_exec/adapters/kotak/kotak_capabilities.hpp"
#include "broker_exec/adapters/kotak/kotak_rest_client.hpp"
#include "broker_exec/adapters/kotak/kotak_session.hpp"
#include "broker_exec/adapters/kotak/kotak_transport.hpp"
#include "broker_exec/capabilities/capabilities.hpp"
#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/domain/decimal_paise.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/lifecycle/lifecycle.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/clock_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/runtime/unknown_resolver.hpp"
#include "broker_exec/store/store.hpp"

#include "conformance_kit.hpp"

namespace conf = broker_exec::conformance;

namespace {

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
constexpr const char* kAccessToken = "atACCESS0000AAAA1111BBBB2222";
constexpr const char* kToken = "ftFINAL9999GGGG0000HHHH1111";
constexpr const char* kSid = "fsSID2222IIII3333JJJJ4444";
constexpr const char* kServerId = "server3";

[[nodiscard]] KotakSessionBundle make_bundle() {
  KotakSessionBundle bundle;
  bundle.access_token = kAccessToken;
  bundle.token = kToken;
  bundle.sid = kSid;
  bundle.hs_server_id = kServerId;
  return bundle;
}

// ── jData body helpers (decode the `jData=<url-encoded json>` form body) ─────

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

// Kotak's quick endpoints carry the whole request object in ONE form field.
// A body that is not a `jData=` frame, or whose payload is not a JSON object,
// decodes to an empty object rather than throwing.
[[nodiscard]] json parse_jdata(const std::string& body) {
  constexpr std::string_view kPrefix = "jData=";
  const std::string_view view(body);
  if (view.size() < kPrefix.size() || view.substr(0, kPrefix.size()) != kPrefix) {
    return json::object();
  }
  const std::string decoded = url_decode(view.substr(kPrefix.size()));
  json parsed = json::parse(decoded, nullptr, /*allow_exceptions=*/false);
  return parsed.is_object() ? parsed : json::object();
}

[[nodiscard]] std::string jstr(const json& obj, const char* key) {
  const auto it = obj.find(key);
  return (it != obj.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

// Route on the endpoint constants themselves, so a path typo in the REST client
// or in `endpoints::` cannot be papered over by a hand-copied literal here.
[[nodiscard]] bool path_is(const HttpRequest& request, std::string_view endpoint) noexcept {
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
// Plus three Kotak-specific knobs the direct tests below use: `hard_reject` (the
// Kotak trap: HTTP 200 carrying `stat:"Not_Ok"`), a configurable fill model
// (partial fills, including a broker that says "complete" while reporting less
// than the full quantity), and a seeded COLLIDING MANUAL ORDER.
//
// DELIBERATE DEVIATION FROM RecordedKiteServer — READS ARE NOT THROTTLED, AND
// THAT DEVIATION IS LOAD-BEARING, NOT COSMETIC. Say so plainly: modelling the
// whole-surface throttle that RecordedKiteServer uses would make this suite FAIL,
// and the failure would be at the kit's infrastructure level rather than a real
// safety finding. The mechanism:
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
// an infrastructure failure, which is a change to the shared kit and therefore out
// of scope for this story. Tracked in docs/kotak-min-qty-smoke.md.
//
// FIXTURE PROVENANCE (AC-1): every envelope this server emits is the SAME shape
// as the committed recorded-response fixtures in
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
    std::string price;  // rupee-decimal TEXT, exactly as Kotak sends it
    std::string status;
  };

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

  // Emit the order TOTAL under a different JSON key. Kotak's field spellings are
  // an unverified tier-2 assumption, so "the total arrives under a name we did not
  // anticipate" is a first-class hazard, not a hypothetical: it is the difference
  // between "absent" and "zero", and getting it wrong turns a live working order
  // into a terminal Filled.
  void set_total_field(std::string key) { total_field_ = std::move(key); }

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

  [[nodiscard]] HttpResponse cancel(const HttpRequest& request) const {
    if (mutation_throttled()) {
      return fault_response(429, "900802", "Message throttled out");
    }
    const json params = parse_jdata(request.body);
    json fields = json::object();
    fields["result"] = jstr(params, "on");  // Kotak echoes the cancelled id here
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
      o["prc"] = rec.price;                      // rupee-decimal TEXT; no float in the fixture
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

  [[nodiscard]] HttpResponse positions() const { return ok_data_response(json::array()); }

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
  std::int64_t fill_qty_ = -1;            // < 0 -> fill the whole order
  std::string fill_status_ = "complete";  // the `ordSt` a recorded order reports
  std::string total_field_ = "qty";       // which key carries the order total

  // Stateful broker truth; mutable because HttpClient::send() is const.
  mutable std::vector<Record> book_;
  mutable std::vector<std::string> place_bodies_;
  mutable std::int64_t next_id_ = 1;
  mutable std::size_t mutation_count_ = 0;
  mutable std::size_t place_count_ = 0;
};

// BROKER-TRUTH TELEMETRY, COLLECTED AT SCENARIO TEARDOWN.
//
// The kit's duplicate metric counts broker rows whose `client_ref` equals the
// signal's — which is a CORRELATION measurement, not a duplicate measurement. For
// an adapter with a perfect tag echo those coincide; for Kotak they do NOT, and
// the gap is exactly the wrong way round: two duplicate ack-lost orders make the
// attribute key AMBIGUOUS, so the adapter (correctly) refuses to name either row,
// both come back ref-empty, and the metric reads ZERO duplicates for the very
// scenario that produced two live orders. A correlation failure would mask a
// duplicate-order catastrophe.
//
// So we measure the thing itself, from the recorded broker's own state: how many
// orders the BOOK holds, and how many places actually crossed the wire. The kit
// still runs unchanged (its three properties are still worth having) — these
// assertions sit alongside it and are the ones that would catch a duplicate.
//
// Collected in the destructor because the kit destroys each scenario's stack
// before the next one starts, so a raw server pointer would dangle by the time
// run_conformance() returns. Members are destroyed AFTER the destructor body, so
// `server` is still alive here.
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

// The Kotak BrokerFactory: per scenario, a fresh recorded server for the
// FaultConfig, a KotakRestClient over it + a synthetic session bundle, and a
// KotakBrokerAdapter — all owned by the returned BrokerPort. `tally` (optional)
// receives each scenario's broker-truth counters at teardown.
conf::BrokerFactory kotak_factory(ConformanceTally* tally = nullptr) {
  return [tally](broker_exec::ports::ClockPort& clock,
                 FaultConfig fault) -> std::unique_ptr<broker_exec::ports::BrokerPort> {
    return std::make_unique<OwningKotakAdapter>(clock, fault, tally);
  };
}

// The canonical intent used by the direct (non-kit) tests, with a KNOWN,
// non-empty client_ref in the "<strategy>-<sig8>-<uuid>" shape the dispatcher
// mints (Story 1.7). Recovery must hand this EXACT ref back.
constexpr const char* kClientRef = "alpha-1a2b3c4d-deadbeefcafebabe0123456789abcdef";

[[nodiscard]] broker_exec::domain::OrderIntent make_intent() {
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
}

}  // namespace

// ── AC-1: the kit, verbatim, across the whole fault matrix ───────────────────

TEST_CASE("conformance: the Kotak adapter passes the full fault matrix with zero duplicates",
          "[conformance][kotak]") {
  ConformanceTally tally;
  const conf::ConformanceReport report = conf::run_conformance(kotak_factory(&tally));

  // Surface every failure line so a regression names the exact scenario+property.
  for (const std::string& f : report.failures) {
    UNSCOPED_INFO("kotak conformance failure: " << f);
  }

  CHECK(report.scenarios_run > 0);
  CHECK(report.duplicate_orders == 0);
  CHECK(report.scenarios_passed == report.scenarios_run);
  CHECK(report.ok());

  // ── The duplicate check that actually measures duplicates ────────────────
  // Everything above is the kit's view, which counts rows BY CLIENT_REF. For an
  // adapter without a broker tag echo that metric can read zero simply because
  // correlation failed — see the ConformanceTally comment. These assertions read
  // the recorded broker's own state instead, so a real duplicate cannot hide
  // behind an ambiguous attribute key.
  REQUIRE(tally.book_sizes.size() == static_cast<std::size_t>(report.scenarios_run));
  REQUIRE(tally.place_counts.size() == static_cast<std::size_t>(report.scenarios_run));

  std::size_t total_places = 0;
  for (std::size_t i = 0; i < tally.book_sizes.size(); ++i) {
    UNSCOPED_INFO("scenario #" << i << ": wire places=" << tally.place_counts[i]
                               << " broker book=" << tally.book_sizes[i]);
    // ZERO DUPLICATES, measured from broker truth: at most ONE order exists at the
    // broker per scenario, whatever the caller managed (or failed) to correlate.
    CHECK(tally.book_sizes[i] <= 1);
    // NO BLIND RETRY, measured at the wire: at most one place crossed it.
    CHECK(tally.place_counts[i] <= 1);
    total_places += tally.place_counts[i];
  }
  // Every scenario placed EXACTLY once — no scenario silently skipped its place
  // (which would make the zero-duplicate result vacuous) and none placed twice.
  CHECK(total_places == static_cast<std::size_t>(report.scenarios_run));
}

TEST_CASE("conformance: every scenario in the matrix is exercised against Kotak",
          "[conformance][kotak]") {
  const conf::ConformanceReport report = conf::run_conformance(kotak_factory());
  CHECK(report.scenarios_run == 7);
}

// ── Explicit non-vacuity: recovery + the no-blind-retry place count ──────────
// These exercise the adapter directly (NOT via run_conformance) to prove two
// things the kit cannot prove for a real adapter by itself. Story 2.14's review
// closed exactly this hole for Kite; it is not reopened here.

TEST_CASE("[conformance][kotak][recovery] an ack-lost order is recovered with its client_ref",
          "[conformance][kotak][recovery]") {
  SECTION("ack-lost place is recovered by ATTRIBUTE CORROBORATION, not a tag echo") {
    // ack_lost_but_placed: the order IS live at the broker but the caller sees a
    // transport failure — the headline duplicate-risk the safety core must survive.
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{.ack_lost_but_placed = true});

    const broker_exec::domain::OrderIntent intent = make_intent();

    // place() returns an Error (ack lost). The dispatcher would mark this UNKNOWN
    // and reconcile — never blindly retry.
    auto placed = owner.adapter.place(intent);
    REQUIRE_FALSE(placed.has_value());

    // NO BLIND RETRY: exactly ONE wire place per place() call. The only legitimate
    // re-fire is the dispatcher's, after reserve().
    CHECK(owner.server->place_count() == 1);
    // ...and the order really is live at the broker despite the failed ack.
    CHECK(owner.server->book_size() == 1);

    // NON-VACUITY, PART 1: the client_ref never went out on the wire, so anything
    // that comes back cannot be a broker echo — it has to be corroboration.
    REQUIRE(owner.server->place_bodies().size() == 1);
    CHECK(owner.server->place_bodies().front().find(kClientRef) == std::string::npos);

    // NON-VACUITY, PART 2: fetch_orders() recovers the live order AND its exact
    // originating client_ref. An adapter returning an empty ref would pass the
    // kit's duplicate count vacuously; this closes that hole.
    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    const broker_exec::domain::Order& recovered = orders.value().front();
    CHECK_FALSE(recovered.intent.client_ref.empty());
    CHECK(recovered.intent.client_ref == kClientRef);
    CHECK(recovered.state == broker_exec::domain::OrderState::Filled);

    // ANCHOR-ON-CORROBORATION: the weak rung ran once and promoted itself to the
    // strong id rung, so the trade book now correlates by broker order id.
    auto trades = owner.adapter.fetch_trades();
    REQUIRE(trades.has_value());
    REQUIRE(trades.value().size() == 1);
    CHECK(trades.value().front().client_ref == kClientRef);
    CHECK(trades.value().front().broker_order_id == recovered.broker_order_id);
  }

  SECTION("clean place is recovered from the broker-order-id map (the strong rung)") {
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});

    const broker_exec::domain::OrderIntent intent = make_intent();

    auto placed = owner.adapter.place(intent);
    REQUIRE(placed.has_value());
    CHECK_FALSE(placed.value().broker_order_id.empty());
    CHECK(placed.value().client_ref == kClientRef);
    CHECK(owner.server->place_count() == 1);  // still exactly one wire place

    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    const broker_exec::domain::Order& recovered = orders.value().front();
    CHECK(recovered.intent.client_ref == kClientRef);
    CHECK(recovered.broker_order_id == placed.value().broker_order_id);
  }
}

TEST_CASE("[conformance][kotak][recovery] a colliding manual order fails CLOSED, never a guess",
          "[conformance][kotak][recovery]") {
  // An operator (or another process) already has an order at the broker with the
  // SAME symbol/side/quantity as ours, and OUR ack is lost. The attribute rung is
  // now ambiguous, and the only safe answer is to claim NOTHING: an order attached
  // to the WRONG signal is worse than an unresolved one, which merely stays UNKNOWN
  // under an operator alert.
  broker_exec::clock::TestClock clock;
  OwningKotakAdapter owner(clock, FaultConfig{.ack_lost_but_placed = true});
  owner.server->seed_manual_order("NIFTY24JUN24000CE", "S", 50, "123.50");

  auto placed = owner.adapter.place(make_intent());
  REQUIRE_FALSE(placed.has_value());
  CHECK(owner.server->place_count() == 1);

  auto orders = owner.adapter.fetch_orders();
  REQUIRE(orders.has_value());
  REQUIRE(orders.value().size() == 2);  // the manual order plus our ack-lost one

  int claimed = 0;
  for (const broker_exec::domain::Order& o : orders.value()) {
    if (o.intent.client_ref == kClientRef) {
      ++claimed;
    }
  }
  // ZERO, not one and emphatically not two: an ambiguous pairing resolves to
  // nothing at all. (Two would be the shape that registers as a duplicate against
  // a single signal — the invariant NFR-3 forbids.)
  CHECK(claimed == 0);

  // The order is still enumerable at the broker; it is simply UNATTRIBUTED, which
  // is what drives the UnknownResolver to its fail-closed operator alert.
  CHECK(owner.server->book_size() == 2);

  // And the correlation tuple is WITHHELD on both uncorrelated rows, so the
  // resolver's own first-match-wins attribute rung cannot overturn this refusal
  // one layer up (pinned end-to-end by the [stack] test below).
  for (const broker_exec::domain::Order& o : orders.value()) {
    CHECK(o.intent.quantity == broker_exec::domain::Quantity::of(0));
    CHECK(o.intent.price == broker_exec::domain::Price::from_paise(0));
  }
}

TEST_CASE("[conformance][kotak][recovery] a DEFINITIVELY rejected intent stops attracting matches",
          "[conformance][kotak][recovery]") {
  // THE BUG THIS PINS: registration happens before the wire call, so a place that
  // the broker DEFINITIVELY refused (HTTP-200 `Not_Ok`, nothing live, book empty)
  // used to leave its shape registered forever. An operator then placing an
  // identical lot by hand would have that order silently adopted as ours — we
  // would be "managing" a position we never opened, and the real one would go
  // unmanaged. A definitive verdict must retire the registration.
  broker_exec::clock::TestClock clock;
  OwningKotakAdapter owner(clock, FaultConfig{});
  owner.server->set_hard_reject(true);

  const broker_exec::domain::OrderIntent intent = make_intent();
  auto placed = owner.adapter.place(intent);
  REQUIRE_FALSE(placed.has_value());
  // A definitive verdict, NOT an ambiguous one: nothing can be live.
  CHECK(placed.error().action == broker_exec::errors::SuggestedAction::DoNotRetry);
  CHECK(owner.server->book_size() == 0);

  // Now the operator places an identical lot by hand.
  owner.server->set_hard_reject(false);
  owner.server->seed_manual_order("NIFTY24JUN24000CE", "S", 50, "123.50");

  auto orders = owner.adapter.fetch_orders();
  REQUIRE(orders.has_value());
  REQUIRE(orders.value().size() == 1);
  const broker_exec::domain::Order& manual = orders.value().front();

  // It is REPORTED (never hide a broker order) but NOT CLAIMED.
  CHECK(manual.intent.client_ref.empty());
  CHECK_FALSE(manual.broker_order_id.empty());
  // ...and without the correlation tuple, so the resolver cannot claim it either.
  CHECK(manual.intent.quantity == broker_exec::domain::Quantity::of(0));
}

TEST_CASE("[conformance][kotak][stack] the colliding-manual refusal holds at the STACK level",
          "[conformance][kotak][stack]") {
  // The adapter refusing to name a row is only half the story: `UnknownResolver`
  // runs its OWN attribute corroboration (rung 3) on (symbol, side, quantity,
  // price), first-match-wins and with no ambiguity check. This test runs the two
  // together and pins what the STACK does — because "the adapter returns an empty
  // ref" is not by itself a safety guarantee.
  namespace fs = std::filesystem;
  namespace rt = broker_exec::runtime;

  std::error_code ec;
  const fs::path datadir = fs::temp_directory_path() / "broker_exec_kotak_stack_collide";
  fs::remove_all(datadir, ec);
  fs::create_directories(datadir, ec);

  const broker_exec::domain::OrderIntent intent = make_intent();

  // The local UNKNOWN order the dispatcher would have recorded for an ack-lost
  // place: full intent, no broker_order_id (the ack never came back).
  const auto make_unknown_order = [&intent] {
    broker_exec::domain::Order order;
    order.intent = intent;
    order.state = broker_exec::domain::OrderState::Unknown;
    return order;
  };

  SECTION("with a colliding manual order the stack stays UNKNOWN and alerts") {
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{.ack_lost_but_placed = true});
    owner.server->seed_manual_order("NIFTY24JUN24000CE", "S", 50, "123.50");

    auto store_opened = broker_exec::store::Store::open((datadir / "collide.db").string());
    REQUIRE(store_opened.has_value());
    broker_exec::store::Store store = std::move(store_opened.value());
    broker_exec::lifecycle::LifecycleEngine fsm;
    conf::detail::CountingAlertSink alerts;
    rt::UnknownResolver resolver(owner, store, fsm, alerts, clock);

    REQUIRE_FALSE(owner.adapter.place(intent).has_value());
    const broker_exec::domain::Order unknown_order = make_unknown_order();
    REQUIRE(store.insert_order(unknown_order).has_value());

    auto resolution = resolver.resolve(unknown_order);
    REQUIRE(resolution.has_value());

    // FAIL-CLOSED, end to end: no rung matched, the order stays UNKNOWN, and the
    // operator is alerted. Without the adapter withholding the correlation tuple
    // the resolver would have adopted whichever colliding row it saw first.
    CHECK(resolution.value().kind == rt::MatchKind::NoMatch);
    CHECK_FALSE(resolution.value().resolved_ok);
    CHECK(resolution.value().new_state == broker_exec::domain::OrderState::Unknown);
    CHECK(alerts.count() > 0);
  }

  SECTION("without a collision the stack DOES resolve (the refusal is not blanket)") {
    // The control that keeps the section above from being vacuous: suppression
    // must cost us nothing when the pairing is unambiguous.
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{.ack_lost_but_placed = true});

    auto store_opened = broker_exec::store::Store::open((datadir / "clean.db").string());
    REQUIRE(store_opened.has_value());
    broker_exec::store::Store store = std::move(store_opened.value());
    broker_exec::lifecycle::LifecycleEngine fsm;
    conf::detail::CountingAlertSink alerts;
    rt::UnknownResolver resolver(owner, store, fsm, alerts, clock);

    REQUIRE_FALSE(owner.adapter.place(intent).has_value());
    const broker_exec::domain::Order unknown_order = make_unknown_order();
    REQUIRE(store.insert_order(unknown_order).has_value());

    auto resolution = resolver.resolve(unknown_order);
    REQUIRE(resolution.has_value());
    CHECK(resolution.value().resolved_ok);
    // HONEST-LABELLING CAVEAT (documented, not encoded): the resolver reports
    // CorrelationToken because the adapter stamped the recovered ref onto the row —
    // but NO broker echoed anything. The evidence was attribute corroboration. On
    // Kotak, read CORRELATION_TOKEN as "order id OR corroborated attributes" until
    // TagCarry is resolved live. There is no field on domain::Order to say
    // "weak match" without rippling through ports/store/lifecycle.
    CHECK(resolution.value().kind == rt::MatchKind::CorrelationToken);
  }

  fs::remove_all(datadir, ec);
}

TEST_CASE("[conformance][kotak][state] Kotak status mapping is fail-closed and quantity-driven",
          "[conformance][kotak][state]") {
  const broker_exec::domain::OrderIntent intent = make_intent();

  SECTION("a partial fill on a working order is PartiallyFilled") {
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->set_fill_model(20, "open");

    REQUIRE(owner.adapter.place(intent).has_value());
    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    CHECK(orders.value().front().state == broker_exec::domain::OrderState::PartiallyFilled);
    CHECK(orders.value().front().filled_qty == broker_exec::domain::Quantity::of(20));
  }

  SECTION("a broker that SAYS complete while reporting a short fill is still PartiallyFilled") {
    // The fillnorm rule: drive off the filled QUANTITY, never the event/status
    // label. Believing the label here would under-report the live remainder.
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->set_fill_model(20, "complete");

    REQUIRE(owner.adapter.place(intent).has_value());
    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    CHECK(orders.value().front().state == broker_exec::domain::OrderState::PartiallyFilled);
  }

  SECTION("'not cancelled' is a WORKING order, not a terminal cancel") {
    // Kotak's vocabulary contains "not cancelled" / "cancel pending": a cancel
    // request that did NOT take effect. A naive substring match on "cancel" would
    // report the engine flat while the order is live at the exchange.
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->set_fill_model(0, "Not Cancelled");

    REQUIRE(owner.adapter.place(intent).has_value());
    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    CHECK(orders.value().front().state == broker_exec::domain::OrderState::Acknowledged);
  }

  SECTION("an UNRECOGNISED status maps to Unknown (fail-closed), never a guess") {
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->set_fill_model(50, "SOME_FUTURE_KOTAK_STATE");

    REQUIRE(owner.adapter.place(intent).has_value());
    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    // Note it is Unknown DESPITE a full filled quantity: an unrecognised status is
    // not evidence we may reason from, so the engine is forced to reconcile.
    CHECK(orders.value().front().state == broker_exec::domain::OrderState::Unknown);
  }

  SECTION("a WORKING order whose total arrives under an unexpected key is NOT Filled") {
    // THE BUG THIS PINS: `qty` absent used to default to 0, so fillnorm computed
    // pending = max(0, 0 - 30) = 0 and read "filled>0 && pending==0" as FILLED —
    // a terminal, ABSORBING state in the lifecycle FSM. A live 30-of-50 working
    // order would have been permanently marked done because one field name
    // differed. Absent is not zero: with no total, a working status may never
    // produce a terminal state.
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->set_total_field("quantity");  // a spelling the adapter does not know
    owner.server->set_fill_model(30, "open");

    REQUIRE(owner.adapter.place(intent).has_value());
    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    const broker_exec::domain::Order& row = orders.value().front();
    CHECK(row.state != broker_exec::domain::OrderState::Filled);
    CHECK(row.state == broker_exec::domain::OrderState::PartiallyFilled);
    CHECK(row.filled_qty == broker_exec::domain::Quantity::of(30));
  }

  SECTION("a WORKING order with an unknown total and no fill is Acknowledged, not Filled") {
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->set_total_field("quantity");
    owner.server->set_fill_model(0, "open");

    REQUIRE(owner.adapter.place(intent).has_value());
    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    CHECK(orders.value().front().state == broker_exec::domain::OrderState::Acknowledged);
  }

  SECTION("an impossible over-fill is clamped to the ordered quantity") {
    // A broker reporting fldQty=500 against qty=50 is reporting garbage, and the
    // dangerous direction is obvious: sizing an exit off 500 sells ten times the
    // position. Cap the fill at what was actually ordered.
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->set_fill_model(500, "complete");

    REQUIRE(owner.adapter.place(intent).has_value());
    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    CHECK(orders.value().front().filled_qty == broker_exec::domain::Quantity::of(50));
    CHECK(orders.value().front().state == broker_exec::domain::OrderState::Filled);
  }

  SECTION("a row carrying UNPARSEABLE money fails closed to Unknown") {
    // Money is never read best-effort: a truncating parse would have read
    // "12.3.4.5" as 1230 paise and reported a confident wrong price.
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->seed_manual_order("INFY-EQ", "B", 1, "12.3.4.5");

    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    // Despite a perfectly recognisable "open" status, the row we cannot read is
    // Unknown — the engine must reconcile rather than trust our parsing.
    CHECK(orders.value().front().state == broker_exec::domain::OrderState::Unknown);
    CHECK(orders.value().front().intent.price == broker_exec::domain::Price::from_paise(0));
  }

  SECTION("a 20-digit money field is a parse failure, not an overflow") {
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->seed_manual_order("INFY-EQ", "B", 1, "99999999999999999999.99");

    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    CHECK(orders.value().front().state == broker_exec::domain::OrderState::Unknown);
  }

  SECTION("observed casings and separators all map to the same state") {
    for (const char* status : {"COMPLETE", "Complete", "complete"}) {
      broker_exec::clock::TestClock clock;
      OwningKotakAdapter owner(clock, FaultConfig{});
      owner.server->set_fill_model(-1, status);

      REQUIRE(owner.adapter.place(intent).has_value());
      auto orders = owner.adapter.fetch_orders();
      REQUIRE(orders.has_value());
      REQUIRE(orders.value().size() == 1);
      CHECK(orders.value().front().state == broker_exec::domain::OrderState::Filled);
    }
    for (const char* status : {"TRIGGER PENDING", "trigger_pending", "Trigger-Pending"}) {
      broker_exec::clock::TestClock clock;
      OwningKotakAdapter owner(clock, FaultConfig{});
      owner.server->set_fill_model(0, status);

      REQUIRE(owner.adapter.place(intent).has_value());
      auto orders = owner.adapter.fetch_orders();
      REQUIRE(orders.has_value());
      REQUIRE(orders.value().size() == 1);
      CHECK(orders.value().front().state == broker_exec::domain::OrderState::Acknowledged);
    }
  }
}

TEST_CASE("[conformance][kotak][reject] an HTTP-200 Not_Ok is a rejection, and records nothing",
          "[conformance][kotak][reject]") {
  // The Kotak trap: a status-code-only client reads this as success and believes
  // it has a live order. It must surface as a typed failure with an EMPTY book.
  broker_exec::clock::TestClock clock;
  OwningKotakAdapter owner(clock, FaultConfig{});
  owner.server->set_hard_reject(true);

  auto placed = owner.adapter.place(make_intent());
  REQUIRE_FALSE(placed.has_value());
  CHECK(placed.error().category == broker_exec::errors::ErrorCategory::InsufficientFunds);
  CHECK(owner.server->place_count() == 1);  // still no retry on a hard reject
  CHECK(owner.server->book_size() == 0);    // nothing reached the book

  auto orders = owner.adapter.fetch_orders();
  REQUIRE(orders.has_value());
  CHECK(orders.value().empty());
}

TEST_CASE("[conformance][kotak][money] prices round-trip as integer paise, never a float",
          "[conformance][kotak][money]") {
  broker_exec::clock::TestClock clock;
  OwningKotakAdapter owner(clock, FaultConfig{});

  broker_exec::domain::OrderIntent intent = make_intent();
  intent.price = broker_exec::domain::Price::from_rupees(1450, 5);  // 1450.05 -> 145005 paise

  REQUIRE(owner.adapter.place(intent).has_value());
  // The wire body carries the decimal TEXT form, produced without any float.
  REQUIRE(owner.server->place_bodies().size() == 1);
  CHECK(owner.server->place_bodies().front().find("1450.05") != std::string::npos);

  auto orders = owner.adapter.fetch_orders();
  REQUIRE(orders.has_value());
  REQUIRE(orders.value().size() == 1);
  CHECK(orders.value().front().avg_price == broker_exec::domain::Price::from_paise(145005));
  CHECK(orders.value().front().intent.price == broker_exec::domain::Price::from_paise(145005));

  auto funds = owner.adapter.fetch_funds();
  REQUIRE(funds.has_value());
  CHECK(funds.value().available_margin == broker_exec::domain::Money::from_paise(10000000));
  CHECK(funds.value().used_margin == broker_exec::domain::Money::from_paise(0));
}

TEST_CASE("[conformance][kotak][money] the decimal->paise parser is fail-closed and overflow-safe",
          "[conformance][kotak][money]") {
  using broker_exec::domain::parse_decimal_paise;
  using broker_exec::domain::parse_int64;
  using broker_exec::domain::paise_to_decimal;

  // Exact values.
  CHECK(parse_decimal_paise("1450.05") == std::optional<std::int64_t>{145005});
  CHECK(parse_decimal_paise("1450.5") == std::optional<std::int64_t>{145050});  // zero-padded
  CHECK(parse_decimal_paise("1450") == std::optional<std::int64_t>{145000});
  CHECK(parse_decimal_paise("-7.25") == std::optional<std::int64_t>{-725});
  CHECK(parse_decimal_paise("  12.34  ") == std::optional<std::int64_t>{1234});
  CHECK(parse_decimal_paise("1.239") == std::optional<std::int64_t>{123});  // sub-paise truncated

  // FAIL-CLOSED: anything we cannot read exactly is nullopt, never a number. A
  // truncating parser read each of these as a confident wrong value.
  CHECK_FALSE(parse_decimal_paise("12.3.4.5").has_value());
  CHECK_FALSE(parse_decimal_paise("abc").has_value());
  CHECK_FALSE(parse_decimal_paise("12abc").has_value());
  CHECK_FALSE(parse_decimal_paise("1.2x9").has_value());
  CHECK_FALSE(parse_decimal_paise("").has_value());
  CHECK_FALSE(parse_decimal_paise(".").has_value());
  CHECK_FALSE(parse_decimal_paise("-").has_value());

  // OVERFLOW is a parse failure, not undefined behaviour. A 20-digit field is a
  // thing a broker can send, and wrapping it silently produces a plausible-looking
  // negative price.
  CHECK_FALSE(parse_decimal_paise("99999999999999999999.99").has_value());
  CHECK_FALSE(parse_decimal_paise("92233720368547758.08").has_value());  // just past the scaling
  CHECK_FALSE(parse_int64("99999999999999999999").has_value());
  CHECK(parse_int64("50") == std::optional<std::int64_t>{50});
  CHECK_FALSE(parse_int64("50 lots").has_value());  // trailing garbage FAILS, never truncates

  // Round-trip, including the INT64_MIN edge the naive negate would overflow on.
  CHECK(paise_to_decimal(145005) == "1450.05");
  CHECK(paise_to_decimal(-725) == "-7.25");
  CHECK(paise_to_decimal(5) == "0.05");
  CHECK_FALSE(paise_to_decimal(std::numeric_limits<std::int64_t>::min()).empty());
}

TEST_CASE("[conformance][kotak][squareoff] square_off REFUSES rather than fail open",
          "[conformance][kotak][squareoff]") {
  // It used to issue a cancel and return ok. Against a FILLED position a cancel is
  // a no-op, so the caller was told "you are flat" while the position was still
  // on — and would stop managing it. A typed refusal is the only honest answer
  // until a real position-flattening exit exists.
  broker_exec::clock::TestClock clock;
  OwningKotakAdapter owner(clock, FaultConfig{});

  auto placed = owner.adapter.place(make_intent());
  REQUIRE(placed.has_value());

  auto squared = owner.adapter.square_off(placed.value().broker_order_id);
  REQUIRE_FALSE(squared.has_value());
  CHECK(squared.error().category == broker_exec::errors::ErrorCategory::NotSupported);
  CHECK(squared.error().action == broker_exec::errors::SuggestedAction::DoNotRetry);
  // And it reached no endpoint: refusing is a local decision, not a broker call.
  CHECK(owner.server->book_size() == 1);
}

// ── AC-2: tier-1 green must NOT promote a single capability ──────────────────

TEST_CASE("[conformance][kotak][capabilities] passing the kit does NOT flip any capability",
          "[conformance][kotak][capabilities]") {
  // THE GUARD THIS TEST EXISTS FOR: it is very tempting, once the conformance kit
  // is green, to "finish the job" by marking PlaceOrder Supported. That would be a
  // lie with real money behind it — the kit ran against OUR OWN recorded endpoint,
  // not against Kotak. Only the operator-run live smoke
  // (docs/kotak-min-qty-smoke.md) may promote an entry, so this test fails loudly
  // if anyone promotes one without it.
  namespace caps = broker_exec::capabilities;
  const caps::CapabilitySet set = broker_exec::adapters::kotak::kotak_capabilities();

  for (std::size_t i = 0; i < caps::kCapabilityCount; ++i) {
    const auto capability = static_cast<caps::Capability>(i);
    UNSCOPED_INFO("capability: " << caps::to_string(capability));
    CHECK(set.support_of(capability) == caps::Support::Unknown);
  }

  // And Unknown must be REJECTED at the gate — early, not mid-trade (AC-2). The
  // capability deltas the story calls out are checked by name.
  CHECK_FALSE(set.supports(caps::Capability::PlaceOrder));
  CHECK_FALSE(set.require(caps::Capability::PlaceOrder).has_value());
  CHECK_FALSE(set.require(caps::Capability::BasketMargin).has_value());
  CHECK_FALSE(set.require(caps::Capability::OrderUpdateWebsocket).has_value());
  CHECK_FALSE(set.require(caps::Capability::HeadlessSessionRefresh).has_value());
  CHECK_FALSE(set.require(caps::Capability::TagCarry).has_value());
}
