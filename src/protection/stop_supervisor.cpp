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
    if (out < band.lower) out = band.lower;  // never price below the band floor
    if (out > band.upper) out = band.upper;  // and never above the ceiling
  } else {
    if (out > band.upper) out = band.upper;  // never price above the band ceiling
    if (out < band.lower) out = band.lower;  // and never below the floor
  }
  return out;
}

// Best-effort Critical alert. The Result is SWALLOWED and the call is wrapped so
// even a THROWING/dead sink can never derail the (already-decided) protective
// outcome or break evaluate_protection's no-throw contract (mirrors
// options::hedge_first's best-effort alerting). The decision is returned
// regardless of whether delivery succeeded.
void best_effort_alert(ports::AlertSink& alerts, const std::string& message) {
  try {
    (void)alerts.send(ports::AlertLevel::Critical, message);
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
  // (-) exits Buy. exit_qty is the magnitude. Computing the magnitude WITHOUT
  // std::abs avoids the INT64_MIN UB; if the magnitude still comes out
  // non-positive (only possible for INT64_MIN, where negation overflows) we
  // cannot form a valid exit -> FAIL-CLOSED Unprotected.
  const domain::Side exit_side =
      stop.position_qty > 0 ? domain::Side::Sell : domain::Side::Buy;
  const std::int64_t exit_qty =
      stop.position_qty > 0 ? stop.position_qty : -stop.position_qty;
  if (exit_qty <= 0) {
    decision.state = ProtectionState::Unprotected;
    decision.detail =
        "non-positive exit qty for " + stop.symbol + ": cannot form protective exit";
    best_effort_alert(alerts, "PROTECTION: " + stop.symbol +
                                  " has a non-positive exit quantity; cannot arm a "
                                  "protective exit — manual intervention required");
    decision.alert = true;
    return decision;
  }

  // ── Step 3: a confirmed Filled protective exit means we ARE protected ─────
  // NOTE: PartiallyFilled is deliberately NOT Filled — the unfilled remainder is
  // still naked and must fall through to the re-arm below.
  if (in.protective_order_state == domain::OrderState::Filled) {
    decision.state = ProtectionState::Closed;
    decision.detail = "protective exit filled for " + stop.symbol + ": position protected";
    return decision;
  }

  // ── Step 4: trigger not crossed -> protection is latent (Armed) ───────────
  // We TRUST the caller's trigger_crossed flag (the caller owns tick semantics).
  if (!in.trigger_crossed) {
    decision.state = ProtectionState::Armed;
    decision.detail = "armed for " + stop.symbol + ": trigger not crossed";
    return decision;
  }

  // ── Step 5: trigger crossed + exit NOT Filled -> RE-ARM (THE CORE FIX) ────
  // The stop fired (or should have) but the protective exit is not confirmed
  // Filled: Rejected / Cancelled / Unknown / PartiallyFilled, OR no protective
  // order was ever placed. NEVER TRUST THE BROKER GTT — a fired-but-unfilled GTT
  // is already deleted and the position is exposed. Emit a fresh band-aware
  // protective exit and raise Critical.
  decision.state = ProtectionState::ReArmNeeded;
  decision.emit_exit = true;
  decision.exit.symbol = stop.symbol;
  decision.exit.side = exit_side;
  decision.exit.qty = exit_qty;

  const std::string side_tag =
      std::string(domain::to_string(exit_side));  // "Buy"/"Sell", redaction-safe

  if (in.band.valid) {
    decision.exit.limit_price = clamp_into_band(exit_side, stop.protective_limit, in.band);
    decision.detail = "re-arm protective " + side_tag + " for " + stop.symbol +
                      ": stop fired-but-unfilled; limit clamped into band";
    best_effort_alert(alerts, "PROTECTION: re-arming protective " + side_tag + " exit for " +
                                  stop.symbol +
                                  " — stop trigger crossed but protective order did NOT fill");
  } else {
    // FAIL-CLOSED for the price: we do NOT skip the exit (an unprotected
    // position is worse than an at-risk price), but we send it UNCLAMPED and
    // escalate so the operator knows the limit was not made band-safe.
    decision.exit.limit_price = stop.protective_limit;
    decision.detail = "re-arm protective " + side_tag + " for " + stop.symbol +
                      ": stop fired-but-unfilled; band unknown — protective limit unclamped";
    best_effort_alert(alerts,
                      "PROTECTION: re-arming protective " + side_tag + " exit for " +
                          stop.symbol +
                          " — band unknown; protective limit UNCLAMPED (price not made safe)");
  }
  decision.alert = true;
  return decision;
}

}  // namespace broker_exec::protection
