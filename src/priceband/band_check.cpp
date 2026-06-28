#include "broker_exec/priceband/band_check.hpp"

#include <string>

#include "broker_exec/ports/ports_common.hpp"

namespace broker_exec::priceband {

namespace {

// Clamp `price` into [lower, upper] with all-integer Money math (no float). A
// price below the floor is pulled up to `lower`; a price above the ceiling is
// pulled down to `upper`; an in-band price is returned unchanged. Reimplemented
// locally (a one-liner) so this module does NOT depend on `protection`'s
// clamp_into_band — the two are deliberately decoupled.
[[nodiscard]] domain::Money clamp(domain::Money price, domain::Money lower,
                                  domain::Money upper) noexcept {
  return price < lower ? lower : (price > upper ? upper : price);
}

// A price is IN BAND iff lower <= price <= upper (INCLUSIVE on both edges): a
// price sitting exactly on the band edge is acceptable to the exchange.
[[nodiscard]] bool in_band(domain::Money price, const PriceBand& band) noexcept {
  return band.lower <= price && price <= band.upper;
}

// Market-style orders carry NO limit price the band can enforce: a plain Market,
// and a stop-loss-MARKET (SL-M), which fires a market order once its trigger is
// crossed. We cannot pre-clamp a market price, so these are MarketUnchecked.
[[nodiscard]] bool is_market_style(domain::OrderType t) noexcept {
  return t == domain::OrderType::Market || t == domain::OrderType::StopLossMarket;
}

// A stop-LIMIT (Kite `StopLoss` / SL) has BOTH a trigger and a limit price, so
// both must be validated against the band. A plain Limit validates the limit
// alone; this distinguishes the two.
[[nodiscard]] bool is_stop_limit(domain::OrderType t) noexcept {
  return t == domain::OrderType::StopLoss;
}

}  // namespace

std::string_view to_string(BandVerdict verdict) noexcept {
  switch (verdict) {
    case BandVerdict::WithinBand:
      return "within_band";
    case BandVerdict::OutsideBandBlocked:
      return "outside_band_blocked";
    case BandVerdict::OutsideBandExitClamped:
      return "outside_band_exit_clamped";
    case BandVerdict::MarketUnchecked:
      return "market_unchecked";
    case BandVerdict::BandUnknown:
      return "band_unknown";
  }
  return "unknown";
}

BandCheckResult check_price_band(domain::Side side, domain::OrderType order_type,
                                 domain::Money limit_price, domain::Money trigger_price,
                                 const PriceBand& band, bool is_exit) {
  // Defaults are the SAFE non-acting value: BandUnknown / not-blocked / no
  // suggestion. Each branch below sets only what it needs.
  BandCheckResult result;
  (void)side;  // side is carried for redaction-safe detail / the gate wrapper.

  // ── Step 1+2: the band must be KNOWN and well-formed to validate anything ──
  // An inverted/degenerate band (known && lower > upper) is malformed garbage;
  // clamping into it would emit an edge price the exchange still rejects, so we
  // treat it like an unavailable band. NON-FREEZING DEFAULT: a missing band does
  // NOT block trading (that would turn a data outage into a self-inflicted
  // freeze) — we let the order through UNVALIDATED and flag it for escalation.
  const bool band_usable = band.known && band.lower <= band.upper;
  if (!band_usable) {
    result.verdict = BandVerdict::BandUnknown;
    result.blocked = false;
    result.has_suggestion = false;
    result.detail = "band unavailable — price not validated";
    return result;
  }

  // ── Step 3: market-style order — no limit price to enforce ────────────────
  // CAVEAT (documented): the exchange MAY still reject a market order out-of-band
  // on a fast instrument, but there is no limit price for us to pre-clamp.
  if (is_market_style(order_type)) {
    result.verdict = BandVerdict::MarketUnchecked;
    result.blocked = false;
    result.detail = "market order — band not price-enforceable";
    return result;
  }

  // ── Step 4: LIMIT / STOP-LIMIT — gather every price that must be in band ──
  // The limit is always checked; a stop-limit ALSO checks its trigger. In-band is
  // inclusive of both edges.
  const bool limit_in = in_band(limit_price, band);
  const bool check_trigger = is_stop_limit(order_type);
  const bool trigger_in = !check_trigger || in_band(trigger_price, band);
  if (limit_in && trigger_in) {
    result.verdict = BandVerdict::WithinBand;
    result.blocked = false;
    result.detail = "within circuit/LPP band";
    return result;
  }

  // ── Step 5: OUT OF BAND — exit clamps, entry blocks (THE SPINE) ───────────
  // The suggested limit is the order's limit pulled inside the band. For a STOP-
  // LIMIT, the trigger is clamped too — suggesting only the limit while leaving the
  // trigger out of band would still be exchange-rejected, defeating the clamp.
  result.suggested_limit = clamp(limit_price, band.lower, band.upper);
  result.has_suggestion = true;
  if (check_trigger) {
    result.suggested_trigger = clamp(trigger_price, band.lower, band.upper);
    result.has_trigger_suggestion = true;
  }
  if (is_exit) {
    // INVARIANT: an EXIT is NEVER blocked. A protective exit must still reach the
    // broker during the very volatility that pushed it out of band — so we clamp
    // it to the band edge (where the exchange accepts it) rather than refuse it.
    result.verdict = BandVerdict::OutsideBandExitClamped;
    result.blocked = false;
    result.detail = "exit priced outside band — clamped to band edge so it is not rejected";
  } else {
    // INVARIANT: an out-of-band ENTRY is ALWAYS blocked. Sending it would only
    // earn an exchange rejection ("price out of LPP range") and waste a round-
    // trip; we deny it pre-submission. This explicitly overrides the non-freezing
    // blocked = false default — no out-of-band entry slips through.
    result.verdict = BandVerdict::OutsideBandBlocked;
    result.blocked = true;
    result.detail =
        "entry priced outside circuit/LPP band — blocked pre-submission (would be "
        "exchange-rejected)";
  }
  return result;
}

Result<ports::Ok> require_band_ok(domain::Side side, domain::OrderType order_type,
                                  domain::Money limit_price, domain::Money trigger_price,
                                  const PriceBand& band, bool is_exit) {
  const BandCheckResult result =
      check_price_band(side, order_type, limit_price, trigger_price, band, is_exit);

  // Only a blocked check (an out-of-band ENTRY) is an error. An exit clamp is NOT
  // blocked, so this returns ok() and the caller applies result.suggested_limit;
  // BandUnknown / MarketUnchecked / WithinBand are likewise ok().
  if (!result.blocked) {
    return ports::ok();
  }

  // Redaction-safe Error: names the verdict + side only, no prices/secrets.
  // Validation category + BlockStrategy action so a gate halts the entry.
  errors::Error err = errors::make_error(
      errors::ErrorCategory::Validation,
      "price band: " + std::string(to_string(result.verdict)) + " (" +
          std::string(domain::to_string(side)) + ") — entry outside circuit/LPP band");
  err.action = errors::SuggestedAction::BlockStrategy;
  return fail(err);
}

}  // namespace broker_exec::priceband
