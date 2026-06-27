#include "broker_exec/reconcile/recovery.hpp"

#include <utility>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/lifecycle/lifecycle.hpp"
#include "broker_exec/reconcile/reconciler.hpp"

namespace broker_exec::reconcile {

namespace {

// Count how many orders are in the ambiguous UNKNOWN state (send result never
// confirmed). Captured BEFORE apply so unknowns_resolved is the delta.
[[nodiscard]] int count_unknown(const std::vector<domain::Order>& orders) noexcept {
  int n = 0;
  for (const domain::Order& order : orders) {
    if (order.state == domain::OrderState::Unknown) {
      ++n;
    }
  }
  return n;
}

}  // namespace

std::string_view to_string(RecoveryStatus status) noexcept {
  switch (status) {
    case RecoveryStatus::ResumedSafe:
      return "RESUMED_SAFE";
    case RecoveryStatus::Blocked:
      return "BLOCKED";
    case RecoveryStatus::ManualInterventionRequired:
      return "MANUAL_INTERVENTION_REQUIRED";
  }
  return "BLOCKED";
}

RecoveryOutcome RecoveryCoordinator::recover() {
  RecoveryOutcome outcome;  // Defaults to Blocked, fail-closed.

  // ── 1. LoadState ──────────────────────────────────────────────────────────
  // Replay persisted/intent-log state. On failure we cannot recover yet: stay
  // Blocked and alert (a Warning, not a Critical — this is retryable).
  auto loaded = load_state_();
  if (!loaded) {
    (void)alerts_.send(ports::AlertLevel::Warning, "recovery: could not load state");
    outcome.detail = "could not load state";
    return outcome;  // Blocked
  }
  outcome.orders = std::move(loaded.value());

  // ── 2. CheckSession ───────────────────────────────────────────────────────
  // Never resume on a dead session. A failed session check is retryable
  // (re-establish, then recover again), so this is Blocked, not terminal.
  if (!check_session_()) {
    outcome.detail = "session not established";
    return outcome;  // Blocked
  }

  // ── 3. FetchBroker ────────────────────────────────────────────────────────
  // Read broker truth off-loop. The Reconciler holds NO Store/engine, so this
  // step structurally cannot mutate. A failed fetch == the broker is UNREACHABLE.
  Reconciler rec(clock_);
  auto fetched = rec.fetch(broker_, /*snapshot_seq=*/1);
  if (!fetched) {
    // Broker unreachable. If ANY loaded order is UNKNOWN, this is a DOUBLE FAULT
    // (UNKNOWN order + broker unreachable): we cannot tell whether a phantom
    // position is live, so we refuse to guess. Mark each UNKNOWN order
    // ManualInterventionRequired, escalate (Critical), and STOP — terminal.
    // CRITICAL: we issue NO broker mutation here (no auto-square-off): a human
    // must intervene.
    int unknown_count = 0;
    for (domain::Order& order : outcome.orders) {
      if (order.state == domain::OrderState::Unknown) {
        order.state = domain::OrderState::ManualInterventionRequired;
        ++unknown_count;
      }
    }
    if (unknown_count > 0) {
      (void)alerts_.send(
          ports::AlertLevel::Critical,
          "recovery: double fault (UNKNOWN order + broker unreachable) -> "
          "MANUAL_INTERVENTION_REQUIRED; human intervention required, no auto-square-off");
      outcome.status = RecoveryStatus::ManualInterventionRequired;
      outcome.unknowns_resolved = 0;
      outcome.unknowns_unresolved = unknown_count;
      outcome.escalated = true;
      outcome.detail = "double fault: UNKNOWN order + broker unreachable";
      return outcome;  // terminal
    }
    // No UNKNOWN order: the broker is merely unreachable. Cannot reconcile yet —
    // retry later (Blocked, NOT terminal). No mutation, no escalation.
    outcome.detail = "broker unreachable; cannot reconcile yet";
    return outcome;  // Blocked
  }

  // ── 4. ResolveUnknowns ────────────────────────────────────────────────────
  // Fold broker truth into the LOCAL orders via the FSM (forward-progressing).
  // Capture the initial UNKNOWN count BEFORE apply so the resolved delta is
  // correct. After apply, any order still UNKNOWN is unresolvable against broker
  // truth (it stays UNKNOWN + was alerted by the applier — fail-closed, never
  // auto-squared-off).
  const int unknowns_before = count_unknown(outcome.orders);
  ReconcileApplier applier(engine_, alerts_);
  const auto recon = applier.apply(fetched.value(), outcome.orders);
  const int unknowns_after = count_unknown(outcome.orders);
  outcome.unknowns_unresolved = unknowns_after;
  outcome.mismatches = recon.mismatches;
  // Clamp: apply() only ever DECREASES the Unknown count here — the broker never
  // reports the LOCAL Unknown marker (it is a local-only ambiguity flag), so the
  // FSM can resolve an Unknown forward but can never create one. Guard anyway so
  // the resolved delta can never go negative.
  outcome.unknowns_resolved =
      (unknowns_before > unknowns_after) ? (unknowns_before - unknowns_after) : 0;

  // ── 5. SafeOrBlocked ──────────────────────────────────────────────────────
  // The safe-start gate is the final go/no-go. A failed gate is Blocked.
  if (!safe_start_check_()) {
    outcome.detail = "safe-start gate blocked";
    return outcome;  // Blocked
  }
  // Never resume with open ambiguity: any unresolved UNKNOWN blocks resume.
  if (outcome.unknowns_unresolved > 0) {
    outcome.detail = "open UNKNOWN orders; not safe to resume";
    return outcome;  // Blocked
  }
  // Never resume when the reconcile itself said "do not trade": a phantom broker
  // order (live at the broker, absent from replayed state — the bot placed but
  // died before the intent log was complete) or a vanished acked local order
  // makes apply() set block_new_orders/mismatches. Resuming here would risk a
  // duplicate / over-exposure, so block (retryable — a human/operator inspects).
  if (recon.block_new_orders || recon.mismatches > 0) {
    outcome.detail = "reconciliation mismatch (phantom/vanished order); not safe to resume";
    return outcome;  // Blocked
  }

  // Session ok AND safe-start ok AND no open unknowns AND no mismatch -> resume.
  outcome.status = RecoveryStatus::ResumedSafe;
  outcome.escalated = false;
  outcome.detail = "reconciled clean; safe to resume";
  return outcome;
}

}  // namespace broker_exec::reconcile
