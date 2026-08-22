#include "broker_exec/marginsafety/margin_buffer.hpp"

#include <cstdint>
#include <string>

#include "broker_exec/domain/money.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::marginsafety {

namespace {

// `blocked` is DERIVED from the verdict in ONE place so the convenience flag the
// gate reads can never desync from the verdict (a future verdict can't ship with
// the wrong flag). InsufficientBlocked is the only blocking verdict.
[[nodiscard]] bool is_blocking(MarginVerdict verdict) noexcept {
  return verdict == MarginVerdict::InsufficientBlocked;
}

// The larger of two Money amounts (Money has operator<=>). Used for the worst-case
// pick and the defensive api_required floor.
[[nodiscard]] domain::Money max_money(domain::Money a, domain::Money b) noexcept {
  return a >= b ? a : b;
}

// Saturating integer add over paise: caps at INT64_MAX rather than wrapping. Both
// operands are non-negative on every call here (base, flat cushion, bps cushion),
// so saturating UP keeps the result fail-closed — a larger requirement is always
// the safe direction, and the sum can never wrap to a SMALLER value than `a`.
[[nodiscard]] std::int64_t sat_add(std::int64_t a, std::int64_t b) noexcept {
  if (b > 0 && a > INT64_MAX - b) {
    return INT64_MAX;
  }
  return a + b;
}

// The ROUNDED-UP (ceil) bps cushion in paise, overflow-guarded.
//   buffer_amount = ceil(base_paise * bps / 10000) = (base_paise * bps + 9999) / 10000
// Rounding UP (not the integer-division default of DOWN) keeps the cushion in the
// fail-closed direction — we never shave a paise off the safety pad. A non-positive
// base or bps yields no cushion. If the product base_paise * bps would overflow
// int64 (an absurd margin), we SATURATE the cushion to INT64_MAX instead of
// wrapping: a maximal requirement is the safe direction and the saturating add
// downstream caps the effective requirement at INT64_MAX.
[[nodiscard]] std::int64_t bps_cushion(std::int64_t base_paise, std::int64_t bps) noexcept {
  if (base_paise <= 0 || bps <= 0) {
    return 0;
  }
  // Overflow guard: base_paise * bps + 9999 must fit in int64. If base_paise
  // exceeds (INT64_MAX - 9999) / bps the product would overflow — saturate UP.
  if (base_paise > (INT64_MAX - 9999) / bps) {
    return INT64_MAX;
  }
  return (base_paise * bps + 9999) / 10000;
}

}  // namespace

std::string_view to_string(MarginVerdict verdict) noexcept {
  switch (verdict) {
    case MarginVerdict::Sufficient:
      return "Sufficient";
    case MarginVerdict::InsufficientBlocked:
      return "InsufficientBlocked";
  }
  return "Unknown";
}

MarginSafetyResult evaluate_margin(const MarginInputs& in, const MarginSafetyConfig& cfg) {
  MarginSafetyResult result;  // fail-closed defaults (InsufficientBlocked/blocked)

  // ── Step 1: BASE REQUIRED (never trust a single broker number) ──────────────
  // Single-leg, and multi-leg with a TRUSTED benefit, use the broker's reported
  // (netted) figure. A multi-leg basket whose benefit is NOT trusted (the SPAN-
  // not-fixed / pre-position boundary) is sized against the WORST CASE: the summed
  // per-leg margins with no benefit. We never quietly trust the netted quote when
  // the benefit could be revoked at execution.
  domain::Money base = in.api_required;
  if (in.is_multi_leg && !in.benefit_trusted) {
    base = max_money(in.summed_leg_margin, in.api_required);
    result.used_worst_case = true;
  }
  // DEFENSIVE FLOOR (always): the buffer must never enforce LESS than the broker
  // already says, even on odd inputs (e.g. summed_leg_margin < api_required).
  base = max_money(base, in.api_required);
  result.base_required = base;

  // ── Step 2: EFFECTIVE REQUIRED = base + flat cushion + bps cushion ──────────
  // The bps cushion is rounded UP and overflow-guarded; the flat cushion is
  // floored at zero (a negative cushion would shrink the requirement — unsafe).
  // Every add saturates at INT64_MAX, so effective_required is ALWAYS >= base and
  // can never wrap to a smaller (negative) requirement.
  const std::int64_t bps = cfg.buffer_bps > 0 ? cfg.buffer_bps : 0;  // clamp >= 0
  const std::int64_t flat = cfg.flat_buffer.paise() > 0 ? cfg.flat_buffer.paise() : 0;
  const std::int64_t cushion = bps_cushion(base.paise(), bps);

  std::int64_t effective = base.paise();
  effective = sat_add(effective, flat);
  effective = sat_add(effective, cushion);
  result.effective_required = domain::Money::from_paise(effective);

  // ── Step 3: VERDICT ─────────────────────────────────────────────────────────
  // Sufficient iff deployable funds cover the buffered requirement (>= : exactly
  // covering the buffer is enough). Else BLOCK pre-submission.
  if (in.available >= result.effective_required) {
    result.verdict = MarginVerdict::Sufficient;
  } else {
    result.verdict = MarginVerdict::InsufficientBlocked;
  }
  result.blocked = is_blocking(result.verdict);

  // ── Step 4: STALENESS FLAG ──────────────────────────────────────────────────
  // The benefit not being trusted == we are near the 9:20 / expiry SPAN boundary.
  result.boundary_flagged = !in.benefit_trusted;

  // Redaction-safe detail: verdict + Money figures + flags only (no secrets, no
  // raw broker text).
  result.detail = std::string(to_string(result.verdict)) + ": effective_required " +
                  result.effective_required.to_string() + " (base " +
                  result.base_required.to_string() + ") vs available " + in.available.to_string() +
                  " [worst_case=" + (result.used_worst_case ? "true" : "false") +
                  ", boundary_flagged=" + (result.boundary_flagged ? "true" : "false") + "]";

  return result;
}

Result<ports::Ok> require_margin_ok(const MarginSafetyResult& r) {
  if (!r.blocked) {
    return ports::ok();
  }
  // Blocked: a margin-safety shortfall against the buffered requirement. RiskRejected
  // (broker-RMS / risk-grounds family) with BlockStrategy (halt; a funds problem).
  // Redaction-safe: Money is non-secret; no raw broker text.
  errors::Error err = errors::make_error(
      errors::ErrorCategory::RiskRejected,
      "margin safety: " + std::string(to_string(r.verdict)) + " - effective_required " +
          r.effective_required.to_string() + " exceeds available funds (buffered requirement)");
  err.action = errors::SuggestedAction::BlockStrategy;
  return broker_exec::fail(err);
}

}  // namespace broker_exec::marginsafety
