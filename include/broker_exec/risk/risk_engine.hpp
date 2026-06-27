#pragma once

// broker_exec::risk — the four-level risk engine (Story 2.10, FR-15).
//
// A stateless evaluator that enforces configured limits across FOUR independent
// levels — account, strategy, instrument and order — on every order. Each level
// is separately callable (AC-2); `check_all` composes them in a fixed order and
// the FIRST violation wins, named ("risk[<level>]: <rule>"). A strategy can be
// stopped (strategy_enabled=false) without stopping any other (AC-3).
//
// The engine is pure logic over (OrderIntent, RiskLimits, RiskState): no clock,
// no I/O, no broker contact. It is the implementation the runtime binds into the
// validation gate's injected `risk_check` (Story 2.8, GateContext::risk_check)
// via `make_risk_check`. Exits (risk-reducing ops) are NOT special-cased here —
// the GATE decides whether to even call risk for an exit (Story 2.8).
//
// Conventions: no double/float (paise are int64, bps/counts are int); a limit of
// 0 / a flag of false means "no limit" and NEVER blocks; a SET limit that cannot
// be evaluated (unknown order value -> margin + order-value only) is FAIL-CLOSED.
// Slippage is supplied directly (slippage_bps) and is NOT gated on order value.
// Every reject is a RiskRejected Error (SuggestedAction default).
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.
// Result<Ok> is no-throw.

#include <cstdint>
#include <functional>

#include "broker_exec/domain/types.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::risk {

// The configured ceilings/flags the engine enforces. Every field is an integer
// (paise / count / basis points) so no float ever touches the risk path. The
// disabled sentinel is uniform: 0 (or false) means "no limit" and NEVER blocks,
// even at extreme state values. A positive value (or true) arms the rule.
struct RiskLimits {
  // ── account level ────────────────────────────────────────────────────────
  // Max tolerated account loss as a POSITIVE magnitude in paise. The rule trips
  // when account_pnl_paise <= -daily_loss_limit_paise. 0 = off.
  std::int64_t daily_loss_limit_paise = 0;
  // Max number of concurrently open positions (entries only). 0 = off.
  int max_open_positions = 0;
  // Max total account margin in paise (used + this order's value). 0 = off.
  std::int64_t max_account_margin_paise = 0;

  // ── order level ──────────────────────────────────────────────────────────
  // Max notional value of a single order in paise. 0 = off.
  std::int64_t max_order_value_paise = 0;
  // When true, plain Market orders are blocked outright. false = off.
  bool block_market_orders = false;
  // Max tolerated estimated slippage in basis points. 0 = off.
  int max_slippage_bps = 0;

  // ── strategy / instrument lot caps ───────────────────────────────────────
  // Max open lots a single strategy may hold (current + this order). 0 = off.
  int max_lots_per_strategy = 0;
  // Max open lots a single instrument may hold (current + this order). 0 = off.
  int max_lots_per_instrument = 0;
  // Max tolerated per-strategy loss as a POSITIVE magnitude in paise. The rule
  // trips when strategy_pnl_paise <= -strategy_daily_loss_limit_paise. 0 = off.
  std::int64_t strategy_daily_loss_limit_paise = 0;
};

// The current world the order is evaluated against (injected by the runtime; the
// real wiring lands in later stories). The engine reads it; it never mutates it.
struct RiskState {
  // ── account ──────────────────────────────────────────────────────────────
  std::int64_t account_pnl_paise = 0;   // Signed; negative = loss.
  int open_positions = 0;               // Currently open position count.
  std::int64_t used_margin_paise = 0;   // Margin already committed.

  // ── this order ───────────────────────────────────────────────────────────
  std::int64_t order_value_paise = 0;   // This order's notional (0 = unknown).
  // True when order_value_paise is a real estimate. false for a Market order
  // with no estimate: the checks that NEED the value (account margin + order
  // value) then FAIL-CLOSED, but only where their limit is armed.
  bool order_value_known = true;
  int order_lots = 0;                   // Lots this order adds.
  // This order's estimated slippage (bps). Supplied directly; evaluated
  // independently of order_value_known (NOT fail-closed on an unknown value).
  int slippage_bps = 0;
  // True = opening / adding exposure; false = reducing. max-open-positions
  // applies to ENTRIES only.
  bool is_entry = true;

  // ── strategy ─────────────────────────────────────────────────────────────
  int strategy_lots = 0;                // This strategy's current lots.
  std::int64_t strategy_pnl_paise = 0;  // Signed; negative = loss.
  // false = this strategy is stopped (AC-3): all its orders are blocked while
  // other strategies, with strategy_enabled=true, pass.
  bool strategy_enabled = true;

  // ── instrument ───────────────────────────────────────────────────────────
  int instrument_lots = 0;              // This instrument's current lots.
  // false = the price is illiquid / stale and not tradable -> block.
  bool data_tradable = true;
};

// The four-level risk engine. Stateless: every method is const and reads only
// its arguments, so the same instance is safe to share and trivial to test one
// level at a time (AC-2).
class RiskEngine {
 public:
  // Account-level limits: daily loss, max open positions (entries only) and max
  // account margin (fail-closed when the order value is unknown).
  [[nodiscard]] Result<ports::Ok> check_account(const RiskLimits& limits,
                                                const RiskState& state) const;

  // Strategy-level limits: strategy stopped (AC-3), strategy daily loss and
  // strategy max lots.
  [[nodiscard]] Result<ports::Ok> check_strategy(const RiskLimits& limits,
                                                 const RiskState& state) const;

  // Instrument-level limits: instrument max lots and illiquid / stale data.
  [[nodiscard]] Result<ports::Ok> check_instrument(const RiskLimits& limits,
                                                   const RiskState& state) const;

  // Order-level limits: market-order block, max order value (fail-closed when
  // the order value is unknown) and slippage.
  [[nodiscard]] Result<ports::Ok> check_order(const domain::OrderIntent& intent,
                                              const RiskLimits& limits,
                                              const RiskState& state) const;

  // Composite: runs account -> strategy -> instrument -> order in that fixed
  // order; the FIRST violation is returned, named. This is what the runtime
  // binds as the gate's injected `risk_check` (Story 2.8).
  [[nodiscard]] Result<ports::Ok> check_all(const domain::OrderIntent& intent,
                                            const RiskLimits& limits,
                                            const RiskState& state) const;
};

// Adapter: produce a nullary predicate bound to (intent, limits, state) that
// calls check_all, so it drops directly into GateContext::risk_check
// (Story 2.8). Intent, limits and state are captured BY VALUE (a snapshot taken
// at bind time) and the engine is stateless, so the closure is self-contained
// with no external lifetime dependency and is safe to outlive every argument.
[[nodiscard]] std::function<Result<ports::Ok>()> make_risk_check(const RiskEngine& engine,
                                                                const domain::OrderIntent& intent,
                                                                RiskLimits limits, RiskState state);

}  // namespace broker_exec::risk
