#pragma once

// broker_exec::options — basket / multi-leg execution (Story 5.2, FR-17).
//
// THE WHOLE POINT IS "NO ORPHANED LEGS": a multi-leg trade is ONE logical unit.
// A dependent leg is NEVER sent while a prerequisite leg failed or was skipped,
// and a mid-basket failure either UNWINDS the already-executed legs or LEAVES
// them in place with a loud alert — per the configured policy. Like the 5.1
// hedge-first executor, this is pure, fail-closed orchestration over INJECTED
// SEAMS (no real broker, no transport) so it stays broker-neutral and
// unit-testable; the real wiring to BrokerPort::place / cancel / square_off +
// reconciliation + per-leg slicing (5.3) is later, not here.
//
// THE LOAD-BEARING INVARIANTS:
//   * PRE-FLIGHT FAIL-CLOSED (AC-1): an ill-formed basket — empty/duplicate
//     leg_id, a depends_on naming an unknown leg, ANY dependency cycle, or a
//     missing place_leg seam — places NOTHING. It is rejected as `Blocked`
//     BEFORE a single order is sent. Never a half-basket. Dependency order +
//     cycle detection are one and the same Kahn topological sort.
//   * NEVER ORPHAN (AC-1): in topological order a leg is placed ONLY if EVERY
//     prerequisite ended `Executed` — a failed, skipped OR ambiguous prerequisite
//     makes this leg `SkippedUnmetDependency` and `place_leg` is NOT called, and
//     the skip then propagates transitively to its own dependents.
//   * UNWIND IS BEST-EFFORT BUT LOUD (AC-2): on a partial basket under
//     `UnwindExecuted`, executed legs are unwound newest-first. A null/Error
//     unwind leaves that leg `Executed` (NOT `Unwound`) and appends a
//     Critical-severity `detail` — the FAILURE TO UNWIND a live leg is always
//     VISIBLE, never silently dropped.
//   * AMBIGUITY IS NOT REJECTION (the naked-position guard): a `place_leg` Error
//     is CLASSIFIED, never assumed definitive. A ReconcileFirst action or a
//     Timeout/Network/Unknown category means the order MAY have reached the
//     exchange (BrokerPort: "a Timeout may mean the order reached the exchange"),
//     so that leg is `AmbiguousMayBeLive`, NOT `Failed`. Such a leg SUPPRESSES the
//     unwind under EVERY policy and forces `ReconcileRequired`: cancelling the
//     protective legs while a risk leg's existence is unproven is the one move
//     that MANUFACTURES a live naked position.
//
// ALERTING IS BEST-EFFORT (mirrors 5.1 / health::Watchdog): the remediation
// (the unwinds) runs FIRST; the AlertSink::send Result is then swallowed AND the
// call is wrapped in try/catch so a dead or THROWING alert channel can never
// derail remediation or break this function's no-throw contract.
//
// Conventions: no-throw across the boundary (return an outcome, never propagate),
// no double/float, redaction-safe `detail` + alert messages (only non-secret
// leg_ids / order_ids and stable error-category tags — never raw broker text).
// Cross-platform: C++20 standard library only — NO OS APIs, NO `#ifdef`. Depends
// inward only on `ports` (BrokerAck/AlertSink/Ok) and `errors` (Result/Error).

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::options {

// One leg of a basket. `leg_id` must be non-empty and unique within the basket.
// `depends_on` lists the leg_ids that MUST be `Executed` before this leg may be
// sent; an empty list marks an independent / root leg.
struct BasketLeg {
  std::string leg_id;
  std::vector<std::string> depends_on;
};

// What to do with the already-executed legs when ANY leg of the basket fails or
// is skipped (AC-2):
//   UnwindExecuted — cancel/exit every already-executed leg (atomic-ish basket).
//   LeaveAndAlert  — leave the executed legs in place but raise an alert.
enum class LegFailurePolicy { UnwindExecuted, LeaveAndAlert };

// Static configuration of a basket attempt.
//   on_leg_failure       — the partial-failure policy above (default: unwind).
//   track_as_single_unit — AC-3: true (default) = the basket is ONE logical
//                          trade keyed by `basket_id`; false = the caller opted
//                          each leg out to independent tracking.
//   basket_id            — the one logical-trade handle the caller persists /
//                          reconciles against. If empty, the executor fills a
//                          DETERMINISTIC fallback (never random / time-based).
struct BasketConfig {
  LegFailurePolicy on_leg_failure = LegFailurePolicy::UnwindExecuted;
  bool track_as_single_unit = true;
  std::string basket_id;
};

// Terminal status of a single leg. Stable, log-friendly names (see to_string):
//   Executed              — placed and acknowledged by the broker seam.
//   Failed                — `place_leg` returned a DEFINITIVE rejection (the
//                           broker gave a verdict): the order does NOT exist.
//   SkippedUnmetDependency— a prerequisite did not end Executed, so this leg was
//                           NEVER sent (the never-orphan invariant).
//   Unwound               — was Executed, then cancelled/exited during
//                           remediation under `UnwindExecuted`.
//   AmbiguousMayBeLive    — `place_leg` returned an AMBIGUOUS Error (ReconcileFirst
//                           action, or Timeout/Network/Unknown category): the order
//                           MAY be LIVE at the exchange and NO order_id was ever
//                           acked, so this library cannot cancel it. It is NOT a
//                           rejection; only reconciliation can settle it.
enum class LegStatus { Executed, Failed, SkippedUnmetDependency, Unwound, AmbiguousMayBeLive };

// Stable, log/serialization-friendly leg-status names (NFR-8 observability).
[[nodiscard]] std::string_view to_string(LegStatus status) noexcept;

// Per-leg result. `order_id` is the broker order id once the leg is Executed
// (empty otherwise). `detail` is a redaction-safe note (leg_ids / order_ids /
// stable error tags only) — for a leg whose unwind failed it carries the
// Critical escalation so the live-orphan risk is never hidden.
struct LegResult {
  std::string leg_id;
  LegStatus status;
  std::string order_id;
  std::string detail;
};

// Terminal outcome of the whole basket. Stable, log-friendly names (to_string):
//   Complete                 — every leg Executed; no alert, no unwind.
//   PartiallyExecutedUnwound — >=1 leg DEFINITIVELY failed/skipped; executed legs
//                              were unwound per `UnwindExecuted` (Critical alert).
//   PartiallyExecutedLeft    — >=1 leg DEFINITIVELY failed/skipped; executed legs
//                              LEFT in place per `LeaveAndAlert` (Warning alert).
//   Blocked                  — invalid basket (cyclic/unknown dependency,
//                              dup/empty leg_id, or null place_leg seam):
//                              NOTHING placed (fail-closed pre-flight).
//   ReconcileRequired        — >=1 leg is `AmbiguousMayBeLive`. This OUTRANKS both
//                              partials: NO leg was unwound under EITHER policy,
//                              and ONE Critical alert names the ambiguous legs. The
//                              basket's true state at the broker is unknown —
//                              reconcile it before cancelling anything.
enum class BasketOutcome {
  Complete,
  PartiallyExecutedUnwound,
  PartiallyExecutedLeft,
  Blocked,
  ReconcileRequired
};

// Stable, log/serialization-friendly outcome names (NFR-8 observability).
[[nodiscard]] std::string_view to_string(BasketOutcome outcome) noexcept;

// The aggregate result. `basket_id` is config's id or the deterministic fallback;
// `tracked_as_single_unit` echoes config (AC-3). `legs` reports every leg in
// input order. `detail` is a redaction-safe summary of the outcome.
struct BasketResult {
  BasketOutcome outcome;
  std::string basket_id;
  std::vector<LegResult> legs;
  bool tracked_as_single_unit;
  std::string detail;
};

// The injected seams that keep the executor broker-neutral and unit-testable
// with NO real broker. `place_leg` is REQUIRED: a null place_leg fails the
// basket closed (`Blocked`) — never a partial placement with a missing placer.
// `unwind_leg` is best-effort during remediation: a null seam OR an Error leaves
// the leg Executed-not-Unwound and is escalated (it is never silently dropped).
// It is NOT called AT ALL when any leg ended `AmbiguousMayBeLive` — see
// `ReconcileRequired` above for why unwinding on ambiguity is the dangerous move.
struct BasketSeams {
  // Place one leg; on success its broker_order_id is captured as the leg order id.
  std::function<Result<ports::BrokerAck>(const BasketLeg&)> place_leg;
  // Cancel/exit an already-executed leg by its broker order id (remediation).
  std::function<Result<ports::Ok>(const std::string& leg_order_id)> unwind_leg;
};

// Execute the basket over the injected seams. NO throw: every path returns a
// populated BasketResult rather than propagating. The fail-closed steps (see the
// file header for the invariants):
//   1. PRE-FLIGHT (before ANY placement) — reject empty/duplicate leg_id, an
//      unknown depends_on, a dependency cycle, or a null place_leg seam =>
//      `Blocked`, nothing placed. Kahn's topological sort both detects cycles
//      and yields the execution order.
//   2. EXECUTE in topological order — a leg is placed ONLY if every prerequisite
//      is `Executed`; otherwise `SkippedUnmetDependency` (place_leg NOT called),
//      and the skip propagates to dependents. A `place_leg` Error is CLASSIFIED:
//      a definitive rejection => `Failed`; an ambiguous one (ReconcileFirst /
//      Timeout / Network / Unknown) => `AmbiguousMayBeLive`.
//   3. TERMINAL + POLICY — ANY `AmbiguousMayBeLive` leg WINS: NO unwind runs
//      under either policy and ONE Critical alert NAMES the ambiguous legs =>
//      `ReconcileRequired`. Otherwise, if any leg is Failed/Skipped:
//      `UnwindExecuted` unwinds executed legs newest-first (null/Error unwind
//      leaves the leg Executed + Critical detail) then sends ONE Critical alert =>
//      `PartiallyExecutedUnwound`; `LeaveAndAlert` leaves them and sends ONE
//      Warning alert => `PartiallyExecutedLeft`. All Executed => `Complete`, no
//      alert. The alert Result is swallowed and wrapped in try/catch.
[[nodiscard]] BasketResult execute_basket(const std::vector<BasketLeg>& legs,
                                          const BasketConfig& config, const BasketSeams& seams,
                                          ports::AlertSink& alerts);

}  // namespace broker_exec::options
