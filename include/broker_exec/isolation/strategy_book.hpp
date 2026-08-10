#pragma once

// broker_exec::isolation — per-strategy virtual position book (Story 6.4,
// FR-32). The in-memory isolation the runtime maintains ALONGSIDE the broker's
// single real position, so strategies sharing one account never interfere.
//
// THE WHOLE POINT IS "STRATEGIES MUST NOT INTERFERE": each strategy gets a
// SEPARATE virtual book keyed by `strategy_id` — its own per-symbol signed net
// quantity, average cost, realized P&L, free-form tag, and active flag. A fill
// or a square-off touches ONLY the owning strategy. The one exception is an
// explicit account-level NETTING square-off, which is the operator deliberately
// collapsing the books across strategies.
//
// THE LOAD-BEARING INVARIANTS:
//   * ISOLATION (AC-1): `apply_fill` / `square_off(.., netting=false)` for
//     strategy A NEVER read or mutate strategy B's positions, P&L, or tag. Two
//     strategies long the same instrument keep independent positions and P&L;
//     squaring off A leaves B byte-identical.
//   * GLOBAL LIMIT IS THE BACKSTOP (AC-2): per-strategy sizing is not enough —
//     `check_new_order` weighs the COMBINED account exposure (summed across ALL
//     strategies) against the account cap, so two individually-small strategies
//     cannot jointly breach it. A NEGATIVE cap is a misconfig and FAILS CLOSED;
//     a cap of exactly 0 means "no limit".
//   * INDEPENDENT LIFECYCLE (AC-3): stopping strategy A blocks ONLY A's new
//     entries; B stays active and tradable. An unknown strategy is active by
//     default (it has placed nothing).
//
// Conventions: no-throw across the boundary (return a Result / an empty vector,
// never propagate; never call `.value()` on an errored Result), NO double/float
// (integer domain::Money paise + integer quantities throughout — the average-cost
// model is all integer paise), redaction-safe error messages (only the caller's
// own `strategy_id` / `symbol`, which are non-secret ids — never a token or raw
// broker text). Cross-platform: C++20 standard library only — NO OS APIs, NO
// `#ifdef`, no clock, no I/O, no threads. Mark prices arrive through a
// `std::function` seam so there is no market-data dependency here. Depends inward
// on `domain` (Money/Quantity/Side), `errors` (Result/Error), and `ports` (Ok).

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::isolation {

// One strategy's virtual position in one symbol. All integers, no float:
//   net_qty     — signed lots/units: +long, -short, 0 flat.
//   avg_cost    — per-unit average entry cost in paise (domain::Money). Zero
//                 when the position is flat (net_qty == 0).
//   realized_pnl — cumulative realized P&L in paise, banked as opposing fills
//                 close the position. Survives a square-off (it is history).
struct Position {
  std::int64_t net_qty = 0;
  domain::Money avg_cost;
  domain::Money realized_pnl;
};

// The offsetting order that would flatten a position: `side` is the OPPOSING
// side (a net-long is flattened by a Sell, a net-short by a Buy) and `qty` is
// the magnitude `|net_qty|`. A flat position contributes no leg.
struct FlattenLeg {
  std::string symbol;
  std::int64_t qty = 0;
  domain::Side side = domain::Side::Sell;
};

// The per-account multi-strategy virtual book. Default-constructs empty. All
// state is in-memory; there is no I/O, clock, or thread here. Keyed by
// `strategy_id`; each strategy owns an isolated set of per-symbol Positions plus
// a free-form tag and an active flag.
class StrategyBook {
 public:
  StrategyBook() = default;

  // Apply a fill to ONLY the named strategy's (symbol) position (the isolation
  // invariant — another strategy's book is never touched). Average-cost model,
  // all integer paise. With signed fill `f = (side==Buy ? +qty : -qty)` and the
  // current net `n`:
  //   * OPENING / SAME DIRECTION (n == 0, or sign(f) == sign(n)): the new
  //     average cost is the size-weighted blend
  //       (|n|*avg_cost + |f|*price) / (|n| + |f|)   (integer divide);
  //     net_qty becomes n + f.
  //   * OPPOSING / REDUCING (sign(f) != sign(n)): realize P&L on the closed
  //     quantity `c = min(|f|, |n|)`:
  //       realized_pnl += (price - avg_cost) * c * (n > 0 ? +1 : -1);
  //     net_qty becomes n + f. If the fill EXCEEDS the position (|f| > |n|, a
  //     cross-zero), the remainder opens a NEW position at `price`
  //     (avg_cost = price). If net_qty returns to 0, avg_cost resets to 0.
  void apply_fill(const std::string& strategy_id, const std::string& symbol,
                  domain::Side side, domain::Quantity qty, domain::Money price);

  // The stored Position for (strategy_id, symbol), or a flat/zero Position if the
  // strategy or symbol is unknown — a strategy never sees another's book.
  [[nodiscard]] Position position_of(const std::string& strategy_id,
                                     const std::string& symbol) const;

  // The known strategy ids (those that have a record), in deterministic order.
  [[nodiscard]] std::vector<std::string> strategies() const;

  // Set / read a strategy's free-form tag (an operator label; non-secret).
  // Setting a tag creates the strategy's record if it did not exist. `tag_of`
  // returns an empty string for an unknown strategy.
  void set_tag(const std::string& strategy_id, const std::string& tag);
  [[nodiscard]] std::string tag_of(const std::string& strategy_id) const;

  // Compute the offsetting legs that would flatten positions, and zero them.
  //   * netting_enabled == false (default isolation): emit one leg per non-flat
  //     position of ONLY `strategy_id`, then zero ONLY that strategy's positions
  //     (net_qty = 0, avg_cost = 0; realized_pnl is kept). Other strategies are
  //     UNTOUCHED — A never squares off B (AC-1).
  //   * netting_enabled == true (explicit account-level action): for each symbol
  //     sum net_qty across ALL strategies; emit one leg per symbol whose
  //     account-net is non-zero (qty = |sum|, side opposing the sum), then zero
  //     that symbol's Position in EVERY strategy. A flat account-net emits no leg.
  [[nodiscard]] std::vector<FlattenLeg> square_off(const std::string& strategy_id,
                                                   bool netting_enabled);

  // The COMBINED account exposure: sum over every (strategy, symbol) of
  // |net_qty| * mark_price(symbol).paise(), returned as Money paise. The
  // mark-price seam keeps this I/O-free (no market-data dependency).
  [[nodiscard]] domain::Money account_exposure(
      const std::function<domain::Money(const std::string& symbol)>& mark_price) const;

  // Pre-trade gate, fail-closed. In order:
  //   1. If the strategy is STOPPED -> RiskRejected/BlockStrategy (the message
  //      names the strategy; redaction-safe). (AC-3)
  //   2. Else if `account_max_exposure` is NEGATIVE -> RiskRejected (a negative
  //      cap is a misconfig; fail closed).
  //   3. Else if `account_max_exposure` is exactly 0 -> NO LIMIT -> ok().
  //   4. Else if `account_exposure(mark) + |qty|*price` would EXCEED the cap ->
  //      RiskRejected (the GLOBAL limit blocks the COMBINED exposure even when
  //      this strategy is individually small). (AC-2)
  //   5. Else ok().
  [[nodiscard]] Result<ports::Ok> check_new_order(
      const std::string& strategy_id, const std::string& symbol, domain::Quantity qty,
      domain::Money price, domain::Money account_max_exposure,
      const std::function<domain::Money(const std::string&)>& mark_price) const;

  // Independent lifecycle (AC-3). `stop_strategy` blocks the strategy's new
  // entries (creating its record if needed); `resume_strategy` re-enables it.
  // `is_active` is true for an unknown strategy (it has placed nothing).
  // Stopping A never changes B's active flag.
  void stop_strategy(const std::string& strategy_id);
  void resume_strategy(const std::string& strategy_id);
  [[nodiscard]] bool is_active(const std::string& strategy_id) const;

 private:
  // One strategy's isolated virtual book: its per-symbol positions, tag, and
  // active flag. std::map keeps iteration (and so square_off / strategies())
  // deterministic.
  struct StrategyState {
    std::map<std::string, Position> positions;
    std::string tag;
    bool active = true;
  };

  std::map<std::string, StrategyState> strategies_;
};

}  // namespace broker_exec::isolation
