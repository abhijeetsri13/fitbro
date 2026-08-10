#include "broker_exec/options/margin_shock.hpp"

#include "broker_exec/capabilities/capabilities.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::options {

// `blocked` is DERIVED from the outcome in one place so the convenience flag the
// gate reads can never desync from the outcome (a future outcome can't silently
// ship with the wrong flag). The two Blocked* outcomes are the only blocking ones.
[[nodiscard]] static bool is_blocking(MarginShockOutcome outcome) noexcept {
  return outcome == MarginShockOutcome::BlockedMarginShock ||
         outcome == MarginShockOutcome::BlockedUnavailableNetShort;
}

std::string_view to_string(MarginShockOutcome outcome) noexcept {
  switch (outcome) {
    case MarginShockOutcome::WithinShockLimit:
      return "WithinShockLimit";
    case MarginShockOutcome::BlockedMarginShock:
      return "BlockedMarginShock";
    case MarginShockOutcome::BlockedUnavailableNetShort:
      return "BlockedUnavailableNetShort";
    case MarginShockOutcome::AllowedUnavailableBounded:
      return "AllowedUnavailableBounded";
  }
  return "Unknown";
}

MarginShockResult evaluate_margin_shock(capabilities::Support span_support,
                                        const MarginShockInputs& inputs,
                                        const MarginShockSeams& seams) {
  MarginShockResult result;

  // ── Step 1: CAPABILITY GATE (fail-closed) ──────────────────────────────────
  // SPAN is "available" ONLY when the broker advertises it (Support::Supported),
  // a source seam is actually wired in, AND the source call returns a value.
  // Unknown / Unsupported (the tri-state fail-closed default), a null source, OR
  // a source that errors all fall through to the UNAVAILABLE path — we cannot
  // model the shock, so a net-short basket must fail closed (AC-2).
  if (span_support == capabilities::Support::Supported && seams.span_margin_source) {
    auto model = seams.span_margin_source();
    if (model) {
      // ── Step 2: AVAILABLE PATH (AC-1) ──────────────────────────────────────
      // Record the modeled requirement and test the RMS auto-square-off line.
      result.span_available = true;
      result.margin_now = model.value().margin_now;
      result.margin_under_shock = model.value().margin_under_shock;

      // CROSSING test: the basket crosses the auto-square-off threshold when the
      // SHOCKED requirement exceeds deployable funds. STRICT integer `>` (Money
      // paise): exactly-equal still FITS and is WithinShockLimit (documented).
      // Defense-in-depth: also block if the requirement is ALREADY over the line
      // NOW (a well-formed source has under_shock >= now so the shock test already
      // catches this, but a malformed source with under_shock < now must not slip
      // an already-crossing basket through — FR-18 is "don't open the force-liquidatable").
      if (result.margin_under_shock > inputs.available_margin ||
          result.margin_now > inputs.available_margin) {
        result.outcome = MarginShockOutcome::BlockedMarginShock;
        result.detail = "SPAN shock CROSSES auto-square-off: required margin (now " +
                        result.margin_now.to_string() + ", shocked " +
                        result.margin_under_shock.to_string() + ") exceeds available " +
                        inputs.available_margin.to_string() + " - basket BLOCKED pre-submission";
      } else {
        result.outcome = MarginShockOutcome::WithinShockLimit;
        result.detail = "SPAN shock within limit: shocked margin " +
                        result.margin_under_shock.to_string() + " fits within available " +
                        inputs.available_margin.to_string() + " - basket allowed";
      }

      // ── Step 4: AUDIT + RETURN (AC-3) ──────────────────────────────────────
      result.available_margin = inputs.available_margin;
      result.blocked = is_blocking(result.outcome);
      if (seams.audit) {
        seams.audit(result);
      }
      return result;
    }
    // A source Error means we could NOT model the shock => fall through to the
    // UNAVAILABLE path (fail-closed for a net-short basket). NEVER touch
    // model.value() on the error branch.
  }

  // ── Step 3: UNAVAILABLE PATH (AC-2) ────────────────────────────────────────
  // No usable SPAN source. The Money fields are zero (from_paise(0)) — documented
  // as NOT-MODELED, not "zero margin required".
  result.span_available = false;
  result.margin_now = domain::Money::from_paise(0);
  result.margin_under_shock = domain::Money::from_paise(0);

  if (inputs.basket_is_net_short) {
    // FAIL CLOSED: a net-short basket is force-liquidatable and its true margin
    // can NOT be approximated by summing per-leg margins (which under-counts a
    // short spread). With no SPAN source there is no safe number => block.
    result.outcome = MarginShockOutcome::BlockedUnavailableNetShort;
    result.detail =
        "SPAN/basket-margin unavailable on this broker; net-short basket blocked - no "
        "summed-legs fallback";
  } else {
    // A defined-risk / long (not net-short) basket is not force-liquidatable the
    // same way; AC-2 only mandates the net-short fail-closed, so this is allowed.
    result.outcome = MarginShockOutcome::AllowedUnavailableBounded;
    result.detail = "SPAN unavailable; basket is not net-short (bounded risk) - allowed";
  }

  // ── Step 4: AUDIT + RETURN (AC-3) ──────────────────────────────────────────
  result.available_margin = inputs.available_margin;
  result.blocked = is_blocking(result.outcome);
  if (seams.audit) {
    seams.audit(result);
  }
  return result;
}

}  // namespace broker_exec::options
