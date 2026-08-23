// Kotak Neo BrokerPort adapter tests — the ABSENT-IS-NOT-ZERO contract on the
// FILL fields of an order-book row (IMP-27).
//
// The adapter's broad behaviour is certified by the broker-agnostic conformance
// kit (tests/conformance/kotak_conformance_test.cpp) against a stateful recorded
// server. That server always emits `fldQty` and `avgPrc`, which is exactly why it
// cannot see this class of defect: the hazard here is a row that OMITS a fill
// field, so it is pinned with hand-written rows at the adapter's own boundary.
//
// Every body below is a COMMITTED RECORDED-RESPONSE FIXTURE shape: no network, no
// live credentials. The session bundle is synthetic and token-shaped.

#include "broker_exec/adapters/kotak/kotak_broker_adapter.hpp"

#include <catch2/catch_test_macros.hpp>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "broker_exec/adapters/kotak/kotak_rest_client.hpp"
#include "broker_exec/adapters/kotak/kotak_session.hpp"
#include "broker_exec/adapters/kotak/kotak_transport.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/result.hpp"

using broker_exec::Result;
using broker_exec::adapters::kotak::HttpClient;
using broker_exec::adapters::kotak::HttpRequest;
using broker_exec::adapters::kotak::HttpResponse;
using broker_exec::adapters::kotak::KotakBrokerAdapter;
using broker_exec::adapters::kotak::KotakRestClient;
using broker_exec::adapters::kotak::KotakSessionBundle;
using broker_exec::domain::OrderState;
using broker_exec::domain::Price;
using broker_exec::domain::Quantity;
using broker_exec::errors::SuggestedAction;

namespace {

namespace endpoints = broker_exec::adapters::kotak::endpoints;

constexpr const char* kOrderId = "220101000000001";

// ── Order-book fixtures ──────────────────────────────────────────────────────
//
// A WORKING order for 50 whose row omits the filled quantity under every spelling
// this adapter knows (`fldQty` / `flQty` / `fillQty` / `filledQty`). Nothing here
// is malformed — no field is present-and-garbage — so the row used to be read as
// a confident "Acknowledged, filled 0".
constexpr const char* kBookWorkingNoFillQty =
    R"({"stat":"Ok","stCode":200,"data":[{"nOrdNo":"220101000000001","trdSym":"INFY-EQ",)"
    R"("trnsTp":"B","qty":"50","ordSt":"open","prcTp":"L","prc":"1450.25","avgPrc":"0.00"}]})";

// A row that CLAIMS a full fill but carries no readable average price. Money is
// never read best-effort here, so the Rs 0.00 this used to publish as the cost
// basis is a fabrication, and it used to travel on a TERMINAL (absorbing) Filled.
constexpr const char* kBookFilledNoAvgPrice =
    R"({"stat":"Ok","stCode":200,"data":[{"nOrdNo":"220101000000001","trdSym":"INFY-EQ",)"
    R"("trnsTp":"B","qty":"50","fldQty":"50","ordSt":"complete","prcTp":"L","prc":"1450.25"}]})";

// The control: the same order, fully reported. Nothing about this row changed.
constexpr const char* kBookFilledComplete =
    R"({"stat":"Ok","stCode":200,"data":[{"nOrdNo":"220101000000001","trdSym":"INFY-EQ",)"
    R"("trnsTp":"B","qty":"50","fldQty":"50","ordSt":"complete","prcTp":"L","prc":"1450.25",)"
    R"("avgPrc":"1450.25"}]})";

constexpr const char* kCancelOk = R"({"stat":"Ok","result":"220101000000001","stCode":200})";

// ── Test doubles (the same scripted seam kotak_rest_client_test.cpp drives) ───

class ScriptedHttpClient final : public HttpClient {
 public:
  void script(HttpRequest::Method method, std::string_view path, long status, std::string body) {
    HttpResponse response;
    response.status_code = status;
    response.body = std::move(body);
    script_[key(method, path)].push_back(std::move(response));
  }

  [[nodiscard]] Result<HttpResponse> send(const HttpRequest& request) const override {
    ++sent_;
    const auto it = script_.find(key(request.method, request.path));
    if (it == script_.end() || it->second.empty()) {
      return broker_exec::fail(broker_exec::errors::make_error(
          broker_exec::errors::ErrorCategory::Unknown, "no scripted response for request", "TEST"));
    }
    HttpResponse response = it->second.front();
    if (it->second.size() > 1) {
      it->second.pop_front();  // keep the last scripted answer repeatable
    }
    return response;
  }

  [[nodiscard]] int sent() const noexcept { return sent_; }

 private:
  static std::string key(HttpRequest::Method method, std::string_view path) {
    return std::to_string(static_cast<int>(method)) + " " + std::string(path);
  }

  mutable std::map<std::string, std::deque<HttpResponse>> script_;
  mutable int sent_ = 0;
};

[[nodiscard]] broker_exec::adapters::kotak::BundleProvider synthetic_bundle() {
  KotakSessionBundle bundle;
  bundle.access_token = "atACCESS0000AAAA1111BBBB2222";
  bundle.token = "ftFINAL9999GGGG0000HHHH1111";
  bundle.sid = "fsSID2222IIII3333JJJJ4444";
  bundle.hs_server_id = "server3";
  return [bundle]() -> Result<KotakSessionBundle> { return bundle; };
}

}  // namespace

TEST_CASE("a row whose FILLED QUANTITY we could not read is published Unknown",
          "[kotak][adapter][orders]") {
  // ABSENT IS NOT ZERO, and the fill is where it bites hardest: Acknowledged vs
  // PartiallyFilled vs Filled differ ONLY by the filled quantity, so a fill we
  // could not read is a lifecycle position we are not entitled to assert. This row
  // used to come back Acknowledged — a confident, wrong claim that a genuinely
  // 30-of-50 order had done nothing — because `first_number(...).value_or(0)` spent
  // "the field was absent" as the number 0 one line after the reader had carefully
  // kept the two apart.
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Get, endpoints::kOrderBook, 200, kBookWorkingNoFillQty);

  KotakRestClient rest(http, synthetic_bundle());
  KotakBrokerAdapter adapter(rest);

  const auto orders = adapter.fetch_orders();
  REQUIRE(orders.has_value());
  // ORDERS MUST STAY ENUMERABLE: the row is still reported, it just carries no
  // state claim. Dropping it would make the order VANISH from the snapshot, which
  // reconcile reads as a manual intervention.
  REQUIRE(orders.value().size() == 1);
  CHECK(orders.value()[0].broker_order_id == kOrderId);
  CHECK(orders.value()[0].state == OrderState::Unknown);
}

TEST_CASE("a row that claims a fill but no readable AVERAGE PRICE is published Unknown",
          "[kotak][adapter][orders]") {
  // Money is never read best-effort in this library. An `avgPrc` absent under every
  // spelling used to publish Rs 0.00 as the cost basis the ledger, the P&L and the
  // operator then believe — and it travelled on a TERMINAL Filled, which is
  // absorbing in the lifecycle FSM, so that zero could never be corrected by a
  // later snapshot. Unknown is non-terminal and therefore repairable.
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Get, endpoints::kOrderBook, 200, kBookFilledNoAvgPrice);

  KotakRestClient rest(http, synthetic_bundle());
  KotakBrokerAdapter adapter(rest);

  const auto orders = adapter.fetch_orders();
  REQUIRE(orders.has_value());
  REQUIRE(orders.value().size() == 1);
  CHECK(orders.value()[0].state == OrderState::Unknown);
  // The fill we DID read is still published — withholding is per FIELD, not a
  // blanket refusal to report anything about the row.
  CHECK(orders.value()[0].filled_qty == Quantity::of(50));

  // THE CONTROL, in the same case so this cannot pass by over-reaching: the same
  // order fully reported still maps to a confident terminal Filled at its real
  // average price.
  ScriptedHttpClient complete_http;
  complete_http.script(HttpRequest::Method::Get, endpoints::kOrderBook, 200, kBookFilledComplete);
  KotakRestClient complete_rest(complete_http, synthetic_bundle());
  KotakBrokerAdapter complete_adapter(complete_rest);

  const auto complete = complete_adapter.fetch_orders();
  REQUIRE(complete.has_value());
  REQUIRE(complete.value().size() == 1);
  CHECK(complete.value()[0].state == OrderState::Filled);
  CHECK(complete.value()[0].avg_price == Price::from_rupees(1450, 25));
}

TEST_CASE("square_off REFUSES an order whose filled quantity it could not read",
          "[kotak][adapter][squareoff]") {
  // THE FAIL-OPEN THIS CLOSES: read as 0, the flatten cancelled the working
  // remainder, computed exit_qty 0, took the "zero filled -> cancel-only IS a
  // complete square-off" branch and returned ok() — reporting success while a real
  // position stayed fully on. The cancel is scripted to SUCCEED precisely so that
  // old path would reach that ok(); the refusal below is the behaviour change, not
  // a transport failure.
  ScriptedHttpClient http;
  http.script(HttpRequest::Method::Get, endpoints::kOrderBook, 200, kBookWorkingNoFillQty);
  http.script(HttpRequest::Method::Post, endpoints::kCancelOrder, 200, kCancelOk);

  KotakRestClient rest(http, synthetic_bundle());
  KotakBrokerAdapter adapter(rest);

  const auto result = adapter.square_off(kOrderId);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().broker_code == "KOTAK-SQUAREOFF-NOFILLQTY");
  CHECK(result.error().action == SuggestedAction::ReconcileFirst);
  // AND NOTHING WAS SENT AFTER THE READ. The refusal happens before the cancel, so
  // a flatten we cannot size never touches the working order at all.
  CHECK(http.sent() == 1);
}
