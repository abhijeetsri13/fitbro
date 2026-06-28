#include "broker_exec/isolation/strategy_book.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::isolation {

namespace {

// |x| for a signed quantity, computed without overflow surprises at realistic
// sizes (paise*qty is documented to fit int64 for real order sizes).
[[nodiscard]] std::int64_t abs64(std::int64_t x) noexcept { return x < 0 ? -x : x; }

// A risk-rejection Error, redaction-safe: the message carries only caller ids
// (strategy_id / symbol), which are non-secret. The action is BlockStrategy —
// the runtime halts this strategy's new entries rather than retrying. Mirrors
// the sibling `make_error(...) then set .action` idiom (modes/trading_mode.cpp).
[[nodiscard]] errors::Error rejected(std::string message) {
  errors::Error err = errors::make_error(errors::ErrorCategory::RiskRejected, std::move(message));
  err.action = errors::SuggestedAction::BlockStrategy;
  return err;
}

}  // namespace

void StrategyBook::apply_fill(const std::string& strategy_id, const std::string& symbol,
                              domain::Side side, domain::Quantity qty, domain::Money price) {
  // Signed fill: Buy adds, Sell subtracts. `n` is the current signed net.
  const std::int64_t f = (side == domain::Side::Buy ? qty.value() : -qty.value());
  if (f == 0) {
    return;  // a zero-quantity fill changes nothing — and must NOT default-insert a
             // phantom (strategy,symbol) record that would pollute strategies()/exposure.
  }

  // Touch ONLY this strategy's (symbol) position — the isolation invariant.
  Position& pos = strategies_[strategy_id].positions[symbol];
  const std::int64_t n = pos.net_qty;

  const bool opening = (n == 0) || ((n > 0) == (f > 0));  // flat or same direction
  if (opening) {
    // Size-weighted average cost, integer paise. (|n| + |f|) > 0 here because
    // f != 0, so the divide is always safe.
    const std::int64_t mag_n = abs64(n);
    const std::int64_t mag_f = abs64(f);
    const std::int64_t blended =
        (mag_n * pos.avg_cost.paise() + mag_f * price.paise()) / (mag_n + mag_f);
    pos.avg_cost = domain::Money::from_paise(blended);
    pos.net_qty = n + f;
    return;
  }

  // OPPOSING / REDUCING: realize P&L on the closed quantity. For a long (n > 0)
  // a Sell above avg_cost is a gain; for a short (n < 0) a Buy below avg_cost is
  // a gain — the (n > 0 ? +1 : -1) sign captures both.
  const std::int64_t closed = std::min(abs64(f), abs64(n));
  const std::int64_t sign = (n > 0) ? 1 : -1;
  pos.realized_pnl =
      pos.realized_pnl +
      domain::Money::from_paise((price.paise() - pos.avg_cost.paise()) * closed * sign);
  pos.net_qty = n + f;

  if (pos.net_qty == 0) {
    // Back to flat — avg_cost is meaningless, reset to zero.
    pos.avg_cost = domain::Money::from_paise(0);
  } else if (abs64(f) > abs64(n)) {
    // Cross-zero: the fill exceeded the position; the remainder opens a NEW
    // position on the other side, entered at `price`.
    pos.avg_cost = price;
  }
  // else: a partial reduction — net_qty shrank but the average entry is unchanged.
}

Position StrategyBook::position_of(const std::string& strategy_id,
                                   const std::string& symbol) const {
  const auto strat = strategies_.find(strategy_id);
  if (strat == strategies_.end()) {
    return Position{};  // unknown strategy -> flat/zero (never sees another's book)
  }
  const auto pos = strat->second.positions.find(symbol);
  if (pos == strat->second.positions.end()) {
    return Position{};  // unknown symbol -> flat/zero
  }
  return pos->second;
}

std::vector<std::string> StrategyBook::strategies() const {
  std::vector<std::string> ids;
  ids.reserve(strategies_.size());
  for (const auto& [id, state] : strategies_) {
    ids.push_back(id);
  }
  return ids;
}

void StrategyBook::set_tag(const std::string& strategy_id, const std::string& tag) {
  strategies_[strategy_id].tag = tag;
}

std::string StrategyBook::tag_of(const std::string& strategy_id) const {
  const auto strat = strategies_.find(strategy_id);
  return strat == strategies_.end() ? std::string{} : strat->second.tag;
}

std::vector<FlattenLeg> StrategyBook::square_off(const std::string& strategy_id,
                                                 bool netting_enabled) {
  std::vector<FlattenLeg> legs;

  if (!netting_enabled) {
    // ISOLATION: flatten ONLY this strategy's non-flat positions; zero ONLY this
    // strategy's book. Other strategies are never read or mutated.
    const auto strat = strategies_.find(strategy_id);
    if (strat == strategies_.end()) {
      return legs;  // unknown strategy: nothing to flatten
    }
    for (auto& [symbol, pos] : strat->second.positions) {
      if (pos.net_qty == 0) {
        continue;  // a flat position contributes no leg
      }
      legs.push_back(FlattenLeg{symbol, abs64(pos.net_qty),
                                pos.net_qty > 0 ? domain::Side::Sell : domain::Side::Buy});
      pos.net_qty = 0;
      pos.avg_cost = domain::Money::from_paise(0);
      // realized_pnl is history — kept.
    }
    return legs;
  }

  // NETTING (explicit account-level action): sum net_qty per symbol across ALL
  // strategies, emit one leg per symbol with a non-zero account-net, then zero
  // that symbol in EVERY strategy. std::map keeps the symbol order deterministic.
  std::map<std::string, std::int64_t> account_net;
  for (const auto& [id, state] : strategies_) {
    for (const auto& [symbol, pos] : state.positions) {
      account_net[symbol] += pos.net_qty;
    }
  }

  for (const auto& [symbol, net] : account_net) {
    if (net == 0) {
      continue;  // a flat account-net emits no leg
    }
    legs.push_back(
        FlattenLeg{symbol, abs64(net), net > 0 ? domain::Side::Sell : domain::Side::Buy});
  }

  // Zero every strategy's position in any netted symbol (it has been collapsed
  // at the account level).
  for (auto& [id, state] : strategies_) {
    for (auto& [symbol, pos] : state.positions) {
      if (account_net.find(symbol) != account_net.end() && pos.net_qty != 0) {
        pos.net_qty = 0;
        pos.avg_cost = domain::Money::from_paise(0);
      }
    }
  }
  return legs;
}

domain::Money StrategyBook::account_exposure(
    const std::function<domain::Money(const std::string& symbol)>& mark_price) const {
  std::int64_t total = 0;
  for (const auto& [id, state] : strategies_) {
    for (const auto& [symbol, pos] : state.positions) {
      if (pos.net_qty == 0) {
        continue;
      }
      total += abs64(pos.net_qty) * mark_price(symbol).paise();
    }
  }
  return domain::Money::from_paise(total);
}

Result<ports::Ok> StrategyBook::check_new_order(
    const std::string& strategy_id, const std::string& symbol, domain::Quantity qty,
    domain::Money price, domain::Money account_max_exposure,
    const std::function<domain::Money(const std::string&)>& mark_price) const {
  // 1. Stopped strategy: refuse its new entries (AC-3). Redaction-safe message.
  if (!is_active(strategy_id)) {
    return fail(rejected("strategy '" + strategy_id + "' is stopped: new orders blocked"));
  }

  // 2. A NEGATIVE cap is a misconfiguration — fail closed, never treat it as
  //    "no limit". (symbol named for the operator's context.)
  if (account_max_exposure.paise() < 0) {
    return fail(rejected("negative account exposure cap is a misconfiguration for '" + symbol +
                         "': blocked (fail-closed)"));
  }

  // 3. A cap of exactly 0 means "no limit" — allow.
  if (account_max_exposure.paise() == 0) {
    return ports::ok();
  }

  // 4. GLOBAL BACKSTOP (AC-2): the COMBINED account exposure plus this new
  //    order's notional must not EXCEED the cap, even when this strategy is
  //    individually small. Strict integer `>` (exactly-at-cap fits).
  const std::int64_t combined =
      account_exposure(mark_price).paise() + abs64(qty.value()) * price.paise();
  if (combined > account_max_exposure.paise()) {
    return fail(rejected("combined account exposure would exceed cap for strategy '" +
                         strategy_id + "' on '" + symbol + "'"));
  }

  // 5. Within the cap.
  return ports::ok();
}

void StrategyBook::stop_strategy(const std::string& strategy_id) {
  strategies_[strategy_id].active = false;  // creates the record if needed
}

void StrategyBook::resume_strategy(const std::string& strategy_id) {
  strategies_[strategy_id].active = true;
}

bool StrategyBook::is_active(const std::string& strategy_id) const {
  const auto strat = strategies_.find(strategy_id);
  // An unknown strategy is active by default (it has placed nothing).
  return strat == strategies_.end() ? true : strat->second.active;
}

}  // namespace broker_exec::isolation
