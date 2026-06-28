#include "broker_exec/priceband/band_check.hpp"

#include <catch2/catch_test_macros.hpp>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
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
  const BandCheckResult exit =
      check_price_band(Side::Sell, OrderType::StopLoss, Money::from_rupees(100),
                       /*trigger=*/Money::from_rupees(70), band_90_110(), /*is_exit=*/true);
  CHECK(exit.verdict == BandVerdict::OutsideBandExitClamped);
  CHECK_FALSE(exit.blocked);

  const BandCheckResult both_in =
      check_price_band(Side::Buy, OrderType::StopLoss, Money::from_rupees(100),
                       /*trigger=*/Money::from_rupees(95), band_90_110(), false);
  CHECK(both_in.verdict == BandVerdict::WithinBand);
}

TEST_CASE("market-style orders are MarketUnchecked, never blocked", "[priceband]") {
  for (const OrderType t : {OrderType::Market, OrderType::StopLossMarket}) {
    const BandCheckResult r =
        check_price_band(Side::Buy, t, Money::from_rupees(999), Money::from_rupees(999),
                         band_90_110(), /*is_exit=*/false);
    CHECK(r.verdict == BandVerdict::MarketUnchecked);
    CHECK_FALSE(r.blocked);
    CHECK_FALSE(r.has_suggestion);
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

TEST_CASE("CORE INVARIANT: no out-of-band ENTRY is ever allowed through unblocked",
          "[priceband]") {
  const PriceBand band = band_90_110();
  // Sweep both sides, both limit/stop-limit order types, and prices on both sides
  // of the band. For every ENTRY that is out of band, blocked MUST be true; for
  // every EXIT, blocked MUST be false.
  const Side sides[] = {Side::Buy, Side::Sell};
  const OrderType limit_types[] = {OrderType::Limit, OrderType::StopLoss};
  const Money prices[] = {Money::from_rupees(50), Money::from_rupees(89), Money::from_rupees(90),
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
