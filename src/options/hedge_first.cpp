#include "broker_exec/options/hedge_first.hpp"

#include <string>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/ports_common.hpp"

namespace broker_exec::options {

namespace {

// A redaction-safe tag for an Error: the stable error-category NAME only. The
// raw broker text / message is NEVER copied into `detail` or an alert, so no
// token-shaped content can leak (the alert/`detail` redaction-safe contract).
[[nodiscard]] std::string error_tag(const errors::Error& err) {
  return std::string(errors::to_string(err.category));
}

// Is this Error AMBIGUOUS about whether the order reached the exchange?
//
// The SAME rule as `Dispatcher::is_reconcile_first` (src/runtime/dispatcher.cpp)
// and as basket.cpp's copy: a ReconcileFirst action, or a Timeout / Network /
// Unknown category, means the order MAY be live at the exchange. It is duplicated
// rather than called because `options` depends inward on `ports` + `errors` only
// and must never link `runtime` (the boundary cmake/HexagonalBoundary.cmake
// enforces) — the same reason `error_tag` is re-declared per TU here. KEEP ALL
// THREE IN SYNC: a category that becomes reconcile-first in the dispatcher must
// become ambiguous here too.
[[nodiscard]] bool is_ambiguous(const errors::Error& err) noexcept {
  if (err.action == errors::SuggestedAction::ReconcileFirst) {
    return true;
  }
  return err.category == errors::ErrorCategory::Timeout ||
         err.category == errors::ErrorCategory::Network ||
         err.category == errors::ErrorCategory::Unknown;
}

}  // namespace

std::string_view to_string(HedgeFirstOutcome outcome) noexcept {
  switch (outcome) {
    case HedgeFirstOutcome::HedgedShortLive:
      return "HedgedShortLive";
    case HedgeFirstOutcome::HedgePlacementFailed:
      return "HedgePlacementFailed";
    case HedgeFirstOutcome::HedgeUnconfirmed:
      return "HedgeUnconfirmed";
    case HedgeFirstOutcome::ShortPlacementFailed:
      return "ShortPlacementFailed";
    case HedgeFirstOutcome::NakedShortRemediated:
      return "NakedShortRemediated";
    case HedgeFirstOutcome::ShortAmbiguousReconcileRequired:
      return "ShortAmbiguousReconcileRequired";
  }
  return "Unknown";
}

HedgeFirstResult execute_hedge_first(const HedgeFirstSeams& seams, ports::AlertSink& alerts) {
  HedgeFirstResult result;

  // ── Step 1: place the protective hedge FIRST ──────────────────────────────
  // Null seam or an Error aborts BEFORE the short is ever considered (AC-2): a
  // missing seam fails closed exactly like a placement error.
  if (!seams.place_hedge) {
    result.outcome = HedgeFirstOutcome::HedgePlacementFailed;
    result.detail = "hedge placement seam not configured";
    return result;
  }
  auto hedge_ack = seams.place_hedge();
  if (!hedge_ack) {
    // An ambiguous hedge outcome is deliberately NOT split out here: the short is
    // never sent on this path, so no risk position can exist and there is no
    // safety claim for a caller to act on — at worst a stray protective long is
    // left at the broker for reconciliation to find. Contrast Step 3, where the
    // same ambiguity would invite the caller to remove a LIVE hedge.
    result.outcome = HedgeFirstOutcome::HedgePlacementFailed;
    result.detail = "hedge placement failed: " + error_tag(hedge_ack.error());
    return result;
  }
  result.hedge_order_id = hedge_ack.value().broker_order_id;

  // ── Step 2: confirm the hedge is live BEFORE sending the short ─────────────
  // Fail-closed: a null seam, an Error checking confirmation, OR a `false` all
  // mean NOT confirmed; the short is never sent (AC-2). An error is never treated
  // as optimistic success.
  if (!seams.confirm_hedge) {
    result.outcome = HedgeFirstOutcome::HedgeUnconfirmed;
    result.detail = "hedge confirmation seam not configured";
    return result;
  }
  auto confirmed = seams.confirm_hedge(result.hedge_order_id);
  if (!confirmed) {
    result.outcome = HedgeFirstOutcome::HedgeUnconfirmed;
    result.detail = "hedge confirmation check failed: " + error_tag(confirmed.error());
    return result;
  }
  if (!confirmed.value()) {
    result.outcome = HedgeFirstOutcome::HedgeUnconfirmed;
    result.detail = "hedge not confirmed live";
    return result;
  }

  // ── Step 3: place the short (reached ONLY with a confirmed hedge) ──────────
  // Null seam or a DEFINITIVE rejection -> ShortPlacementFailed. The hedge stands:
  // a lone long hedge is SAFE (not naked), so NO Critical alert and NO emergency
  // action, and the hedge order id is returned so the caller can keep/close it.
  // An AMBIGUOUS Error takes the branch below instead — that safety claim is only
  // true when the short's absence is PROVEN.
  if (!seams.place_short) {
    result.outcome = HedgeFirstOutcome::ShortPlacementFailed;
    result.detail = "short placement seam not configured (hedge stands)";
    return result;
  }
  auto short_ack = seams.place_short();
  if (!short_ack) {
    // AMBIGUOUS IS NOT REJECTED. `ShortPlacementFailed` is a SAFETY CLAIM — it
    // tells the caller a lone long hedge stands and it is safe to keep or CLOSE
    // it. On a Timeout/Network/Unknown the short MAY already be live at the
    // exchange, and a caller acting on that claim closes the hedge over a live
    // short: the naked position this module exists to prevent. Only a definitive
    // broker verdict earns the claim.
    if (is_ambiguous(short_ack.error())) {
      result.outcome = HedgeFirstOutcome::ShortAmbiguousReconcileRequired;
      result.detail = "short placement AMBIGUOUS, short MAY be LIVE (no order id): " +
                      error_tag(short_ack.error()) + "; hedge LEFT in place, reconcile first";
      // The emergency action is deliberately NOT run: it is a square-off/cancel
      // that would remove the HEDGE — the one move that could turn an unproven
      // short into a naked one — and we hold no order id to target the short with
      // anyway. Nothing is naked right now, so the correct action is to leave the
      // book exactly as it is and escalate. Alert last, Result swallowed and
      // wrapped, same best-effort discipline as AC-3 below.
      try {
        (void)alerts.send(ports::AlertLevel::Critical,
                          "SHORT AMBIGUOUS: short placement outcome is UNKNOWN and the short may "
                          "be LIVE at the broker; the hedge was LEFT in place - do NOT close it "
                          "before reconciling");
      } catch (...) {  // NOLINT(bugprone-empty-catch): alerting is strictly best-effort
      }
      return result;
    }
    result.outcome = HedgeFirstOutcome::ShortPlacementFailed;
    result.detail = "short placement failed (hedge stands): " + error_tag(short_ack.error());
    return result;
  }
  result.short_order_id = short_ack.value().broker_order_id;

  // ── Step 4: re-verify the hedge is STILL live after the short (AC-3) ───────
  // Fail-closed: only an explicit `true` proves the hedge is live. A null seam,
  // an Error, or a `false` all mean we cannot prove it -> remediate.
  bool hedge_live = false;
  if (seams.recheck_hedge_live) {
    auto recheck = seams.recheck_hedge_live(result.hedge_order_id);
    hedge_live = recheck && recheck.value();
  }
  if (hedge_live) {
    result.outcome = HedgeFirstOutcome::HedgedShortLive;
    result.detail = "hedge re-verified live after short";
    return result;
  }

  // AC-3 remediation: the short is LIVE but the hedge can no longer be proven
  // live. Two actions: the configured EMERGENCY ACTION (square-off/cancel — the
  // load-bearing safety step that removes the naked risk) and an IMMEDIATE Critical
  // alert (notification). Run the EMERGENCY ACTION FIRST: the alert sink is an
  // external impl that, in exactly the comms-failure conditions that trigger AC-3,
  // could throw — and a thrown alert must NEVER skip the square-off. The square-off
  // matters more than telling someone about it.
  result.outcome = HedgeFirstOutcome::NakedShortRemediated;
  if (seams.emergency_action) {
    auto emergency = seams.emergency_action();
    result.emergency_action_ran = emergency.has_value();
    result.detail = emergency.has_value() ? "naked short remediated: emergency action ran"
                                          : "naked short: emergency action returned error: " +
                                                error_tag(emergency.error());
  } else {
    result.emergency_action_ran = false;
    result.detail = "naked short: no emergency action configured; operator alerted";
  }
  // Best-effort alert AFTER the square-off. Result SWALLOWED, and the call is
  // wrapped so even a THROWING sink cannot derail the (already-run) remediation or
  // break execute_hedge_first's no-throw contract (mirrors health::Watchdog's
  // best-effort alerting, hardened for the higher stakes here).
  try {
    (void)alerts.send(ports::AlertLevel::Critical,
                      "NAKED SHORT: hedge no longer verifiable after short placement; "
                      "emergency action invoked");
  } catch (...) {  // NOLINT(bugprone-empty-catch): alerting is strictly best-effort
  }
  return result;
}

}  // namespace broker_exec::options
