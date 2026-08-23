#include "broker_exec/risk/risk_engine.hpp"

#include <string>
#include <utility>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/errors/error.hpp"

namespace broker_exec::risk {

namespace {

using errors::Error;
using errors::ErrorCategory;
using errors::make_error;
using errors::SuggestedAction;

// Every risk reject NAMES the failing level + rule (AC-1) with a stable prefix
// "risk[<level>]: <rule>", as a fresh RiskRejected Error whose action is set
// EXPLICITLY to BlockStrategy.
//
// WHY NOT THE CATEGORY DEFAULT: RiskRejected defaults to ReconcileFirst —
// "resolve against broker truth before any decision" — which is right for a
// BROKER RMS refusal and wrong for every verdict here. These are all computed
// LOCALLY from operator config: a breached daily-loss limit, a stopped strategy
// or an untradable price is exactly as breached after a reconcile, so that action
// sends the runtime round the loop again against a hard stop instead of halting
// the strategy, and boot::exit_class_for maps ReconcileFirst to the
// auto-restartable ExitClass::Crash. Mirrors every sibling that decides locally
// (validation_gate.cpp's kill-switch / UNKNOWN-pause, isolation/strategy_book.cpp,
// modes/posture.cpp). The gate's wrap_error PRESERVES this action, so it is the
// verdict the strategy actually switches on.
[[nodiscard]] Error rejected(const char* level, const std::string& rule) {
  Error err = make_error(ErrorCategory::RiskRejected, std::string("risk[") + level + "]: " + rule);
  err.action = SuggestedAction::BlockStrategy;
  return err;
}

// `a + b > ceiling` for an ARMED (positive) ceiling, decided WITHOUT ever forming
// the sum. Callers prove `a` and `b` non-negative first, so `ceiling - b` cannot
// underflow. FORMING the sum is the bug this exists to prevent: signed overflow is
// UB, and on the usual two's-complement wrap the total lands large-NEGATIVE, which
// is <= any positive ceiling — so the armed limit PASSES at exactly the extreme it
// exists to catch, and the fail-open lands on the rules that bound total leverage
// and total lots. marginsafety/margin_buffer.cpp holds the same invariant with
// sat_add ("NO INTEGER WRAP"); risk was the last place adding two caller-supplied
// numbers raw.
template <typename T>
[[nodiscard]] constexpr bool sum_over(T a, T b, T ceiling) noexcept {
  return a > ceiling - b;
}

}  // namespace

Result<ports::Ok> RiskEngine::check_account(const RiskLimits& limits,
                                            const RiskState& state) const {
  // ── daily loss: pnl is signed (negative = loss); the limit is a positive
  // magnitude. Trip when the loss reaches or exceeds the limit. 0 = off. ──────
  if (limits.daily_loss_limit_paise > 0 &&
      state.account_pnl_paise <= -limits.daily_loss_limit_paise) {
    return fail(rejected("account", "daily loss limit breached"));
  }

  // ── max open positions: entries only — a risk-reducing op never trips it. ──
  if (limits.max_open_positions > 0 && state.is_entry &&
      state.open_positions >= limits.max_open_positions) {
    return fail(rejected("account", "max open positions reached"));
  }

  // ── max account margin: needs this order's value. If the value is unknown
  // (Market with no estimate) the SET limit cannot be evaluated, so FAIL-CLOSED
  // rather than silently pass a margin check we cannot verify. ────────────────
  if (limits.max_account_margin_paise > 0) {
    if (!state.order_value_known) {
      return fail(rejected("account", "cannot verify margin (order value unknown)"));
    }
    // A NEGATIVE used-margin or order value is not a number this rule can
    // evaluate — both are magnitudes — and it is the shape that drags the total
    // DOWN under the ceiling. Refuse it rather than compare against it.
    if (state.used_margin_paise < 0 || state.order_value_paise < 0) {
      return fail(rejected("account", "cannot verify margin (negative margin/order value)"));
    }
    if (sum_over(state.used_margin_paise, state.order_value_paise,
                 limits.max_account_margin_paise)) {
      return fail(rejected("account", "max account margin exceeded"));
    }
  }

  return ports::ok();
}

Result<ports::Ok> RiskEngine::check_strategy(const RiskLimits& limits,
                                             const RiskState& state) const {
  // ── strategy stopped (AC-3): this strategy only is halted. ─────────────────
  if (!state.strategy_enabled) {
    return fail(rejected("strategy", "strategy is stopped"));
  }

  // ── strategy daily loss: signed pnl vs positive-magnitude limit. 0 = off. ──
  if (limits.strategy_daily_loss_limit_paise > 0 &&
      state.strategy_pnl_paise <= -limits.strategy_daily_loss_limit_paise) {
    return fail(rejected("strategy", "strategy daily loss limit breached"));
  }

  // ── strategy max lots: current + this order's lots. 0 = off. ───────────────
  // Same wrap hazard as the account margin above, one size down: these are `int`,
  // so the sum overflows four billion times sooner. A negative lot count is not a
  // position, so it is refused rather than netted off the cap.
  if (limits.max_lots_per_strategy > 0) {
    if (state.strategy_lots < 0 || state.order_lots < 0) {
      return fail(rejected("strategy", "cannot verify lots (negative lot count)"));
    }
    if (sum_over(state.strategy_lots, state.order_lots, limits.max_lots_per_strategy)) {
      return fail(rejected("strategy", "max lots per strategy exceeded"));
    }
  }

  return ports::ok();
}

Result<ports::Ok> RiskEngine::check_instrument(const RiskLimits& limits,
                                               const RiskState& state) const {
  // ── instrument max lots: current + this order's lots. 0 = off. ─────────────
  // Non-negative first, then compared by subtraction — see check_strategy.
  if (limits.max_lots_per_instrument > 0) {
    if (state.instrument_lots < 0 || state.order_lots < 0) {
      return fail(rejected("instrument", "cannot verify lots (negative lot count)"));
    }
    if (sum_over(state.instrument_lots, state.order_lots, limits.max_lots_per_instrument)) {
      return fail(rejected("instrument", "max lots per instrument exceeded"));
    }
  }

  // ── illiquid / stale price -> not tradable. ────────────────────────────────
  if (!state.data_tradable) {
    return fail(rejected("instrument", "price not tradable (illiquid/stale)"));
  }

  return ports::ok();
}

Result<ports::Ok> RiskEngine::check_order(const domain::OrderIntent& intent,
                                          const RiskLimits& limits, const RiskState& state) const {
  // ── market-order block: a posture flag, evaluated from the intent only. ────
  if (limits.block_market_orders && intent.order_type == domain::OrderType::Market) {
    return fail(rejected("order", "market orders are blocked"));
  }

  // ── max order value: needs this order's value; FAIL-CLOSED if unknown. ─────
  if (limits.max_order_value_paise > 0) {
    if (!state.order_value_known) {
      return fail(rejected("order", "cannot verify order value (unknown)"));
    }
    // A negative notional is not a smaller order, it is corrupt input, and it
    // slips under ANY armed ceiling. Refused for the same reason check_account
    // refuses it. (slippage_bps below is deliberately NOT guarded this way: a
    // negative slippage estimate is price IMPROVEMENT, a real and benign value.)
    if (state.order_value_paise < 0) {
      return fail(rejected("order", "cannot verify order value (negative)"));
    }
    if (state.order_value_paise > limits.max_order_value_paise) {
      return fail(rejected("order", "max order value exceeded"));
    }
  }

  // ── slippage: estimated bps vs ceiling. 0 = off. ───────────────────────────
  if (limits.max_slippage_bps > 0 && state.slippage_bps > limits.max_slippage_bps) {
    return fail(rejected("order", "max slippage exceeded"));
  }

  return ports::ok();
}

Result<ports::Ok> RiskEngine::check_all(const domain::OrderIntent& intent, const RiskLimits& limits,
                                        const RiskState& state) const {
  // Fixed order, fail-closed: account -> strategy -> instrument -> order. The
  // FIRST violation wins and is returned named (AC-1).
  if (auto account = check_account(limits, state); !account) {
    return account;
  }
  if (auto strategy = check_strategy(limits, state); !strategy) {
    return strategy;
  }
  if (auto instrument = check_instrument(limits, state); !instrument) {
    return instrument;
  }
  if (auto order = check_order(intent, limits, state); !order) {
    return order;
  }
  return ports::ok();
}

std::function<Result<ports::Ok>()> make_risk_check(const RiskEngine& engine,
                                                   const domain::OrderIntent& intent,
                                                   RiskLimits limits, RiskState state) {
  // Capture a snapshot BY VALUE (intent / limits / state) so the closure is
  // self-contained with no external lifetime dependency: it holds NO reference
  // to anything, so it cannot dangle if any argument (including the engine) is
  // destroyed first. The engine is stateless, so the closure constructs a fresh
  // one rather than referencing the caller's. Drops into GateContext::risk_check.
  (void)engine;
  return [intent, limits = std::move(limits), state = std::move(state)]() -> Result<ports::Ok> {
    return RiskEngine{}.check_all(intent, limits, state);
  };
}

}  // namespace broker_exec::risk
