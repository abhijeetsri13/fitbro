#include "broker_exec/risk/risk_engine.hpp"

#include <catch2/catch_test_macros.hpp>
#include <climits>
#include <cstdint>
#include <string>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/result.hpp"

using broker_exec::Result;
using broker_exec::domain::OrderIntent;
using broker_exec::domain::OrderType;
using broker_exec::domain::Price;
using broker_exec::domain::Product;
using broker_exec::domain::Quantity;
using broker_exec::domain::Side;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::SuggestedAction;
using broker_exec::ports::Ok;
using broker_exec::risk::make_risk_check;
using broker_exec::risk::RiskEngine;
using broker_exec::risk::RiskLimits;
using broker_exec::risk::RiskState;

namespace {

// A Limit order intent (so the market-order block is OFF unless a test opts in).
[[nodiscard]] OrderIntent limit_intent() {
  return OrderIntent{.client_ref = "strat-a-0001-uuid",
                     .symbol = "NIFTY26JUL24000CE",
                     .side = Side::Buy,
                     .quantity = Quantity::of(50),
                     .price = Price::from_paise(10000),
                     .order_type = OrderType::Limit,
                     .product = Product::Intraday,
                     .strategy = "strat-a"};
}

[[nodiscard]] OrderIntent market_intent() {
  OrderIntent intent = limit_intent();
  intent.order_type = OrderType::Market;
  return intent;
}

// A "clean" limits set: every rule ARMED (non-zero / true) but with headroom so
// the clean state below passes all four levels.
[[nodiscard]] RiskLimits clean_limits() {
  RiskLimits limits;
  limits.daily_loss_limit_paise = 1'000'000;  // -10,000 rupees tolerated.
  limits.max_open_positions = 10;
  limits.max_account_margin_paise = 10'000'000;  // 100,000 rupees.
  limits.max_order_value_paise = 5'000'000;      // 50,000 rupees.
  limits.block_market_orders = true;             // a Limit order is fine.
  limits.max_slippage_bps = 50;
  limits.max_lots_per_strategy = 20;
  limits.max_lots_per_instrument = 20;
  limits.strategy_daily_loss_limit_paise = 500'000;
  return limits;
}

// A "clean" state: comfortably inside every armed limit above.
[[nodiscard]] RiskState clean_state() {
  RiskState state;
  state.account_pnl_paise = -100'000;  // small loss, well under the limit.
  state.open_positions = 2;
  state.used_margin_paise = 1'000'000;
  state.order_value_paise = 500'000;
  state.order_value_known = true;
  state.order_lots = 1;
  state.slippage_bps = 5;
  state.is_entry = true;
  state.strategy_lots = 3;
  state.strategy_pnl_paise = -50'000;
  state.strategy_enabled = true;
  state.instrument_lots = 3;
  state.data_tradable = true;
  return state;
}

// True iff `err.message` contains `needle`.
[[nodiscard]] bool has(const std::string& message, const char* needle) {
  return message.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("clean state passes every level and check_all", "[risk]") {
  const RiskEngine engine;
  const RiskLimits limits = clean_limits();
  const RiskState state = clean_state();
  const OrderIntent intent = limit_intent();

  REQUIRE(engine.check_account(limits, state));
  REQUIRE(engine.check_strategy(limits, state));
  REQUIRE(engine.check_instrument(limits, state));
  REQUIRE(engine.check_order(intent, limits, state));
  REQUIRE(engine.check_all(intent, limits, state));
}

TEST_CASE("account: daily loss limit blocks when loss reaches the magnitude", "[risk]") {
  const RiskEngine engine;
  const RiskLimits limits = clean_limits();
  RiskState state = clean_state();
  state.account_pnl_paise = -limits.daily_loss_limit_paise;  // exactly at the limit.

  const auto result = engine.check_account(limits, state);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::RiskRejected);
  CHECK(has(result.error().message, "risk[account]"));
  CHECK(has(result.error().message, "daily loss"));
}

TEST_CASE("account: max open positions blocks entries at the ceiling", "[risk]") {
  const RiskEngine engine;
  const RiskLimits limits = clean_limits();
  RiskState state = clean_state();
  state.open_positions = limits.max_open_positions;  // >= ceiling.

  const auto result = engine.check_account(limits, state);
  REQUIRE_FALSE(result);
  CHECK(has(result.error().message, "risk[account]"));
  CHECK(has(result.error().message, "open positions"));
}

TEST_CASE("account: max open positions is entries-only", "[risk]") {
  const RiskEngine engine;
  const RiskLimits limits = clean_limits();
  RiskState state = clean_state();
  state.open_positions = limits.max_open_positions;  // would trip an entry.
  state.is_entry = false;                            // ...but this is a reduce.

  CHECK(engine.check_account(limits, state));  // not tripped.
}

TEST_CASE("account: max account margin blocks when used + order value exceeds it", "[risk]") {
  const RiskEngine engine;
  const RiskLimits limits = clean_limits();
  RiskState state = clean_state();
  state.used_margin_paise = limits.max_account_margin_paise;
  state.order_value_paise = 1;  // pushes the total over.

  const auto result = engine.check_account(limits, state);
  REQUIRE_FALSE(result);
  CHECK(has(result.error().message, "risk[account]"));
  CHECK(has(result.error().message, "margin"));
}

TEST_CASE("strategy: a stopped strategy is blocked (AC-3)", "[risk]") {
  const RiskEngine engine;
  const RiskLimits limits = clean_limits();
  RiskState state = clean_state();
  state.strategy_enabled = false;

  const auto result = engine.check_strategy(limits, state);
  REQUIRE_FALSE(result);
  CHECK(has(result.error().message, "risk[strategy]"));
  CHECK(has(result.error().message, "stopped"));
}

TEST_CASE("AC-3: strategy A stopped blocks while strategy B (same state) passes", "[risk]") {
  const RiskEngine engine;
  const RiskLimits limits = clean_limits();

  RiskState strategy_a = clean_state();
  strategy_a.strategy_enabled = false;
  RiskState strategy_b = clean_state();
  strategy_b.strategy_enabled = true;  // everything else identical.

  CHECK_FALSE(engine.check_strategy(limits, strategy_a));
  CHECK(engine.check_strategy(limits, strategy_b));
}

TEST_CASE("strategy: daily loss limit blocks at the magnitude", "[risk]") {
  const RiskEngine engine;
  const RiskLimits limits = clean_limits();
  RiskState state = clean_state();
  state.strategy_pnl_paise = -limits.strategy_daily_loss_limit_paise;

  const auto result = engine.check_strategy(limits, state);
  REQUIRE_FALSE(result);
  CHECK(has(result.error().message, "risk[strategy]"));
  CHECK(has(result.error().message, "daily loss"));
}

TEST_CASE("strategy: max lots blocks when current + order lots exceeds the cap", "[risk]") {
  const RiskEngine engine;
  const RiskLimits limits = clean_limits();
  RiskState state = clean_state();
  state.strategy_lots = limits.max_lots_per_strategy;  // + order_lots(1) > cap.

  const auto result = engine.check_strategy(limits, state);
  REQUIRE_FALSE(result);
  CHECK(has(result.error().message, "risk[strategy]"));
  CHECK(has(result.error().message, "lots"));
}

TEST_CASE("instrument: max lots blocks when current + order lots exceeds the cap", "[risk]") {
  const RiskEngine engine;
  const RiskLimits limits = clean_limits();
  RiskState state = clean_state();
  state.instrument_lots = limits.max_lots_per_instrument;  // + order_lots(1) > cap.

  const auto result = engine.check_instrument(limits, state);
  REQUIRE_FALSE(result);
  CHECK(has(result.error().message, "risk[instrument]"));
  CHECK(has(result.error().message, "lots"));
}

TEST_CASE("instrument: illiquid / stale price is blocked", "[risk]") {
  const RiskEngine engine;
  const RiskLimits limits = clean_limits();
  RiskState state = clean_state();
  state.data_tradable = false;

  const auto result = engine.check_instrument(limits, state);
  REQUIRE_FALSE(result);
  CHECK(has(result.error().message, "risk[instrument]"));
  CHECK(has(result.error().message, "tradable"));
}

TEST_CASE("order: market-order block rejects a Market order", "[risk]") {
  const RiskEngine engine;
  const RiskLimits limits = clean_limits();
  const RiskState state = clean_state();

  const auto result = engine.check_order(market_intent(), limits, state);
  REQUIRE_FALSE(result);
  CHECK(has(result.error().message, "risk[order]"));
  CHECK(has(result.error().message, "market"));
}

TEST_CASE("order: max order value blocks an over-value order", "[risk]") {
  const RiskEngine engine;
  const RiskLimits limits = clean_limits();
  RiskState state = clean_state();
  state.order_value_paise = limits.max_order_value_paise + 1;

  const auto result = engine.check_order(limit_intent(), limits, state);
  REQUIRE_FALSE(result);
  CHECK(has(result.error().message, "risk[order]"));
  CHECK(has(result.error().message, "order value"));
}

TEST_CASE("order: slippage blocks when estimate exceeds the ceiling", "[risk]") {
  const RiskEngine engine;
  const RiskLimits limits = clean_limits();
  RiskState state = clean_state();
  state.slippage_bps = limits.max_slippage_bps + 1;

  const auto result = engine.check_order(limit_intent(), limits, state);
  REQUIRE_FALSE(result);
  CHECK(has(result.error().message, "risk[order]"));
  CHECK(has(result.error().message, "slippage"));
}

TEST_CASE("no limit: 0/false limits never block even at extreme state", "[risk]") {
  const RiskEngine engine;
  const RiskLimits off;  // every field default = 0 / false = off.

  RiskState extreme = clean_state();
  extreme.account_pnl_paise = -1'000'000'000;  // huge loss.
  extreme.open_positions = 1'000'000;
  extreme.used_margin_paise = 1'000'000'000;
  extreme.order_value_paise = 1'000'000'000;
  extreme.order_value_known = false;  // unknown — but no armed limit needs it.
  extreme.order_lots = 1'000'000;
  extreme.slippage_bps = 1'000'000;
  extreme.strategy_lots = 1'000'000;
  extreme.strategy_pnl_paise = -1'000'000'000;
  extreme.instrument_lots = 1'000'000;
  // strategy_enabled and data_tradable stay true (false would block regardless
  // of any limit, by design — they are state flags, not numeric ceilings).

  CHECK(engine.check_all(market_intent(), off, extreme));
}

TEST_CASE("check_all ordering: an account violation is named before an order one", "[risk]") {
  const RiskEngine engine;
  const RiskLimits limits = clean_limits();

  RiskState state = clean_state();
  state.account_pnl_paise = -limits.daily_loss_limit_paise;  // account violation.
  state.slippage_bps = limits.max_slippage_bps + 1;          // order violation too.

  const auto result = engine.check_all(market_intent(), limits, state);
  REQUIRE_FALSE(result);
  CHECK(has(result.error().message, "risk[account]"));  // account wins.
}

TEST_CASE("fail-closed: unknown order value blocks an armed margin limit", "[risk]") {
  const RiskEngine engine;
  RiskLimits limits;
  limits.max_account_margin_paise = 10'000'000;  // armed.

  RiskState state = clean_state();
  state.order_value_known = false;  // cannot evaluate the margin.

  const auto result = engine.check_account(limits, state);
  REQUIRE_FALSE(result);
  CHECK(has(result.error().message, "risk[account]"));
  CHECK(has(result.error().message, "margin"));
}

TEST_CASE("fail-closed: unknown order value blocks an armed order-value limit", "[risk]") {
  const RiskEngine engine;
  RiskLimits limits;
  limits.max_order_value_paise = 5'000'000;  // armed.

  RiskState state = clean_state();
  state.order_value_known = false;

  const auto result = engine.check_order(limit_intent(), limits, state);
  REQUIRE_FALSE(result);
  CHECK(has(result.error().message, "risk[order]"));
  CHECK(has(result.error().message, "order value"));
}

TEST_CASE("fail-closed: unknown order value does NOT block when those limits are off", "[risk]") {
  const RiskEngine engine;
  RiskLimits limits = clean_limits();
  limits.max_account_margin_paise = 0;  // off.
  limits.max_order_value_paise = 0;     // off.

  RiskState state = clean_state();
  state.order_value_known = false;

  CHECK(engine.check_account(limits, state));
  CHECK(engine.check_order(limit_intent(), limits, state));
}

TEST_CASE("boundary: strict-> rules PASS exactly at the limit", "[risk]") {
  const RiskEngine engine;
  const RiskLimits limits = clean_limits();

  SECTION("account: used + order value == max account margin passes") {
    RiskState state = clean_state();
    state.used_margin_paise = limits.max_account_margin_paise - state.order_value_paise;
    // used + order_value == max_account_margin_paise exactly.
    REQUIRE(engine.check_account(limits, state).has_value());
  }

  SECTION("order: order value == max order value passes") {
    RiskState state = clean_state();
    state.order_value_paise = limits.max_order_value_paise;  // exactly at the limit.
    REQUIRE(engine.check_order(limit_intent(), limits, state).has_value());
  }

  SECTION("order: slippage == max slippage passes") {
    RiskState state = clean_state();
    state.slippage_bps = limits.max_slippage_bps;  // exactly at the limit.
    REQUIRE(engine.check_order(limit_intent(), limits, state).has_value());
  }

  SECTION("strategy: strategy lots + order lots == max lots passes") {
    RiskState state = clean_state();
    state.strategy_lots = limits.max_lots_per_strategy - state.order_lots;
    // strategy_lots + order_lots == max_lots_per_strategy exactly.
    REQUIRE(engine.check_strategy(limits, state).has_value());
  }

  SECTION("instrument: instrument lots + order lots == max lots passes") {
    RiskState state = clean_state();
    state.instrument_lots = limits.max_lots_per_instrument - state.order_lots;
    // instrument_lots + order_lots == max_lots_per_instrument exactly.
    REQUIRE(engine.check_instrument(limits, state).has_value());
  }

  SECTION("account: open positions == ceiling - 1 passes (>= rule, off-by-one)") {
    RiskState state = clean_state();
    state.open_positions = limits.max_open_positions - 1;  // one below the >= ceiling.
    REQUIRE(engine.check_account(limits, state).has_value());
  }
}

TEST_CASE("check_all ordering: strategy is named before instrument and order", "[risk]") {
  const RiskEngine engine;
  const RiskLimits limits = clean_limits();

  RiskState state = clean_state();
  state.strategy_lots = limits.max_lots_per_strategy;      // strategy violation.
  state.instrument_lots = limits.max_lots_per_instrument;  // instrument violation too.
  state.slippage_bps = limits.max_slippage_bps + 1;        // order violation too.

  const auto result = engine.check_all(market_intent(), limits, state);
  REQUIRE_FALSE(result);
  CHECK(has(result.error().message, "risk[strategy]"));  // strategy wins over both.
}

TEST_CASE("make_risk_check returns the same verdict as check_all", "[risk]") {
  const RiskEngine engine;
  const RiskLimits limits = clean_limits();
  const OrderIntent intent = limit_intent();

  SECTION("passing case") {
    const RiskState state = clean_state();
    const auto check = make_risk_check(engine, intent, limits, state);
    CHECK(static_cast<bool>(check()) == static_cast<bool>(engine.check_all(intent, limits, state)));
    CHECK(check());
  }

  SECTION("rejecting case names the same rule") {
    RiskState state = clean_state();
    state.account_pnl_paise = -limits.daily_loss_limit_paise;  // account violation.

    const auto check = make_risk_check(engine, intent, limits, state);
    const auto direct = engine.check_all(intent, limits, state);
    const auto bound = check();
    REQUIRE_FALSE(bound);
    REQUIRE_FALSE(direct);
    CHECK(bound.error().message == direct.error().message);
  }
}

// ── the SuggestedAction on a locally-decided verdict ─────────────────────────

TEST_CASE("every rejection carries BlockStrategy, not RiskRejected's ReconcileFirst", "[risk]") {
  const RiskEngine engine;
  const RiskLimits limits = clean_limits();

  // One state per LEVEL, each tripping exactly that level's rule. The action is
  // load-bearing, not cosmetic: ReconcileFirst (the category's default) means
  // "resolve against broker truth before any decision", and none of these verdicts
  // change after a reconcile — the limit is local operator config. It also
  // classifies as an auto-restartable Crash on the boot exit-code path.
  RiskState account = clean_state();
  account.account_pnl_paise = -limits.daily_loss_limit_paise;
  RiskState strategy = clean_state();
  strategy.strategy_enabled = false;
  RiskState instrument = clean_state();
  instrument.data_tradable = false;
  RiskState order = clean_state();
  order.slippage_bps = limits.max_slippage_bps + 1;

  const auto a = engine.check_account(limits, account);
  REQUIRE_FALSE(a);
  CHECK(a.error().category == ErrorCategory::RiskRejected);
  CHECK(a.error().action == SuggestedAction::BlockStrategy);

  const auto s = engine.check_strategy(limits, strategy);
  REQUIRE_FALSE(s);
  CHECK(s.error().action == SuggestedAction::BlockStrategy);

  const auto i = engine.check_instrument(limits, instrument);
  REQUIRE_FALSE(i);
  CHECK(i.error().action == SuggestedAction::BlockStrategy);

  const auto o = engine.check_order(limit_intent(), limits, order);
  REQUIRE_FALSE(o);
  CHECK(o.error().action == SuggestedAction::BlockStrategy);

  // And it survives the bound closure the gate actually calls, which is the form
  // the verdict reaches the strategy in.
  const auto bound = make_risk_check(engine, limit_intent(), limits, account)();
  REQUIRE_FALSE(bound);
  CHECK(bound.error().category == ErrorCategory::RiskRejected);
  CHECK(bound.error().action == SuggestedAction::BlockStrategy);
}

// ── no armed rule may FORM the sum it compares ───────────────────────────────

TEST_CASE("account margin: a near-INT64_MAX used margin cannot WRAP past the ceiling", "[risk]") {
  const RiskEngine engine;
  RiskLimits limits;
  limits.max_account_margin_paise = 10'000'000;  // armed.

  RiskState state = clean_state();
  state.used_margin_paise = INT64_MAX - 10;  // absurd, but caller-supplied.
  state.order_value_paise = 1'000;           // used + value overflows int64.

  // Forming the sum wrapped it to a large NEGATIVE total, which is <= any positive
  // ceiling, so the one account-level rule that bounds total leverage returned ok()
  // at exactly the extreme it exists to catch (and the add itself was UB).
  const auto result = engine.check_account(limits, state);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::RiskRejected);
  CHECK(has(result.error().message, "risk[account]"));
  CHECK(has(result.error().message, "margin"));
}

TEST_CASE("account margin: a NEGATIVE input is refused, not netted off the ceiling", "[risk]") {
  const RiskEngine engine;
  RiskLimits limits;
  limits.max_account_margin_paise = 10'000'000;  // armed.

  SECTION("a negative used margin would make any order 'fit'") {
    RiskState state = clean_state();
    state.used_margin_paise = -1'000'000'000;
    state.order_value_paise = 500'000'000;  // fifty times the armed ceiling.

    const auto result = engine.check_account(limits, state);
    REQUIRE_FALSE(result);
    CHECK(has(result.error().message, "risk[account]"));
    CHECK(has(result.error().message, "margin"));
  }

  SECTION("a negative order value shrinks the committed total") {
    RiskState state = clean_state();
    state.order_value_paise = -1;

    const auto result = engine.check_account(limits, state);
    REQUIRE_FALSE(result);
    CHECK(has(result.error().message, "risk[account]"));
  }
}

TEST_CASE("order value: a negative notional is refused by an armed ceiling", "[risk]") {
  const RiskEngine engine;
  RiskLimits limits;
  limits.max_order_value_paise = 5'000'000;  // armed; nothing else is.

  RiskState state = clean_state();
  state.order_value_paise = -1;  // passed `> max_order_value_paise` before.

  const auto result = engine.check_order(limit_intent(), limits, state);
  REQUIRE_FALSE(result);
  CHECK(has(result.error().message, "risk[order]"));
  CHECK(has(result.error().message, "order value"));
}

TEST_CASE("lot caps: the int sum cannot wrap, and a negative count is refused", "[risk]") {
  const RiskEngine engine;
  RiskLimits limits;
  limits.max_lots_per_strategy = 20;
  limits.max_lots_per_instrument = 20;

  SECTION("near-INT_MAX current lots + this order's lots overflows `int`") {
    RiskState state = clean_state();
    state.strategy_lots = INT_MAX - 1;
    state.instrument_lots = INT_MAX - 1;
    state.order_lots = 100;  // current + order wraps large-NEGATIVE, i.e. "under".

    const auto s = engine.check_strategy(limits, state);
    REQUIRE_FALSE(s);
    CHECK(has(s.error().message, "risk[strategy]"));

    const auto i = engine.check_instrument(limits, state);
    REQUIRE_FALSE(i);
    CHECK(has(i.error().message, "risk[instrument]"));
  }

  SECTION("a negative lot count is refused rather than netted off the cap") {
    RiskState state = clean_state();
    state.strategy_lots = -1'000;
    state.instrument_lots = -1'000;
    state.order_lots = 500;  // far over the cap of 20, but -1000 + 500 < 20.

    const auto s = engine.check_strategy(limits, state);
    REQUIRE_FALSE(s);
    CHECK(has(s.error().message, "risk[strategy]"));
    CHECK(has(s.error().message, "lots"));

    const auto i = engine.check_instrument(limits, state);
    REQUIRE_FALSE(i);
    CHECK(has(i.error().message, "risk[instrument]"));
    CHECK(has(i.error().message, "lots"));
  }

  SECTION("a negative ORDER lot count is refused too") {
    RiskState state = clean_state();
    state.order_lots = -1;

    CHECK_FALSE(engine.check_strategy(limits, state));
    CHECK_FALSE(engine.check_instrument(limits, state));
  }
}
