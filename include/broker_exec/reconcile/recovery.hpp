#pragma once

// broker_exec::reconcile — crash recovery / reconcile-before-resume (Story 3.4,
// FR-14, NFR-2 double-fault).
//
// THE RECONCILE-BEFORE-RESUME GUARANTEE. After a crash the bot must NEVER trade
// immediately on restart. The RecoveryCoordinator runs a fail-closed state
// machine that loads persisted state, confirms the broker session is alive,
// fetches broker truth, resolves any UNKNOWN (ambiguous in-flight) orders against
// that truth, and resumes ONLY when it is provably safe to do so:
//
//   load-state -> session -> fetch broker truth -> resolve unknowns -> safe-start
//
// READ + RECONCILE ONLY (zero duplicates, AC-2). recover() issues NO broker
// mutation — it never calls place/modify/cancel/square_off. It only READS (via
// the 3.1 `Reconciler::fetch`) and folds broker truth into a LOCAL copy of the
// orders (via the 3.1 `ReconcileApplier::apply`, which drives the lifecycle FSM).
// Because recovery never re-sends an order, a process kill mid-flight cannot
// produce a duplicate.
//
// DOUBLE FAULT = MANUAL_INTERVENTION_REQUIRED (AC-3, NFR-2). If an order is
// UNKNOWN (its send result was never confirmed) AND the broker is unreachable
// (the fetch fails), recovery cannot tell whether a phantom position is live.
// It refuses to guess: it marks each such order ManualInterventionRequired,
// escalates with a Critical alert, and stops — terminal. It NEVER auto-squares-off
// a position it cannot see. A human must intervene.
//
// SEAMS (deterministic, decoupled). The three integration points the coordinator
// needs are injected as std::function so it composes 1.5 intent-log replay,
// the 2.4 session check, and the 2.13 safe-start gate without depending on them:
//   * load_state       — replay persisted/intent-log state into the orders that
//                        MIGHT have been sent (incl. any Unknown/Sent).
//   * check_session    — is a broker session established (never resume on a dead
//                        session).
//   * safe_start_check — the safe-start gate (post-reconcile go/no-go).
//
// CROSS-PLATFORM: C++20 standard library only. No OS APIs, no `#ifdef`, no
// floating point, no throw across the boundary (fail-closed via RecoveryOutcome).

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "broker_exec/domain/types.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/clock_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::lifecycle {
// Forward-declared: the engine is held by reference and only passed through to
// the ReconcileApplier inside recovery.cpp, so the public header does not depend
// on the lifecycle header (keeps the lifecycle link PRIVATE, matching 3.1).
class LifecycleEngine;
}  // namespace broker_exec::lifecycle

namespace broker_exec::reconcile {

// The terminal verdict of a recovery run.
enum class RecoveryStatus {
  ResumedSafe,                 // Reconciled clean; safe to resume trading.
  Blocked,                     // Not safe yet (retryable) — do NOT resume.
  ManualInterventionRequired,  // Double fault: a human must intervene (terminal).
};

// Stable, log-friendly name for a RecoveryStatus (observability contract).
[[nodiscard]] std::string_view to_string(RecoveryStatus status) noexcept;

// The outcome of a recovery run: the verdict plus the (locally reconciled)
// orders and the unknown-resolution accounting. `orders` is recovery's LOCAL
// view — recovery mutated only this copy, never the broker.
struct RecoveryOutcome {
  RecoveryStatus status = RecoveryStatus::Blocked;
  std::vector<domain::Order> orders;
  int unknowns_resolved = 0;    // UNKNOWN orders converged to broker truth.
  int unknowns_unresolved = 0;  // UNKNOWN orders still ambiguous after reconcile.
  bool escalated = false;       // A Critical escalation alert was sent.
  std::string detail;           // Redaction-safe reason (for logs / operators).
  int mismatches = 0;           // Reconcile mismatches (phantom/vanished) — blocks resume.
};

// The crash-recovery state machine. Composes the 3.1 Reconciler/ReconcileApplier
// + the lifecycle FSM + three injected seams, and runs reconcile-before-resume.
//
// SAFETY POSTURE: recover() is READ + RECONCILE ONLY — it never issues a broker
// mutation (place/modify/cancel/square_off). Every fallible step is fail-closed:
// the first blocking condition short-circuits to the correct status. No throw.
class RecoveryCoordinator {
 public:
  // Seam types: replay persisted state, confirm the broker session, run the
  // safe-start gate. All return a Result so a failure is a value, not a throw.
  using LoadStateFn = std::function<Result<std::vector<domain::Order>>()>;
  using CheckSessionFn = std::function<Result<ports::Ok>()>;
  using SafeStartFn = std::function<Result<ports::Ok>()>;

  RecoveryCoordinator(ports::BrokerPort& broker, ports::AlertSink& alerts,
                      const ports::ClockPort& clock, lifecycle::LifecycleEngine& engine,
                      LoadStateFn load_state, CheckSessionFn check_session,
                      SafeStartFn safe_start_check) noexcept
      : broker_(broker),
        alerts_(alerts),
        clock_(clock),
        engine_(engine),
        load_state_(std::move(load_state)),
        check_session_(std::move(check_session)),
        safe_start_check_(std::move(safe_start_check)) {}

  // Run the reconcile-before-resume state machine and return the verdict.
  // Issues NO broker mutation — only Reconciler::fetch (reads) +
  // ReconcileApplier::apply (FSM over the LOCAL orders). See the header banner
  // for the full step-by-step contract.
  [[nodiscard]] RecoveryOutcome recover();

 private:
  ports::BrokerPort& broker_;
  ports::AlertSink& alerts_;
  const ports::ClockPort& clock_;
  lifecycle::LifecycleEngine& engine_;
  LoadStateFn load_state_;
  CheckSessionFn check_session_;
  SafeStartFn safe_start_check_;
};

}  // namespace broker_exec::reconcile
