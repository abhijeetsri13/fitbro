#include "broker_exec/isolation/strategy_book.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/result.hpp"

using broker_exec::isolation::FlattenLeg;
using broker_exec::isolation::Position;
using broker_exec::isolation::StrategyBook;

namespace domain = broker_exec::domain;
namespace errors = broker_exec::errors;

namespace {

using domain::Money;
using domain::Quantity;
using domain::Side;

// A flat mark-price seam: every symbol marks at `rupees` per unit. Keeps the
// exposure tests I/O-free (no market-data dependency).
[[nodiscard]] auto flat_mark(std::int64_t rupees) {
  return [rupees](const std::string&) { return Money::from_rupees(rupees); };
}

constexpr const char* kSym = "NIFTY..CE";

}  // namespace

// ── AC-1: isolation — two strategies, independent positions ─────────────────
TEST_CASE("two strategies long the same symbol keep independent positions",
          "[isolation][ac1]") {
  StrategyBook book;
  book.apply_fill("A", kSym, Side::Buy, Quantity::of(50), Money::from_rupees(100));
  book.apply_fill("B", kSym, Side::Buy, Quantity::of(50), Money::from_rupees(100));

  CHECK(book.position_of("A", kSym).net_qty == 50);
  CHECK(book.position_of("B", kSym).net_qty == 50);

  // Snapshot B, then trade A again: B must be EXACTLY unchanged (the isolation
  // invariant — a fill on A never touches B's book).
  const Position b_before = book.position_of("B", kSym);
  book.apply_fill("A", kSym, Side::Buy, Quantity::of(25), Money::from_rupees(120));

  CHECK(book.position_of("A", kSym).net_qty == 75);
  const Position b_after = book.position_of("B", kSym);
  CHECK(b_after.net_qty == b_before.net_qty);
  CHECK(b_after.avg_cost == b_before.avg_cost);
  CHECK(b_after.realized_pnl == b_before.realized_pnl);
}

TEST_CASE("position_of for an unknown strategy or symbol is flat/zero", "[isolation][ac1]") {
  StrategyBook book;
  const Position p = book.position_of("ghost", kSym);
  CHECK(p.net_qty == 0);
  CHECK(p.avg_cost == Money::from_paise(0));
  CHECK(p.realized_pnl == Money::from_paise(0));

  book.apply_fill("A", kSym, Side::Buy, Quantity::of(10), Money::from_rupees(100));
  CHECK(book.position_of("A", "OTHER").net_qty == 0);  // known strategy, unknown symbol
}

// ── AC-1: per-strategy realized P&L ─────────────────────────────────────────
TEST_CASE("realized P&L is per strategy", "[isolation][ac1][pnl]") {
  StrategyBook book;
  // A round-trips 50 lots: buy @100, sell @120 (per-unit rupees).
  book.apply_fill("A", kSym, Side::Buy, Quantity::of(50), Money::from_rupees(100));
  book.apply_fill("A", kSym, Side::Sell, Quantity::of(50), Money::from_rupees(120));
  // B only buys.
  book.apply_fill("B", kSym, Side::Buy, Quantity::of(50), Money::from_rupees(100));

  // P&L = (12000 - 10000 paise) * 50 = 100000 paise = 1000 rupees.
  CHECK(book.position_of("A", kSym).realized_pnl == Money::from_rupees(1000));
  CHECK(book.position_of("A", kSym).realized_pnl == Money::from_paise(100000));
  CHECK(book.position_of("A", kSym).net_qty == 0);
  CHECK(book.position_of("A", kSym).avg_cost == Money::from_paise(0));  // flat resets avg

  // B never closed anything: zero realized P&L.
  CHECK(book.position_of("B", kSym).realized_pnl == Money::from_paise(0));
  CHECK(book.position_of("B", kSym).net_qty == 50);
}

// ── AC-1: square-off respects isolation; netting is account-level ───────────
TEST_CASE("square_off without netting flattens only that strategy", "[isolation][ac1][squareoff]") {
  StrategyBook book;
  book.apply_fill("A", kSym, Side::Buy, Quantity::of(50), Money::from_rupees(100));
  book.apply_fill("B", kSym, Side::Buy, Quantity::of(50), Money::from_rupees(100));

  const auto legs = book.square_off("A", /*netting_enabled=*/false);
  REQUIRE(legs.size() == 1);
  CHECK(legs[0].symbol == kSym);
  CHECK(legs[0].qty == 50);
  CHECK(legs[0].side == Side::Sell);  // offsetting a net-long

  // A is zeroed; B is UNTOUCHED.
  CHECK(book.position_of("A", kSym).net_qty == 0);
  CHECK(book.position_of("B", kSym).net_qty == 50);
}

TEST_CASE("square_off with netting nets across all strategies", "[isolation][ac1][squareoff]") {
  StrategyBook book;
  // A long 50, B short 20 of the same symbol -> account-net +30.
  book.apply_fill("A", kSym, Side::Buy, Quantity::of(50), Money::from_rupees(100));
  book.apply_fill("B", kSym, Side::Sell, Quantity::of(20), Money::from_rupees(100));

  const auto legs = book.square_off("A", /*netting_enabled=*/true);
  REQUIRE(legs.size() == 1);
  CHECK(legs[0].symbol == kSym);
  CHECK(legs[0].qty == 30);          // |+50 - 20|
  CHECK(legs[0].side == Side::Sell);  // net long -> sell to flatten

  // Both strategies' positions in that symbol are zeroed.
  CHECK(book.position_of("A", kSym).net_qty == 0);
  CHECK(book.position_of("B", kSym).net_qty == 0);
}

TEST_CASE("square_off with netting emits no leg for a flat account-net", "[isolation][squareoff]") {
  StrategyBook book;
  book.apply_fill("A", kSym, Side::Buy, Quantity::of(40), Money::from_rupees(100));
  book.apply_fill("B", kSym, Side::Sell, Quantity::of(40), Money::from_rupees(100));

  const auto legs = book.square_off("A", /*netting_enabled=*/true);
  CHECK(legs.empty());  // account-net is flat -> nothing to flatten
}

// ── AC-2: global account limit backstops the COMBINED exposure ──────────────
TEST_CASE("global limit blocks the combined exposure of small strategies", "[isolation][ac2]") {
  StrategyBook book;
  // A and B are each long 50 @ mark 100 rupees -> 5000 rupees notional each;
  // combined account exposure = 10000 rupees.
  book.apply_fill("A", kSym, Side::Buy, Quantity::of(50), Money::from_rupees(100));
  book.apply_fill("B", kSym, Side::Buy, Quantity::of(50), Money::from_rupees(100));

  const auto mark = flat_mark(100);
  CHECK(book.account_exposure(mark) == Money::from_rupees(10000));

  // A SMALL new order from strategy C (10 lots @ 100 = 1000 rupees) pushes the
  // combined exposure to 11000 — over a 10500-rupee cap. Rejected even though C
  // is individually tiny.
  const auto over = book.check_new_order("C", kSym, Quantity::of(10), Money::from_rupees(100),
                                         Money::from_rupees(10500), mark);
  REQUIRE_FALSE(over.has_value());
  CHECK(over.error().category == errors::ErrorCategory::RiskRejected);
  CHECK(over.error().action == errors::SuggestedAction::BlockStrategy);

  // The same order under a generous cap is allowed.
  const auto within = book.check_new_order("C", kSym, Quantity::of(10), Money::from_rupees(100),
                                           Money::from_rupees(20000), mark);
  CHECK(within.has_value());
}

TEST_CASE("a negative cap fails closed; a zero cap means no limit", "[isolation][ac2]") {
  StrategyBook book;
  book.apply_fill("A", kSym, Side::Buy, Quantity::of(1000), Money::from_rupees(100));
  const auto mark = flat_mark(100);

  // Negative cap = misconfiguration -> reject (fail-closed).
  const auto neg = book.check_new_order("A", kSym, Quantity::of(1), Money::from_rupees(100),
                                        Money::from_rupees(-1), mark);
  REQUIRE_FALSE(neg.has_value());
  CHECK(neg.error().category == errors::ErrorCategory::RiskRejected);

  // Zero cap = NO LIMIT -> allow, even with a large standing exposure.
  const auto zero = book.check_new_order("A", kSym, Quantity::of(1), Money::from_rupees(100),
                                         Money::from_paise(0), mark);
  CHECK(zero.has_value());
}

TEST_CASE("exposure exactly at the cap is allowed (strict >)", "[isolation][ac2]") {
  StrategyBook book;
  book.apply_fill("A", kSym, Side::Buy, Quantity::of(50), Money::from_rupees(100));
  const auto mark = flat_mark(100);
  // Standing exposure 5000 rupees + a 1000-rupee order = 6000; cap exactly 6000.
  const auto at_cap = book.check_new_order("A", kSym, Quantity::of(10), Money::from_rupees(100),
                                           Money::from_rupees(6000), mark);
  CHECK(at_cap.has_value());
}

// ── AC-3: independent stop / resume ─────────────────────────────────────────
TEST_CASE("stopping one strategy does not stop another", "[isolation][ac3]") {
  StrategyBook book;
  const auto mark = flat_mark(100);

  book.stop_strategy("A");
  CHECK_FALSE(book.is_active("A"));
  CHECK(book.is_active("B"));  // untouched; unknown strategy is active by default

  // A's new orders are rejected while stopped.
  const auto a = book.check_new_order("A", kSym, Quantity::of(1), Money::from_rupees(100),
                                      Money::from_rupees(100000), mark);
  REQUIRE_FALSE(a.has_value());
  CHECK(a.error().category == errors::ErrorCategory::RiskRejected);
  CHECK(a.error().action == errors::SuggestedAction::BlockStrategy);

  // B still trades.
  const auto b = book.check_new_order("B", kSym, Quantity::of(1), Money::from_rupees(100),
                                      Money::from_rupees(100000), mark);
  CHECK(b.has_value());

  // Resume re-enables A.
  book.resume_strategy("A");
  CHECK(book.is_active("A"));
  const auto a2 = book.check_new_order("A", kSym, Quantity::of(1), Money::from_rupees(100),
                                       Money::from_rupees(100000), mark);
  CHECK(a2.has_value());
}

// ── average-cost / cross-zero ───────────────────────────────────────────────
TEST_CASE("cross-zero fill realizes P&L on the closed lots and reopens at price",
          "[isolation][avgcost]") {
  StrategyBook book;
  book.apply_fill("A", kSym, Side::Buy, Quantity::of(100), Money::from_rupees(100));
  book.apply_fill("A", kSym, Side::Sell, Quantity::of(150), Money::from_rupees(110));

  const Position p = book.position_of("A", kSym);
  CHECK(p.net_qty == -50);                              // 100 long, sold 150 -> 50 short
  CHECK(p.avg_cost == Money::from_rupees(110));         // remainder reopened at the fill price
  // Realized only on the 100 closed: (11000 - 10000 paise) * 100 = 1000 rupees.
  CHECK(p.realized_pnl == Money::from_rupees(1000));

  // Buying back the 50 short flattens it and resets avg_cost to zero.
  book.apply_fill("A", kSym, Side::Buy, Quantity::of(50), Money::from_rupees(110));
  const Position flat = book.position_of("A", kSym);
  CHECK(flat.net_qty == 0);
  CHECK(flat.avg_cost == Money::from_paise(0));
}

TEST_CASE("closing a SHORT below cost realizes a POSITIVE P&L (sign correctness)",
          "[isolation][avgcost]") {
  StrategyBook book;
  // Open a short 100 @ 120, then buy it back 100 @ 100 (covered cheaper -> profit).
  book.apply_fill("A", kSym, Side::Sell, Quantity::of(100), Money::from_rupees(120));
  book.apply_fill("A", kSym, Side::Buy, Quantity::of(100), Money::from_rupees(100));

  const Position p = book.position_of("A", kSym);
  CHECK(p.net_qty == 0);
  CHECK(p.avg_cost == Money::from_paise(0));
  // Short-close realized = (price - avg_cost) * c * sign(n<0) = (10000-12000)*100*(-1)
  //                      = +200000 paise = +2000 rupees. The negative leg sign MATTERS.
  CHECK(p.realized_pnl == Money::from_rupees(2000));
}

TEST_CASE("same-direction fills blend the average cost", "[isolation][avgcost]") {
  StrategyBook book;
  // Buy 50 @ 100, then 50 @ 120 -> avg 110.
  book.apply_fill("A", kSym, Side::Buy, Quantity::of(50), Money::from_rupees(100));
  book.apply_fill("A", kSym, Side::Buy, Quantity::of(50), Money::from_rupees(120));
  const Position p = book.position_of("A", kSym);
  CHECK(p.net_qty == 100);
  CHECK(p.avg_cost == Money::from_rupees(110));
}

// ── tags + strategies() ─────────────────────────────────────────────────────
TEST_CASE("tags are per strategy and strategies() lists known ids", "[isolation]") {
  StrategyBook book;
  book.set_tag("A", "momentum");
  book.set_tag("B", "meanrev");
  CHECK(book.tag_of("A") == "momentum");
  CHECK(book.tag_of("B") == "meanrev");
  CHECK(book.tag_of("ghost").empty());

  const auto ids = book.strategies();
  REQUIRE(ids.size() == 2);
  CHECK(ids[0] == "A");  // std::map -> deterministic order
  CHECK(ids[1] == "B");
}
