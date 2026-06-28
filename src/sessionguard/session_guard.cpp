#include "broker_exec/sessionguard/session_guard.hpp"

#include <string>

#include "broker_exec/brokerreason/rejection_classifier.hpp"
#include "broker_exec/domain/redaction.hpp"
#include "broker_exec/ports/ports_common.hpp"

namespace broker_exec::sessionguard {

namespace {

// Build the redaction-safe posture detail. By construction it embeds ONLY the
// canonical SessionState name — NEVER any part of broker_error_text — and is then
// run through domain::scrub as defense in depth, so even a future change that
// tried to fold in raw text could not leak a token-shaped secret.
[[nodiscard]] std::string make_detail(session::SessionState state) {
  std::string detail = "session posture '";
  detail += std::string(session::to_string(state));
  detail += "'";
  return domain::scrub(detail);
}

// Compose the posture for an already-resolved effective state. The single source
// of truth for the per-state booleans, so assess_session can never let the flags
// drift out of agreement with the documented mapping.
//
// THE INVARIANT MADE EXPLICIT: allow_exits and allow_reconcile_reads are TRUE in
// EVERY branch — a dead or failed session must still let a position be unwound
// (exit) and the book be learned (reconcile read); only `freeze_entries` (new
// RISK) and `alert` move with the state. Healthy is the sole branch that opens
// entries and stays silent.
[[nodiscard]] SessionPosture posture_for(session::SessionState state) {
  SessionPosture p;
  p.state = state;
  p.allow_exits = true;             // risk-reducing — never withheld
  p.allow_reconcile_reads = true;   // needed to reconcile — retried post-reauth
  p.detail = make_detail(state);
  switch (state) {
    case session::SessionState::Healthy:
      p.freeze_entries = false;
      p.alert = false;
      return p;
    case session::SessionState::NeedsReauth:
      // Auth dead: stop NEW entries and alert, but keep exits/reads alive so an
      // open position can still be unwound and reconciled once the operator
      // re-establishes. We do NOT retry the auth here — that is a human action.
      p.freeze_entries = true;
      p.alert = true;
      return p;
    case session::SessionState::Failed:
      // Non-auth establishment failure: fail CLOSED — freeze entries and alert —
      // yet still permit exits as the safest action.
      p.freeze_entries = true;
      p.alert = true;
      return p;
  }
  // Unreachable for a valid enumerator (the switch is exhaustive; -Wswitch/WX
  // guards an added one). Fail CLOSED to a frozen, alerting posture rather than
  // silently opening entries on an unmapped future state.
  p.state = session::SessionState::Failed;
  p.freeze_entries = true;
  p.alert = true;
  p.detail = make_detail(session::SessionState::Failed);
  return p;
}

// The frozen-entry Error: redaction-safe, names the op, and instructs a session
// re-establish. Mirrors session::KiteSessionEstablisher::needs_reauth_error —
// category SessionExpired + action ReEstablishSession — so the runtime gets the
// SAME typed verdict whether the dead token was caught at start-up or mid-session.
[[nodiscard]] errors::Error frozen_entry_error(OpClass op) {
  errors::Error err = errors::make_error(
      errors::ErrorCategory::SessionExpired,
      "sessionguard: entries frozen; operator must re-establish session (" +
          std::string(to_string(op)) + " not allowed)");
  err.action = errors::SuggestedAction::ReEstablishSession;
  return err;
}

}  // namespace

std::string_view to_string(OpClass op) noexcept {
  switch (op) {
    case OpClass::Entry:
      return "entry";
    case OpClass::Exit:
      return "exit";
    case OpClass::ReconcileRead:
      return "reconcile_read";
  }
  return "unknown";
}

SessionPosture assess_session(session::SessionState current,
                              std::string_view broker_error_text) {
  // REUSE the single auth-failure signal: a mid-session reject that the versioned
  // classifier maps to SessionExpired means the token is auth-dead and OVERRIDES a
  // caller who still believes it is Healthy. Any other classification (margin,
  // circuit, an empty/benign text -> Unknown) is NOT an auth failure, so the
  // posture reflects the caller's last known `current` state unchanged. We read
  // only the classifier's `reason`; we deliberately never touch broker_error_text
  // again (redaction).
  const brokerreason::Classification c =
      brokerreason::classify_rejection(broker_error_text);
  const session::SessionState effective =
      (c.reason == brokerreason::RejectReason::SessionExpired)
          ? session::SessionState::NeedsReauth
          : current;
  return posture_for(effective);
}

Result<ports::Ok> require_op_allowed(const SessionPosture& posture, OpClass op) {
  // Exits and reconcile reads are ALWAYS permitted — a re-auth/fail-closed posture
  // never blocks risk-reducing work or the reads needed to reconcile. This is the
  // load-bearing invariant: it holds even under Failed and a default-constructed
  // (fail-closed) posture, so a dead token can never trap an open position.
  if (op == OpClass::Exit || op == OpClass::ReconcileRead) {
    return ports::ok();
  }

  // Entry is new RISK: allowed ONLY for a HEALTHY, non-frozen posture. We gate on
  // BOTH the state and the freeze flag (defense-in-depth) so a hand-constructed or
  // corrupt posture — e.g. {state=NeedsReauth, freeze_entries=false} — can never
  // let an entry into a dead session. A frozen/non-Healthy entry is refused with
  // the re-establish-session verdict, never blindly retried.
  if (posture.freeze_entries || posture.state != session::SessionState::Healthy) {
    return fail(frozen_entry_error(op));
  }
  return ports::ok();
}

bool is_reauth_needed(const SessionPosture& posture) noexcept {
  return posture.state == session::SessionState::NeedsReauth;
}

}  // namespace broker_exec::sessionguard
