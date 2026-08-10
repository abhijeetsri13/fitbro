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

// Every risk reject NAMES the failing level + rule (AC-1) with a stable prefix
// "risk[<level>]: <rule>". Built as a fresh RiskRejected Error (SuggestedAction
// default for the category).
[[nodiscard]] Error rejected(const char* level, const std::string& rule) {
  return make_error(ErrorCategory::RiskRejected,
                    std::string("risk[") + level + "]: " + rule);
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
    if (state.used_margin_paise + state.order_value_paise > limits.max_account_margin_paise) {
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
  if (limits.max_lots_per_strategy > 0 &&
      state.strategy_lots + state.order_lots > limits.max_lots_per_strategy) {
    return fail(rejected("strategy", "max lots per strategy exceeded"));
  }

  return ports::ok();
}

Result<ports::Ok> RiskEngine::check_instrument(const RiskLimits& limits,
                                               const RiskState& state) const {
  // ── instrument max lots: current + this order's lots. 0 = off. ─────────────
  if (limits.max_lots_per_instrument > 0 &&
      state.instrument_lots + state.order_lots > limits.max_lots_per_instrument) {
    return fail(rejected("instrument", "max lots per instrument exceeded"));
  }

  // ── illiquid / stale price -> not tradable. ────────────────────────────────
  if (!state.data_tradable) {
    return fail(rejected("instrument", "price not tradable (illiquid/stale)"));
  }

  return ports::ok();
}

Result<ports::Ok> RiskEngine::check_order(const domain::OrderIntent& intent,
                                          const RiskLimits& limits,
                                          const RiskState& state) const {
  // ── market-order block: a posture flag, evaluated from the intent only. ────
  if (limits.block_market_orders && intent.order_type == domain::OrderType::Market) {
    return fail(rejected("order", "market orders are blocked"));
  }

  // ── max order value: needs this order's value; FAIL-CLOSED if unknown. ─────
  if (limits.max_order_value_paise > 0) {
    if (!state.order_value_known) {
      return fail(rejected("order", "cannot verify order value (unknown)"));
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

Result<ports::Ok> RiskEngine::check_all(const domain::OrderIntent& intent,
                                        const RiskLimits& limits,
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
  return [intent, limits = std::move(limits),
          state = std::move(state)]() -> Result<ports::Ok> {
    return RiskEngine{}.check_all(intent, limits, state);
  };
}

}  // namespace broker_exec::risk
