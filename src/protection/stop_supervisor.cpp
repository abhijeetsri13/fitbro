#include "broker_exec/protection/stop_supervisor.hpp"

#include <string>

#include "broker_exec/ports/alert_sink.hpp"

namespace broker_exec::protection {

namespace {

// Clamp `limit` INTO the (assumed-valid) band [band.lower, band.upper]. This is
// the LPP/circuit fix: the re-armed protective exit price is pulled inside the
// band so the exchange cannot reject it "price out of LPP range" — the exact
// failure that silently deletes a fired GTT and leaves the position naked.
//
// For a SELL (long exit) the priority is "never below the floor" then "never
// above the ceiling"; for a BUY (short exit) it is "never above the ceiling"
// then "never below the floor". Both collapse to clamping into [lower, upper],
// but the directional comments capture WHY each edge matters for that side.
[[nodiscard]] domain::Money clamp_into_band(domain::Side side, domain::Money limit,
                                            const PriceBand& band) {
  domain::Money out = limit;
  if (side == domain::Side::Sell) {
    if (out < band.lower)
      out = band.lower;  // never price below the band floor
    if (out > band.upper)
      out = band.upper;  // and never above the ceiling
  } else {
    if (out > band.upper)
      out = band.upper;  // never price above the band ceiling
    if (out < band.lower)
      out = band.lower;  // and never below the floor
  }
  return out;
}

// Best-effort Critical alert. The Result is SWALLOWED and the call is wrapped so
// even a THROWING/dead sink can never derail the (already-decided) protective
// outcome or break evaluate_protection's no-throw contract (mirrors
// options::hedge_first's best-effort alerting). The decision is returned
// regardless of whether delivery succeeded.
//
// THE SYMBOL TRAVELS AS TYPED PROVENANCE, NOT IN THE BODY (IMP-16). A sink scrubs
// the whole free-form body, and scrub()'s bare high-entropy rule redacts any
// >=20-char run mixing letters and digits — so an interpolated
// "PROTECTION: BANKNIFTY24JUN52000CE ..." reached the operator as
// "PROTECTION: ***REDACTED*** ...". The single most urgent alert this module can
// raise — "your position is unprotected" — named NO INSTRUMENT for precisely the
// index options this library trades (NIFTY at 17 chars survived; FINNIFTY,
// BANKNIFTY and MIDCPNIFTY at 20-22 did not). ports::AlertContext::symbol is
// rendered through the whole-column symbol allowlist instead.
void best_effort_alert(ports::AlertSink& alerts, const std::string& message,
                       const std::string& symbol) {
  ports::AlertContext provenance;
  provenance.symbol = symbol;
  try {
    (void)alerts.send_with_context(ports::AlertLevel::Critical, message, provenance);
  } catch (...) {  // NOLINT(bugprone-empty-catch): alerting is strictly best-effort
  }
}

}  // namespace

std::string_view to_string(ProtectionState state) noexcept {
  switch (state) {
    case ProtectionState::Armed:
      return "Armed";
    case ProtectionState::Closed:
      return "Closed";
    case ProtectionState::ReArmNeeded:
      return "ReArmNeeded";
    case ProtectionState::Unprotected:
      return "Unprotected";
  }
  return "Unknown";
}

ProtectionDecision evaluate_protection(const ProtectiveStop& stop, const StopInputs& in,
                                       ports::AlertSink& alerts) {
  ProtectionDecision decision;

  // ── Step 1: a flat position has nothing to protect ────────────────────────
  if (stop.position_qty == 0) {
    decision.state = ProtectionState::Closed;
    decision.detail = "flat position (" + stop.symbol + "): nothing to protect";
    return decision;
  }

  // ── Step 2: derive the exit (OPPOSITE side, absolute size) ────────────────
  // The exit side is the opposite of the position: long (+) exits Sell, short
  // (-) exits Buy. exit_qty is the magnitude, computed via UNSIGNED negation so
  // INT64_MIN is well-defined (plain `-INT64_MIN` is signed-overflow UB and would
  // TRAP under the UBSan build). A genuinely non-positive magnitude is impossible
  // after this, but the guard stays as defence-in-depth -> FAIL-CLOSED Unprotected.
  const domain::Side exit_side = stop.position_qty > 0 ? domain::Side::Sell : domain::Side::Buy;
  const std::int64_t exit_qty =
      stop.position_qty > 0
          ? stop.position_qty
          : static_cast<std::int64_t>(0ULL - static_cast<std::uint64_t>(stop.position_qty));
  if (exit_qty <= 0) {
    decision.state = ProtectionState::Unprotected;
    decision.detail = "non-positive exit qty for " + stop.symbol + ": cannot form protective exit";
    best_effort_alert(alerts,
                      "PROTECTION: non-positive exit quantity; cannot arm a protective "
                      "exit — manual intervention required",
                      stop.symbol);
    decision.alert = true;
    return decision;
  }

  // ── Step 3: trigger not crossed -> protection is latent (Armed) ───────────
  // We TRUST the caller's trigger_crossed flag (the caller owns tick semantics).
  // NB: there is deliberately NO "protective_order_state == Filled -> Closed"
  // shortcut here. The SOURCE OF TRUTH for exposure is the live position_qty, NOT
  // a broker order flag: a position that the protective exit actually flattened is
  // reported as position_qty == 0 and already returned Closed in Step 1. A `Filled`
  // flag arriving while position_qty is still non-zero (a stale/leftover fill, an
  // inconsistent !known+Filled, or a fill that did not flatten the position) means
  // the position is STILL exposed — trusting the flag there would leave a naked
  // position. So a crossed trigger on a non-flat position ALWAYS re-arms.
  if (!in.trigger_crossed) {
    decision.state = ProtectionState::Armed;
    decision.detail = "armed for " + stop.symbol + ": trigger not crossed";
    return decision;
  }

  // ── Step 4: trigger crossed on a still-exposed position -> RE-ARM (CORE) ───
  // The stop fired (or should have) and position_qty is non-zero, so the position
  // is exposed. Whatever the protective order claims (Rejected / Cancelled /
  // Unknown / PartiallyFilled / a stale Filled / never placed), the live position
  // is not flat. NEVER TRUST THE BROKER GTT — a fired-but-unfilled GTT is already
  // deleted. Emit a fresh band-aware protective exit and raise Critical.
  decision.state = ProtectionState::ReArmNeeded;
  decision.emit_exit = true;
  decision.exit.symbol = stop.symbol;
  decision.exit.side = exit_side;
  decision.exit.qty = exit_qty;

  const std::string side_tag =
      std::string(domain::to_string(exit_side));  // "Buy"/"Sell", redaction-safe

  // An inverted band (lower > upper) is a malformed/garbage band — clamping into
  // it would emit an edge price the exchange still rejects, defeating the point.
  // Treat it like an unknown band: emit unclamped + escalate, never silently trust.
  const bool band_usable = in.band.valid && in.band.lower <= in.band.upper;
  if (band_usable) {
    decision.exit.limit_price = clamp_into_band(exit_side, stop.protective_limit, in.band);
    decision.detail = "re-arm protective " + side_tag + " for " + stop.symbol +
                      ": stop fired-but-unfilled; limit clamped into band";
    best_effort_alert(alerts,
                      "PROTECTION: re-arming protective " + side_tag +
                          " exit — stop trigger crossed but protective order did NOT fill",
                      stop.symbol);
  } else {
    // FAIL-CLOSED for the price: we do NOT skip the exit (an unprotected
    // position is worse than an at-risk price), but we send it UNCLAMPED and
    // escalate so the operator knows the limit was not made band-safe.
    decision.exit.limit_price = stop.protective_limit;
    decision.detail = "re-arm protective " + side_tag + " for " + stop.symbol +
                      ": stop fired-but-unfilled; band unknown — protective limit unclamped";
    best_effort_alert(alerts,
                      "PROTECTION: re-arming protective " + side_tag +
                          " exit — band unknown; protective limit UNCLAMPED (price not "
                          "made safe)",
                      stop.symbol);
  }
  decision.alert = true;
  return decision;
}

}  // namespace broker_exec::protection
