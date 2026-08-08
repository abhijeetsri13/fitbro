#include "broker_exec/risk/validation_gate.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
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
    // PRE-IMP-11 this section drove `price`, because the gate tick-checked an SL-M's
    // limit as a stand-in for a trigger the domain could not carry. It now drives
    // the REAL field, which is the whole point of the change.
    OrderIntent intent = make_entry_intent();
    intent.order_type = OrderType::StopLossMarket;
    intent.trigger_price = Price::from_paise(10003);
    GateContext ctx = base_entry(intent, instrument);
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().category == ErrorCategory::Validation);
    CHECK(names(r.error(), "tick"));
    // And it names WHICH price, so an SL's two numbers are distinguishable.
    CHECK(r.error().message.find("trigger price") != std::string::npos);
  }

  SECTION("StopLoss limit price must be tick-aligned") {
    OrderIntent intent = make_entry_intent();
    intent.order_type = OrderType::StopLoss;
    intent.side = Side::Sell;  // so limit <= trigger holds and ORDERING is not what fires
    intent.price = Price::from_paise(10003);
    intent.trigger_price = Price::from_paise(10005);  // trigger fine, limit is not
    GateContext ctx = base_entry(intent, instrument);
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE_FALSE(r.has_value());
    CHECK(names(r.error(), "tick"));
  }

  SECTION("StopLoss TRIGGER price must be tick-aligned independently of the limit") {
    OrderIntent intent = make_entry_intent();
    intent.order_type = OrderType::StopLoss;
    intent.price = Price::from_paise(10005);         // limit fine
    intent.trigger_price = Price::from_paise(10003);  // trigger is not
    GateContext ctx = base_entry(intent, instrument);
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE_FALSE(r.has_value());
    CHECK(names(r.error(), "tick"));
    CHECK(r.error().message.find("trigger price") != std::string::npos);
  }

  SECTION("a tick-aligned StopLossMarket trigger passes the tick check") {
    OrderIntent intent = make_entry_intent();
    intent.order_type = OrderType::StopLossMarket;
    intent.trigger_price = Price::from_paise(10005);  // 10005 % 5 == 0
    GateContext ctx = base_entry(intent, instrument);
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE(r.has_value());  // no calendar/funds wired -> Allow
  }

  SECTION("an SL-M's IGNORED limit price is not tick-checked") {
    // SL-M fires a market order; its `price` never reaches the broker, so
    // tick-checking it would refuse a perfectly well-formed protective order over
    // a leftover number. See the shape matrix in validation_gate.cpp.
    OrderIntent intent = make_entry_intent();
    intent.order_type = OrderType::StopLossMarket;
    intent.price = Price::from_paise(10003);          // misaligned, and ignored
    intent.trigger_price = Price::from_paise(10005);  // the number that matters
    GateContext ctx = base_entry(intent, instrument);
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE(r.has_value());
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

// ── IMP-11 AC-1: the (order_type x price fields) SHAPE MATRIX ────────────────
//
// Four order types x trigger-present/absent = eight cells, all asserted below.
// The matrix is fail-closed in BOTH directions: a stop with no trigger is refused
// (it would reach the broker unarmed or be rejected), and a non-stop carrying a
// trigger is refused too (the adapters do not transmit it, so accepting it would
// place an UNPROTECTED order for a caller who believes a stop is armed).
TEST_CASE("order-shape: SL/SL-M require a trigger, Limit/Market forbid one",
          "[risk][gate][IMP-11]") {
  const Instrument instrument = make_instrument();
  const ValidationGate gate;
  const Price kAligned = Price::from_paise(10005);  // tick 5 paise

  // Build an intent of `type`, optionally armed, with tick-clean prices so the
  // ONLY thing under test is the shape.
  const auto intent_of = [&](OrderType type, bool with_trigger) {
    OrderIntent intent = make_entry_intent();
    intent.order_type = type;
    intent.price = kAligned;
    if (with_trigger) {
      intent.trigger_price = kAligned;
    }
    return intent;
  };

  SECTION("the four WELL-FORMED cells are allowed") {
    const auto ok_case = [&](OrderType type, bool with_trigger) {
      const OrderIntent intent = intent_of(type, with_trigger);
      GateContext ctx = base_entry(intent, instrument);
      const Result<GateOutcome> r = gate.validate(ctx);
      REQUIRE(r.has_value());
      CHECK(r.value() == GateOutcome::Allow);
    };
    ok_case(OrderType::Market, /*with_trigger=*/false);
    ok_case(OrderType::Limit, /*with_trigger=*/false);
    ok_case(OrderType::StopLoss, /*with_trigger=*/true);
    ok_case(OrderType::StopLossMarket, /*with_trigger=*/true);
  }

  SECTION("the four MALFORMED cells are refused, naming the order-shape check") {
    const auto bad_case = [&](OrderType type, bool with_trigger) {
      const OrderIntent intent = intent_of(type, with_trigger);
      GateContext ctx = base_entry(intent, instrument);
      const Result<GateOutcome> r = gate.validate(ctx);
      REQUIRE_FALSE(r.has_value());
      CHECK(r.error().category == ErrorCategory::Validation);
      CHECK(names(r.error(), "order-shape"));
    };
    bad_case(OrderType::StopLoss, /*with_trigger=*/false);        // stop with no trigger
    bad_case(OrderType::StopLossMarket, /*with_trigger=*/false);  // ditto
    bad_case(OrderType::Limit, /*with_trigger=*/true);            // trigger on a limit
    bad_case(OrderType::Market, /*with_trigger=*/true);           // trigger on a market
  }

  SECTION("an SL requires a POSITIVE limit price as well as a trigger") {
    OrderIntent intent = intent_of(OrderType::StopLoss, /*with_trigger=*/true);
    intent.side = Side::Sell;             // 0 <= trigger, so ORDERING passes...
    intent.price = Price::from_paise(0);  // ...and the missing limit is what fires
    GateContext ctx = base_entry(intent, instrument);
    const Result<GateOutcome> r = gate.validate(ctx);
    REQUIRE_FALSE(r.has_value());
    CHECK(names(r.error(), "tick"));  // the "must be positive" branch
  }
}

// ── IMP-11 MEDIUM-5: an SL's two prices must be SIDE-ORDERED ─────────────────
//
// A stop-loss limit only works if its limit sits on the far side of its trigger.
// A swapped pair is a shape bug the BROKER ACCEPTS HAPPILY: a sell stop whose
// limit sits above its trigger arms only once the market has fallen past a level
// it then refuses to sell at, so it sits unfillable through the entire move it
// existed to escape. The caller believes they are protected and they are not,
// which is why this has to be caught here rather than at the exchange.
TEST_CASE("order-shape: an SL's limit must be on the correct side of its trigger",
          "[risk][gate][IMP-11]") {
  const Instrument instrument = make_instrument();  // tick 5 paise
  const ValidationGate gate;

  const auto sl = [&](Side side, std::int64_t limit_paise, std::int64_t trigger_paise) {
    OrderIntent intent = make_entry_intent();
    intent.order_type = OrderType::StopLoss;
    intent.side = side;
    intent.price = Price::from_paise(limit_paise);
    intent.trigger_price = Price::from_paise(trigger_paise);
    return intent;
  };
  const auto verdict = [&](const OrderIntent& intent, bool is_exit) {
    GateContext ctx = base_entry(intent, instrument);
    ctx.is_risk_reducing = is_exit;
    return gate.validate(ctx);
  };

  SECTION("SELL SL: limit BELOW trigger is well-formed; ABOVE is refused") {
    // Arms as the market falls through 100.00, then works down to 99.50.
    CHECK(verdict(sl(Side::Sell, 9950, 10000), false).has_value());

    const Result<GateOutcome> bad = verdict(sl(Side::Sell, 10050, 10000), false);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().category == ErrorCategory::Validation);
    CHECK(names(bad.error(), "order-shape"));
    CHECK(bad.error().message.find("wrong side") != std::string::npos);
  }

  SECTION("BUY SL: limit ABOVE trigger is well-formed; BELOW is refused") {
    // Arms as the market rises through 100.00, then works up to 100.50.
    CHECK(verdict(sl(Side::Buy, 10050, 10000), false).has_value());

    const Result<GateOutcome> bad = verdict(sl(Side::Buy, 9950, 10000), false);
    REQUIRE_FALSE(bad.has_value());
    CHECK(names(bad.error(), "order-shape"));
  }

  SECTION("BOUNDARY: limit == trigger is allowed on both sides") {
    // The common "stop at the touch" order; refusing it would be a false positive
    // on the single most ordinary stop shape there is.
    CHECK(verdict(sl(Side::Sell, 10000, 10000), false).has_value());
    CHECK(verdict(sl(Side::Buy, 10000, 10000), false).has_value());
  }

  SECTION("the ordering rule applies to EXITS too") {
    // An exit is exempt from the entry-only blocks, never from shape validation —
    // and a malformed exit stop is the dangerous one: it is the leg standing
    // between a live position and an unbounded loss.
    const Result<GateOutcome> bad = verdict(sl(Side::Sell, 10050, 10000), /*is_exit=*/true);
    REQUIRE_FALSE(bad.has_value());
    CHECK(names(bad.error(), "order-shape"));

    // ...and a correctly-ordered exit stop still sails through.
    CHECK(verdict(sl(Side::Sell, 9950, 10000), /*is_exit=*/true).has_value());
  }

  SECTION("SL-M has no limit, so no ordering rule constrains it") {
    // Its `price` is ignored end-to-end; an absurd value must not be read as a
    // swapped pair and refuse a perfectly good protective order.
    OrderIntent slm = sl(Side::Sell, 99900, 10000);  // limit far ABOVE the trigger
    slm.order_type = OrderType::StopLossMarket;
    CHECK(verdict(slm, false).has_value());
  }
}

TEST_CASE("order-shape is NOT exit-exempt: a malformed protective order is still refused",
          "[risk][gate][IMP-11]") {
  // A risk-reducing op skips the ENTRY-ONLY blocks, never shape validation. A stop
  // exit with no trigger is not protection — it is a broker rejection at the exact
  // moment protection was needed, so it must be refused here and not sent.
  const Instrument instrument = make_instrument();
  const ValidationGate gate;

  OrderIntent intent = make_entry_intent();
  intent.order_type = OrderType::StopLossMarket;  // and NO trigger
  GateContext ctx = base_entry(intent, instrument);
  ctx.is_risk_reducing = true;
  ctx.kill_entry_block = true;      // exempt
  ctx.unknown_pause_active = true;  // exempt

  const Result<GateOutcome> r = gate.validate(ctx);
  REQUIRE_FALSE(r.has_value());
  CHECK(names(r.error(), "order-shape"));

  // The SAME exit, correctly shaped, sails through every entry-only block.
  OrderIntent armed = intent;
  armed.trigger_price = Price::from_paise(10005);
  GateContext ok_ctx = base_entry(armed, instrument);
  ok_ctx.is_risk_reducing = true;
  ok_ctx.kill_entry_block = true;
  ok_ctx.unknown_pause_active = true;
  const Result<GateOutcome> ok = gate.validate(ok_ctx);
  REQUIRE(ok.has_value());
  CHECK(ok.value() == GateOutcome::Allow);
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
