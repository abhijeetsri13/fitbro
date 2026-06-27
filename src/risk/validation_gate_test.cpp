#include "broker_exec/risk/validation_gate.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <system_error>

#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/refdata/trading_calendar.hpp"
#include "broker_exec/result.hpp"

using broker_exec::Result;
using broker_exec::clock::TestClock;
using broker_exec::domain::Instrument;
using broker_exec::domain::OrderIntent;
using broker_exec::domain::OrderType;
using broker_exec::domain::Price;
using broker_exec::domain::Product;
using broker_exec::domain::Quantity;
using broker_exec::domain::Side;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::make_error;
using broker_exec::ports::Ok;
using broker_exec::refdata::TradingCalendar;
using broker_exec::risk::GateContext;
using broker_exec::risk::GateOutcome;
using broker_exec::risk::ValidationGate;

namespace {

// A unique temp directory, cleaned up on destruction (RAII).
struct TempDir {
  std::filesystem::path path;
  TempDir() {
    std::random_device rd;
    path = std::filesystem::temp_directory_path() /
           ("brexec_risk_" + std::to_string(rd()) + "_" + std::to_string(rd()));
    std::filesystem::create_directories(path);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

// Build a UTC wall instant for a UTC calendar date + time-of-day (deterministic).
// IST (the exchange-local zone the calendar uses) = this UTC + 5:30.
[[nodiscard]] std::chrono::system_clock::time_point utc_at(int year, unsigned month, unsigned day,
                                                           int hour, int minute) {
  const std::chrono::sys_days d{std::chrono::year{year} / std::chrono::month{month} /
                                std::chrono::day{day}};
  return std::chrono::system_clock::time_point(d) + std::chrono::hours(hour) +
         std::chrono::minutes(minute);
}

[[nodiscard]] TestClock clock_utc(int year, unsigned month, unsigned day, int hour, int minute) {
  return TestClock(std::chrono::steady_clock::time_point{}, utc_at(year, month, day, hour, minute));
}

// Trading calendar: windows open 09:15 / entry_cutoff 15:00 / square_off 15:20 /
// close 15:30 IST; no holidays. 2026-06-29 is a Monday (a trading day).
constexpr const char* kCal =
    R"({"holidays":[],"special_sessions":[],)"
    R"("windows":{"open":"09:15","entry_cutoff":"15:00","square_off":"15:20","close":"15:30"}})";

[[nodiscard]] std::function<Result<std::string>()> static_fetcher(std::string doc) {
  return [doc = std::move(doc)]() -> Result<std::string> { return doc; };
}

// The known instrument under test: NFO option, lot 50, tick 5 paise (0.05),
// freeze 1800.
[[nodiscard]] Instrument make_instrument() {
  return Instrument{.symbol = "NIFTY26JUL24000CE",
                    .token = 123456,
                    .exchange = "NFO",
                    .lot_size = Quantity::of(50),
                    .tick_size = Price::from_paise(5),
                    .freeze_qty = Quantity::of(1800),
                    .expiry = "2026-07-30"};
}

// A clean limit entry: lot-aligned qty 50, tick-aligned price 100.00.
[[nodiscard]] OrderIntent make_entry_intent() {
  return OrderIntent{.client_ref = "alpha-1a2b3c4d-uuid",
                     .symbol = "NIFTY26JUL24000CE",
                     .side = Side::Buy,
                     .quantity = Quantity::of(50),
                     .price = Price::from_rupees(100),
                     .order_type = OrderType::Limit,
                     .product = Product::Intraday,
                     .strategy = "alpha"};
}

// A context with the canonical allow-lists and all injected predicates passing
// (empty => pass) and no calendar (the time-window check is skipped). Each test
// then flips exactly one input to drive one check. The referenced `intent` /
// `instrument` must outlive the returned context.
[[nodiscard]] GateContext base_entry(const OrderIntent& intent, const Instrument& instrument) {
  GateContext ctx{.intent = intent, .instrument = instrument};
  ctx.allowed_exchanges = {"NFO"};
  ctx.allowed_products = {Product::Intraday};
  return ctx;
}

[[nodiscard]] std::function<Result<Ok>()> failing(ErrorCategory category, std::string message) {
  return [category, message = std::move(message)]() -> Result<Ok> {
    return broker_exec::fail(make_error(category, message));
  };
}

[[nodiscard]] bool names(const broker_exec::errors::Error& err, const std::string& check) {
  return err.message.find("gate: " + check + " check failed") != std::string::npos;
}

}  // namespace

TEST_CASE("a clean entry with every predicate passing -> Allow", "[risk][gate][AC1]") {
  TempDir dir;
  TestClock clk = clock_utc(2026, 6, 29, 3, 50);  // IST 09:20 Monday — in entry window
  TradingCalendar cal(static_fetcher(kCal), clk, dir.path, "kite");
  REQUIRE(cal.refresh().has_value());

  const OrderIntent intent = make_entry_intent();
  const Instrument instrument = make_instrument();
  GateContext ctx = base_entry(intent, instrument);
  ctx.calendar = &cal;  // and the window passes

  const ValidationGate gate;
  const Result<GateOutcome> r = gate.validate(ctx);
  REQUIRE(r.has_value());
  CHECK(r.value() == GateOutcome::Allow);
}

TEST_CASE("each check fails in isolation and the Error NAMES that check", "[risk][gate][AC1]") {
  const Instrument instrument = make_instrument();
  const ValidationGate gate;

  SECTION("lot: qty 75 is not a multiple of 50 -> Validation, names lot") {
    OrderIntent intent = make_entry_intent();
    intent.quantity = Quantity::of(75);
    GateContext ctx = base_entry(intent, instrument);
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().category == ErrorCategory::Validation);
    CHECK(names(r.error(), "lot"));
    CHECK(r.error().message.find("75") != std::string::npos);
  }

  SECTION("tick: price 100.03 is not tick-aligned -> Validation, names tick") {
    OrderIntent intent = make_entry_intent();
    intent.price = Price::from_rupees(100, 3);  // 10003 paise, not a multiple of 5
    GateContext ctx = base_entry(intent, instrument);
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().category == ErrorCategory::Validation);
    CHECK(names(r.error(), "tick"));
  }

  SECTION("exchange: instrument exchange not in the allow-list -> names exchange") {
    Instrument other = instrument;
    other.exchange = "NSE";  // allow-list is {"NFO"}
    const OrderIntent intent = make_entry_intent();
    GateContext ctx = base_entry(intent, other);
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().category == ErrorCategory::Validation);
    CHECK(names(r.error(), "exchange"));
  }

  SECTION("product: product not in the allow-list -> names product") {
    OrderIntent intent = make_entry_intent();
    intent.product = Product::Delivery;  // allow-list is {Intraday}
    GateContext ctx = base_entry(intent, instrument);
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().category == ErrorCategory::Validation);
    CHECK(names(r.error(), "product"));
  }

  SECTION("duplicate: is_duplicate true -> DuplicateOrder, names duplicate") {
    const OrderIntent intent = make_entry_intent();
    GateContext ctx = base_entry(intent, instrument);
    ctx.is_duplicate = []() { return true; };
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().category == ErrorCategory::DuplicateOrder);
    CHECK(names(r.error(), "duplicate"));
  }

  SECTION("kill-switch: active entry-block -> names kill-switch") {
    const OrderIntent intent = make_entry_intent();
    GateContext ctx = base_entry(intent, instrument);
    ctx.kill_entry_block = true;
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE_FALSE(r.has_value());
    CHECK(names(r.error(), "kill-switch"));
  }

  SECTION("UNKNOWN-pause: active -> names UNKNOWN-pause") {
    const OrderIntent intent = make_entry_intent();
    GateContext ctx = base_entry(intent, instrument);
    ctx.unknown_pause_active = true;
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE_FALSE(r.has_value());
    CHECK(names(r.error(), "UNKNOWN-pause"));
  }

  SECTION("funds: stale funds view -> DataStale (fail-closed), names funds") {
    const OrderIntent intent = make_entry_intent();
    GateContext ctx = base_entry(intent, instrument);
    ctx.funds_check = failing(ErrorCategory::DataStale, "funds view is stale");
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().category == ErrorCategory::DataStale);  // category preserved
    CHECK(names(r.error(), "funds"));
  }

  SECTION("risk: risk engine rejects -> RiskRejected, names risk") {
    const OrderIntent intent = make_entry_intent();
    GateContext ctx = base_entry(intent, instrument);
    ctx.risk_check = failing(ErrorCategory::RiskRejected, "per-strategy exposure exceeded");
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().category == ErrorCategory::RiskRejected);
    CHECK(names(r.error(), "risk"));
  }

  SECTION("hedge: hedge check rejects -> names hedge") {
    const OrderIntent intent = make_entry_intent();
    GateContext ctx = base_entry(intent, instrument);
    ctx.hedge_check = failing(ErrorCategory::RiskRejected, "naked short leg");
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE_FALSE(r.has_value());
    CHECK(names(r.error(), "hedge"));
  }
}

TEST_CASE("out-of-window entry -> MarketClosed, names time-window", "[risk][gate][AC1]") {
  TempDir dir;
  TestClock clk = clock_utc(2026, 6, 29, 9, 55);  // IST 15:25 — past the 15:00 cutoff
  TradingCalendar cal(static_fetcher(kCal), clk, dir.path, "kite");
  REQUIRE(cal.refresh().has_value());

  const OrderIntent intent = make_entry_intent();
  const Instrument instrument = make_instrument();
  GateContext ctx = base_entry(intent, instrument);
  ctx.calendar = &cal;

  const ValidationGate gate;
  const Result<GateOutcome> r = gate.validate(ctx);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().category == ErrorCategory::MarketClosed);  // propagated category
  CHECK(names(r.error(), "time-window"));
}

TEST_CASE("a market order skips the tick check", "[risk][gate][AC1]") {
  const Instrument instrument = make_instrument();
  OrderIntent intent = make_entry_intent();
  intent.order_type = OrderType::Market;
  intent.price = Price::from_rupees(100, 3);  // not tick-aligned, but ignored for Market
  GateContext ctx = base_entry(intent, instrument);

  const ValidationGate gate;
  const Result<GateOutcome> r = gate.validate(ctx);
  REQUIRE(r.has_value());
  CHECK(r.value() == GateOutcome::Allow);
}

TEST_CASE("over-freeze quantity: slice-mode slices, reject-mode rejects", "[risk][gate][AC1]") {
  const Instrument instrument = make_instrument();  // freeze_qty 1800
  const ValidationGate gate;

  SECTION("qty 2000 in slice-mode (default) -> AllowWithSlicing, not an error") {
    OrderIntent intent = make_entry_intent();
    intent.quantity = Quantity::of(2000);  // > 1800, lot-aligned (2000 % 50 == 0)
    GateContext ctx = base_entry(intent, instrument);
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE(r.has_value());
    CHECK(r.value() == GateOutcome::AllowWithSlicing);
  }

  SECTION("qty 2000 in reject-mode -> Validation Error, names freeze") {
    OrderIntent intent = make_entry_intent();
    intent.quantity = Quantity::of(2000);
    GateContext ctx = base_entry(intent, instrument);
    ctx.slice_mode = false;
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().category == ErrorCategory::Validation);
    CHECK(names(r.error(), "freeze"));
  }

  SECTION("over-freeze keeps running later checks: a failing risk check still wins") {
    OrderIntent intent = make_entry_intent();
    intent.quantity = Quantity::of(2000);
    GateContext ctx = base_entry(intent, instrument);  // slice_mode default true
    ctx.risk_check = failing(ErrorCategory::RiskRejected, "exposure");
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE_FALSE(r.has_value());  // slicing decision does NOT short-circuit later checks
    CHECK(names(r.error(), "risk"));
  }
}

TEST_CASE("AC-3: kill-switch + UNKNOWN-pause block an entry but a risk-reducing exit passes",
          "[risk][gate][AC3]") {
  const Instrument instrument = make_instrument();
  const ValidationGate gate;

  SECTION("an ENTRY under both blocks is rejected (kill-switch named first)") {
    const OrderIntent intent = make_entry_intent();
    GateContext ctx = base_entry(intent, instrument);
    ctx.kill_entry_block = true;
    ctx.unknown_pause_active = true;
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE_FALSE(r.has_value());
    CHECK(names(r.error(), "kill-switch"));
  }

  SECTION("a risk-reducing EXIT under both blocks passes them") {
    const OrderIntent intent = make_entry_intent();
    GateContext ctx = base_entry(intent, instrument);
    ctx.kill_entry_block = true;
    ctx.unknown_pause_active = true;
    ctx.is_risk_reducing = true;  // exempt from kill-switch / UNKNOWN-pause
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE(r.has_value());
    CHECK(r.value() == GateOutcome::Allow);
  }

  SECTION("lot/tick STILL apply to a risk-reducing exit") {
    OrderIntent intent = make_entry_intent();
    intent.quantity = Quantity::of(75);  // bad lot
    GateContext ctx = base_entry(intent, instrument);
    ctx.is_risk_reducing = true;
    ctx.kill_entry_block = true;
    ctx.unknown_pause_active = true;
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE_FALSE(r.has_value());
    CHECK(names(r.error(), "lot"));  // the exemption does NOT skip lot
  }
}

TEST_CASE("ordering: when several checks would fail, the FIRST in the pipeline is named",
          "[risk][gate][AC1]") {
  const Instrument instrument = make_instrument();
  OrderIntent intent = make_entry_intent();
  intent.quantity = Quantity::of(75);  // bad lot (a later check)
  GateContext ctx = base_entry(intent, instrument);
  ctx.kill_entry_block = true;  // kill-switch is check #1

  const ValidationGate gate;
  const Result<GateOutcome> r = gate.validate(ctx);
  REQUIRE_FALSE(r.has_value());
  CHECK(names(r.error(), "kill-switch"));  // kill-switch precedes lot
  CHECK_FALSE(names(r.error(), "lot"));
}

TEST_CASE("tick alignment is enforced for every price-bearing order type", "[risk][gate][AC1]") {
  const Instrument instrument = make_instrument();  // tick 5 paise
  const ValidationGate gate;

  // 100.03 = 10003 paise; 10003 % 5 != 0 -> not tick-aligned.
  SECTION("StopLossMarket trigger price must be tick-aligned (regression: was skipped)") {
    OrderIntent intent = make_entry_intent();
    intent.order_type = OrderType::StopLossMarket;
    intent.price = Price::from_paise(10003);
    GateContext ctx = base_entry(intent, instrument);
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().category == ErrorCategory::Validation);
    CHECK(names(r.error(), "tick"));
  }

  SECTION("StopLoss limit price must be tick-aligned") {
    OrderIntent intent = make_entry_intent();
    intent.order_type = OrderType::StopLoss;
    intent.price = Price::from_paise(10003);
    GateContext ctx = base_entry(intent, instrument);
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE_FALSE(r.has_value());
    CHECK(names(r.error(), "tick"));
  }

  SECTION("a tick-aligned StopLossMarket trigger passes the tick check") {
    OrderIntent intent = make_entry_intent();
    intent.order_type = OrderType::StopLossMarket;
    intent.price = Price::from_paise(10005);  // 10005 % 5 == 0
    GateContext ctx = base_entry(intent, instrument);
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE(r.has_value());  // no calendar/funds wired -> Allow
  }

  SECTION("a Market order has no price and skips the tick check") {
    OrderIntent intent = make_entry_intent();
    intent.order_type = OrderType::Market;
    intent.price = Price::from_paise(10003);  // would be misaligned, but ignored
    GateContext ctx = base_entry(intent, instrument);
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE(r.has_value());
  }
}

TEST_CASE("an instrument with no exchange is rejected, naming the exchange check", "[risk][gate][AC1]") {
  Instrument instrument = make_instrument();
  instrument.exchange = "";  // corrupt / unresolved instrument
  OrderIntent intent = make_entry_intent();
  GateContext ctx = base_entry(intent, instrument);
  ctx.allowed_exchanges = {};  // accept any non-empty exchange; empty must still fail

  const ValidationGate gate;
  const Result<GateOutcome> r = gate.validate(ctx);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().category == ErrorCategory::Validation);
  CHECK(names(r.error(), "exchange"));
}
