#include "broker_exec/priceband/band_check.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"

using broker_exec::priceband::BandCheckResult;
using broker_exec::priceband::BandVerdict;
using broker_exec::priceband::check_price_band;
using broker_exec::priceband::PriceBand;
using broker_exec::priceband::require_band_ok;
using broker_exec::priceband::to_string;

using broker_exec::domain::Money;
using broker_exec::domain::OrderType;
using broker_exec::domain::Side;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::SuggestedAction;

namespace {

// A well-formed band [90.00, 110.00] in rupees, used across the happy-path tests.
[[nodiscard]] PriceBand band_90_110() {
  PriceBand b;
  b.lower = Money::from_rupees(90);
  b.upper = Money::from_rupees(110);
  b.known = true;
  return b;
}

constexpr Money kZero;  // a placeholder price for fields a branch ignores.

}  // namespace

TEST_CASE("within band: a limit buy inside [90,110] is WithinBand, not blocked", "[priceband]") {
  const BandCheckResult r = check_price_band(Side::Buy, OrderType::Limit, Money::from_rupees(100),
                                             kZero, band_90_110(), /*is_exit=*/false);
  CHECK(r.verdict == BandVerdict::WithinBand);
  CHECK_FALSE(r.blocked);
  CHECK_FALSE(r.has_suggestion);
  CHECK(require_band_ok(Side::Buy, OrderType::Limit, Money::from_rupees(100), kZero, band_90_110(),
                        false)
            .has_value());
}

TEST_CASE("entry out of band is BLOCKED with a clamped suggestion", "[priceband]") {
  // A limit buy at 120 (> upper 110), entry -> OutsideBandBlocked, blocked.
  const BandCheckResult r = check_price_band(Side::Buy, OrderType::Limit, Money::from_rupees(120),
                                             kZero, band_90_110(), /*is_exit=*/false);
  CHECK(r.verdict == BandVerdict::OutsideBandBlocked);
  CHECK(r.blocked);
  REQUIRE(r.has_suggestion);
  CHECK(r.suggested_limit == Money::from_rupees(110));  // clamped to the ceiling

  // require_band_ok surfaces it as a typed Validation / BlockStrategy Error.
  const auto gated = require_band_ok(Side::Buy, OrderType::Limit, Money::from_rupees(120), kZero,
                                     band_90_110(), false);
  REQUIRE_FALSE(gated.has_value());
  CHECK(gated.error().category == ErrorCategory::Validation);
  CHECK(gated.error().action == SuggestedAction::BlockStrategy);
}

TEST_CASE("exit out of band is CLAMPED, never blocked", "[priceband]") {
  // A limit sell at 80 (< lower 90), exit -> OutsideBandExitClamped, NOT blocked.
  const BandCheckResult r = check_price_band(Side::Sell, OrderType::Limit, Money::from_rupees(80),
                                             kZero, band_90_110(), /*is_exit=*/true);
  CHECK(r.verdict == BandVerdict::OutsideBandExitClamped);
  CHECK_FALSE(r.blocked);
  REQUIRE(r.has_suggestion);
  CHECK(r.suggested_limit == Money::from_rupees(90));  // clamped up to the floor

  // A clamp is NOT blocked, so the gate returns ok(); the caller applies the clamp.
  CHECK(require_band_ok(Side::Sell, OrderType::Limit, Money::from_rupees(80), kZero, band_90_110(),
                        true)
            .has_value());
}

TEST_CASE("band edges are inclusive: a price exactly on an edge is WithinBand", "[priceband]") {
  const BandCheckResult lo = check_price_band(Side::Buy, OrderType::Limit, Money::from_rupees(90),
                                              kZero, band_90_110(), false);
  CHECK(lo.verdict == BandVerdict::WithinBand);
  CHECK_FALSE(lo.blocked);

  const BandCheckResult hi = check_price_band(Side::Sell, OrderType::Limit, Money::from_rupees(110),
                                              kZero, band_90_110(), false);
  CHECK(hi.verdict == BandVerdict::WithinBand);
  CHECK_FALSE(hi.blocked);
}

TEST_CASE("stop-limit checks BOTH the trigger and the limit", "[priceband]") {
  // Limit in band (100) but the TRIGGER is out of band (120) -> out-of-band path.
  const BandCheckResult entry =
      check_price_band(Side::Buy, OrderType::StopLoss, Money::from_rupees(100),
                       /*trigger=*/Money::from_rupees(120), band_90_110(), /*is_exit=*/false);
  CHECK(entry.verdict == BandVerdict::OutsideBandBlocked);
  CHECK(entry.blocked);

  // The same on an exit clamps (never blocks); both prices in band would be Within.
  // BOTH the limit AND the out-of-band trigger must be clamped into the band — a
  // half-clamped stop-limit would still be exchange-rejected on the trigger.
  const BandCheckResult exit =
      check_price_band(Side::Sell, OrderType::StopLoss, Money::from_rupees(100),
                       /*trigger=*/Money::from_rupees(70), band_90_110(), /*is_exit=*/true);
  CHECK(exit.verdict == BandVerdict::OutsideBandExitClamped);
  CHECK_FALSE(exit.blocked);
  CHECK(exit.has_suggestion);
  CHECK(exit.suggested_limit == Money::from_rupees(100));  // already in band
  CHECK(exit.has_trigger_suggestion);
  CHECK(exit.suggested_trigger == Money::from_rupees(90));  // 70 clamped UP to band.lower

  const BandCheckResult both_in =
      check_price_band(Side::Buy, OrderType::StopLoss, Money::from_rupees(100),
                       /*trigger=*/Money::from_rupees(95), band_90_110(), false);
  CHECK(both_in.verdict == BandVerdict::WithinBand);
}

TEST_CASE("a PLAIN market order is MarketUnchecked, never blocked", "[priceband]") {
  // Only a plain Market has genuinely no price of ours for the exchange to
  // reject. (SL-M used to be lumped in here; see the IMP-11 case below for why
  // that was a hole rather than a simplification.)
  const BandCheckResult r =
      check_price_band(Side::Buy, OrderType::Market, Money::from_rupees(999),
                       Money::from_rupees(999), band_90_110(), /*is_exit=*/false);
  CHECK(r.verdict == BandVerdict::MarketUnchecked);
  CHECK_FALSE(r.blocked);
  CHECK_FALSE(r.has_suggestion);
  CHECK_FALSE(r.has_trigger_suggestion);
}

TEST_CASE("IMP-11: an SL-M's TRIGGER is band-checked even though it fires at market",
          "[priceband][IMP-11]") {
  // THE HOLE THIS CLOSES. An SL-M was treated as "market-style, nothing to
  // check" — but the exchange rejects a stop whose TRIGGER is outside the
  // circuit/LPP band regardless of how the order fires. So the one order type
  // whose entire job is to survive a violent move was the one type never
  // validated, and a protective SL-M armed just outside a fast-moving band was
  // silently not protection at all.

  SECTION("an in-band trigger passes") {
    const BandCheckResult r =
        check_price_band(Side::Sell, OrderType::StopLossMarket, /*limit=*/Money::from_rupees(999),
                         /*trigger=*/Money::from_rupees(100), band_90_110(), /*is_exit=*/false);
    // NOTE the deliberately absurd limit: an SL-M has none, so it must be
    // IGNORED. A check that validated it would block this well-formed order.
    CHECK(r.verdict == BandVerdict::WithinBand);
    CHECK_FALSE(r.blocked);
  }

  SECTION("an out-of-band trigger BLOCKS an entry and suggests only the TRIGGER") {
    const BandCheckResult r =
        check_price_band(Side::Sell, OrderType::StopLossMarket, kZero,
                         /*trigger=*/Money::from_rupees(120), band_90_110(), /*is_exit=*/false);
    CHECK(r.verdict == BandVerdict::OutsideBandBlocked);
    CHECK(r.blocked);
    REQUIRE(r.has_trigger_suggestion);
    CHECK(r.suggested_trigger == Money::from_rupees(110));  // clamped to the ceiling
    // No limit suggestion: an SL-M has no limit field for the caller to apply, so
    // offering a number for it would invite writing to a field no broker reads.
    CHECK_FALSE(r.has_suggestion);
  }

  SECTION("an out-of-band trigger on an EXIT is CLAMPED, never blocked") {
    const BandCheckResult r =
        check_price_band(Side::Sell, OrderType::StopLossMarket, kZero,
                         /*trigger=*/Money::from_rupees(70), band_90_110(), /*is_exit=*/true);
    CHECK(r.verdict == BandVerdict::OutsideBandExitClamped);
    CHECK_FALSE(r.blocked);  // the module's spine: an exit is NEVER blocked
    REQUIRE(r.has_trigger_suggestion);
    CHECK(r.suggested_trigger == Money::from_rupees(90));  // 70 clamped UP to the floor
  }

  SECTION("require_band_ok agrees: entry refused, exit allowed") {
    CHECK_FALSE(require_band_ok(Side::Sell, OrderType::StopLossMarket, kZero,
                                Money::from_rupees(120), band_90_110(), /*is_exit=*/false)
                    .has_value());
    CHECK(require_band_ok(Side::Sell, OrderType::StopLossMarket, kZero, Money::from_rupees(120),
                          band_90_110(), /*is_exit=*/true)
              .has_value());
  }
}

TEST_CASE("an unavailable band is BandUnknown and does NOT freeze trading", "[priceband]") {
  PriceBand unknown;  // known defaults to false
  const BandCheckResult r = check_price_band(Side::Buy, OrderType::Limit, Money::from_rupees(120),
                                             kZero, unknown, /*is_exit=*/false);
  CHECK(r.verdict == BandVerdict::BandUnknown);
  CHECK_FALSE(r.blocked);  // not frozen — the order is let through unvalidated
  CHECK_FALSE(r.has_suggestion);
  // Even an out-of-band-looking entry is NOT blocked when the band is unknown.
  CHECK(require_band_ok(Side::Buy, OrderType::Limit, Money::from_rupees(120), kZero, unknown, false)
            .has_value());
}

TEST_CASE("an inverted band (lower>upper, known) is treated as BandUnknown", "[priceband]") {
  PriceBand inverted;
  inverted.lower = Money::from_rupees(110);
  inverted.upper = Money::from_rupees(90);
  inverted.known = true;
  const BandCheckResult r = check_price_band(Side::Buy, OrderType::Limit, Money::from_rupees(100),
                                             kZero, inverted, /*is_exit=*/false);
  // A malformed band cannot validate or produce a bogus clamp.
  CHECK(r.verdict == BandVerdict::BandUnknown);
  CHECK_FALSE(r.blocked);
  CHECK_FALSE(r.has_suggestion);
}

TEST_CASE("CORE INVARIANT: no out-of-band ENTRY is ever allowed through unblocked", "[priceband]") {
  const PriceBand band = band_90_110();
  // Sweep both sides, both limit/stop-limit order types, and prices on both sides
  // of the band. For every ENTRY that is out of band, blocked MUST be true; for
  // every EXIT, blocked MUST be false.
  const Side sides[] = {Side::Buy, Side::Sell};
  const OrderType limit_types[] = {OrderType::Limit, OrderType::StopLoss};
  const Money prices[] = {Money::from_rupees(50),  Money::from_rupees(89),  Money::from_rupees(90),
                          Money::from_rupees(100), Money::from_rupees(110), Money::from_rupees(111),
                          Money::from_rupees(200)};

  for (const Side side : sides) {
    for (const OrderType t : limit_types) {
      for (const Money price : prices) {
        // trigger == price so a stop-limit exercises both checked prices together.
        const BandCheckResult entry = check_price_band(side, t, price, price, band, false);
        if (entry.verdict == BandVerdict::OutsideBandBlocked) {
          CHECK(entry.blocked);  // an out-of-band entry is ALWAYS blocked
        }
        // The contrapositive: an entry is unblocked ONLY when it is in band.
        if (!entry.blocked) {
          CHECK(entry.verdict == BandVerdict::WithinBand);
        }

        const BandCheckResult exit = check_price_band(side, t, price, price, band, true);
        CHECK_FALSE(exit.blocked);  // an EXIT is NEVER blocked
      }
    }
  }
}

TEST_CASE("to_string names are the stable observability contract", "[priceband]") {
  CHECK(to_string(BandVerdict::WithinBand) == "within_band");
  CHECK(to_string(BandVerdict::OutsideBandBlocked) == "outside_band_blocked");
  CHECK(to_string(BandVerdict::OutsideBandExitClamped) == "outside_band_exit_clamped");
  CHECK(to_string(BandVerdict::MarketUnchecked) == "market_unchecked");
  CHECK(to_string(BandVerdict::BandUnknown) == "band_unknown");
}

// ── IMP-11 AC-3: the intent-driven overloads read the REAL trigger ───────────
//
// Before OrderIntent carried a distinct trigger, a caller had to synthesize one —
// in practice by passing `price` twice — so the band check validated a number the
// broker would never see. These overloads take the intent, so the pair under test
// is the pair that ships.

namespace {

[[nodiscard]] broker_exec::domain::OrderIntent stop_limit_intent(std::int64_t limit_rupees,
                                                                 std::int64_t trigger_rupees) {
  broker_exec::domain::OrderIntent intent;
  intent.client_ref = "alpha-1a2b3c4d-uuid";
  intent.symbol = "NIFTY26JUL24000CE";
  // A SELL stop-limit, so limit <= trigger is the well-formed shape (matching the
  // gate's side-ordering rule) for every case below.
  intent.side = Side::Sell;
  intent.quantity = broker_exec::domain::Quantity::of(50);
  intent.price = broker_exec::domain::Price::from_rupees(limit_rupees);
  intent.trigger_price = broker_exec::domain::Price::from_rupees(trigger_rupees);
  intent.order_type = OrderType::StopLoss;
  intent.strategy = "alpha";
  return intent;
}

}  // namespace

TEST_CASE("intent overload: an in-band limit with an OUT-OF-BAND trigger is still caught",
          "[priceband][IMP-11]") {
  // THE CASE THE SYNTHESIZED TRIGGER COULD NOT SEE. Passing `price` twice made the
  // trigger equal to an in-band limit, so this stop-limit looked clean while its
  // real trigger (120, above the 110 ceiling) would be exchange-rejected.
  const broker_exec::domain::OrderIntent intent = stop_limit_intent(/*limit=*/100, /*trigger=*/120);

  const BandCheckResult entry = check_price_band(intent, band_90_110(), /*is_exit=*/false);
  CHECK(entry.verdict == BandVerdict::OutsideBandBlocked);
  CHECK(entry.blocked);
  REQUIRE(entry.has_trigger_suggestion);
  CHECK(entry.suggested_trigger == Money::from_rupees(110));  // clamped to the ceiling
  CHECK_FALSE(require_band_ok(intent, band_90_110(), false).has_value());

  // The same order as an EXIT is clamped, never blocked (the module's spine).
  const BandCheckResult exit = check_price_band(intent, band_90_110(), /*is_exit=*/true);
  CHECK(exit.verdict == BandVerdict::OutsideBandExitClamped);
  CHECK_FALSE(exit.blocked);
  CHECK(require_band_ok(intent, band_90_110(), true).has_value());
}

TEST_CASE("intent overload: a stop-limit fully inside the band passes", "[priceband][IMP-11]") {
  const broker_exec::domain::OrderIntent intent = stop_limit_intent(/*limit=*/99, /*trigger=*/100);
  const BandCheckResult r = check_price_band(intent, band_90_110(), /*is_exit=*/false);
  CHECK(r.verdict == BandVerdict::WithinBand);
  CHECK_FALSE(r.blocked);
  CHECK(require_band_ok(intent, band_90_110(), false).has_value());
}

TEST_CASE("intent overload: a trigger-less order is not made to look like a stop",
          "[priceband][IMP-11]") {
  // An absent trigger maps to zero Money, which is far OUTSIDE [90,110] — so if
  // the module ever started checking a plain Limit's trigger, this would flip to
  // blocked. It must stay WithinBand: only a stop-limit has a trigger to check.
  broker_exec::domain::OrderIntent intent = stop_limit_intent(100, 100);
  intent.order_type = OrderType::Limit;
  intent.trigger_price.reset();

  const BandCheckResult r = check_price_band(intent, band_90_110(), /*is_exit=*/false);
  CHECK(r.verdict == BandVerdict::WithinBand);
  CHECK_FALSE(r.blocked);

  // And a PLAIN market order is unchecked entirely, trigger-less by definition.
  intent.order_type = OrderType::Market;
  CHECK(check_price_band(intent, band_90_110(), false).verdict == BandVerdict::MarketUnchecked);

  // An SL-M, by contrast, IS checked — on its trigger (IMP-11). Its limit is
  // ignored, so an in-band trigger passes no matter what `price` holds.
  intent.order_type = OrderType::StopLossMarket;
  intent.price = broker_exec::domain::Price::from_rupees(500);  // ignored
  intent.trigger_price = broker_exec::domain::Price::from_rupees(100);
  CHECK(check_price_band(intent, band_90_110(), false).verdict == BandVerdict::WithinBand);

  intent.trigger_price = broker_exec::domain::Price::from_rupees(500);  // out of band
  CHECK(check_price_band(intent, band_90_110(), false).verdict == BandVerdict::OutsideBandBlocked);
}
