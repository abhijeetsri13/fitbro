#pragma once

// broker_exec::options — hedge-first option-spread execution (Story 5.1, FR-16).
//
// THE WHOLE POINT IS "NEVER MOMENTARILY NAKED": for a hedged short the protective
// long (the hedge) MUST be live before the naked-risk short is sent. This module
// is the ordered, fail-closed state machine that guarantees it. It is pure
// orchestration over INJECTED SEAMS (no real broker, no transport) so it stays
// broker-neutral and unit-testable; the real wiring to BrokerPort::place /
// reconcile + the basket engine is Story 5.2 / the composition root, not here.
//
// The single load-bearing invariant (AC-1/AC-2): the ONLY path that places the
// short is hedge-PLACED-AND-CONFIRMED. Every failure or ambiguity before that
// aborts the short — no naked window ever opens. Fail-closed: an ERROR while
// confirming (or rechecking) counts as "not safe", NEVER as "proceed
// optimistically".
//
// A LONE HEDGE IS SAFE (AC-2 corollary): if the hedge is live but the short
// placement fails, that is ShortPlacementFailed — a long option with no short
// risk. It is NOT an emergency; no Critical alert, no emergency action. Only a
// LIVE short whose hedge later fails (NakedShortRemediated) triggers AC-3.
//
// BUT THAT COROLLARY NEEDS THE SHORT'S ABSENCE PROVEN. A Timeout/Network/Unknown
// on `place_short` is NOT proof the short does not exist (BrokerPort: "a Timeout
// may mean the order reached the exchange"), and "a lone hedge is safe, keep or
// close it" is a SAFETY CLAIM a caller acts on — closing the hedge over a short
// that IS live opens the naked position this module exists to prevent. So an
// ambiguous short outcome is ShortAmbiguousReconcileRequired, never
// ShortPlacementFailed: hedge LEFT alone, Critical alert, reconcile first.
//
// ALERTING IS BEST-EFFORT (mirrors health::Watchdog): the AlertSink::send Result
// is swallowed so a dead alert channel can never derail or block the safety
// outcome. The emergency action runs even if the alert send returned an Error.
//
// Conventions: no-throw across the boundary (return an outcome, never propagate),
// no double/float, redaction-safe `detail` + alert messages (no tokens/secrets).
// Cross-platform: C++20 standard library only — NO OS APIs, NO `#ifdef`. Depends
// inward only on `ports` (BrokerAck/AlertSink/Ok) and `errors` (Result/Error).

#include <functional>
#include <string>
#include <string_view>

#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::options {

// The terminal outcome of a hedge-first attempt. Stable, log-friendly names (see
// to_string); renaming a returned name is a breaking observability change.
//   HedgedShortLive       — hedge confirmed, short placed, hedge re-verified live:
//                           the ONLY fully-successful terminal.
//   HedgePlacementFailed  — the hedge order could not be placed; aborted BEFORE
//                           any short was sent (AC-2; never naked).
//   HedgeUnconfirmed      — the hedge was placed but not confirmed (or the
//                           confirmation check errored); the short was NEVER sent
//                           (AC-2, fail-closed). The caller decides on the hedge.
//   ShortPlacementFailed  — hedge is live and the short was DEFINITIVELY rejected
//                           (the broker gave a verdict): SAFE (a lone long hedge is
//                           not naked). No remediation; the hedge order id is
//                           returned so the caller can keep/close it.
//   NakedShortRemediated  — the short is live but the hedge could not be proven
//                           live afterwards: AC-3 fired (Critical alert + the
//                           configured emergency action ran).
//   ShortAmbiguousReconcileRequired
//                         — hedge is live and the short's placement outcome is
//                           AMBIGUOUS (ReconcileFirst action, or Timeout/Network/
//                           Unknown category): the short MAY be live and no order id
//                           for it was ever acked. Distinct from ShortPlacementFailed
//                           precisely because that outcome makes a safety claim
//                           ("keep or close the hedge") which is FALSE here. Critical
//                           alert, hedge LEFT in place, emergency action NOT run —
//                           it would remove the hedge. Reconcile before deciding.
enum class HedgeFirstOutcome {
  HedgedShortLive,
  HedgePlacementFailed,
  HedgeUnconfirmed,
  ShortPlacementFailed,
  NakedShortRemediated,
  ShortAmbiguousReconcileRequired
};

// Stable, log/serialization-friendly outcome names (NFR-8 observability contract).
[[nodiscard]] std::string_view to_string(HedgeFirstOutcome outcome) noexcept;

// The result of execute_hedge_first. `detail` is a redaction-safe human/audit note
// (no tokens, no raw broker text — only stable error-category tags). The two order
// ids are populated as far as the state machine got: hedge_order_id once the hedge
// is placed; short_order_id once the short is placed. `emergency_action_ran` is
// true only when the AC-3 emergency seam was present AND returned success.
struct HedgeFirstResult {
  HedgeFirstOutcome outcome;
  std::string hedge_order_id;
  std::string short_order_id;
  bool emergency_action_ran = false;
  std::string detail;
};

// The injected seams that keep the executor broker-neutral and unit-testable with
// NO real broker. Each fallible seam returns a Result; an Error returned by a seam
// is treated as failure of THAT step. A null required seam FAILS CLOSED — it is
// treated as that step failing, never as license to proceed past a missing seam to
// a send.
struct HedgeFirstSeams {
  // Place the protective long hedge. On success its broker_order_id is captured.
  std::function<Result<ports::BrokerAck>()> place_hedge;
  // Confirm the hedge is filled/live. true == confirmed; false / Error / null ==
  // NOT confirmed (fail-closed) and the short is never sent.
  std::function<Result<bool>(const std::string& hedge_order_id)> confirm_hedge;
  // Place the naked-risk short — reached ONLY after the hedge is confirmed. An
  // AMBIGUOUS Error here is NOT a rejection: the short may already be live, so it
  // yields ShortAmbiguousReconcileRequired rather than ShortPlacementFailed.
  std::function<Result<ports::BrokerAck>()> place_short;
  // Post-short re-verification that the hedge is STILL live (AC-3). true == live;
  // false / Error / null == cannot prove live (fail-closed) -> remediation.
  std::function<Result<bool>(const std::string& hedge_order_id)> recheck_hedge_live;
  // The configured remediation (square-off / cancel-all / ...) run when AC-3 fires.
  std::function<Result<ports::Ok>()> emergency_action;
};

// Execute the hedge-first state machine over the injected seams. NO throw: every
// failure path returns a populated HedgeFirstResult rather than propagating. The
// ordered, fail-closed steps (see the file header for the invariants):
//   1. place_hedge   — Error / null seam -> HedgePlacementFailed (short NEVER sent).
//   2. confirm_hedge — Error / false / null seam -> HedgeUnconfirmed (short NEVER
//                      sent; an error checking confirmation counts as NOT confirmed).
//   3. place_short   — null seam / a DEFINITIVE rejection -> ShortPlacementFailed
//                      (hedge stands; SAFE — no Critical alert, no emergency
//                      action). An AMBIGUOUS Error -> ShortAmbiguousReconcileRequired:
//                      the short may be LIVE, so a Critical alert is sent (Result
//                      swallowed), the hedge is LEFT in place and emergency_action is
//                      NOT run — running it would remove the hedge.
//   4. recheck_hedge_live — true -> HedgedShortLive (terminal success). false /
//                      Error / null seam -> AC-3: a Critical alert is sent (Result
//                      swallowed) AND emergency_action runs (its Result swallowed
//                      into emergency_action_ran); outcome NakedShortRemediated. The
//                      emergency action runs even if the alert send returned Error.
[[nodiscard]] HedgeFirstResult execute_hedge_first(const HedgeFirstSeams& seams,
                                                   ports::AlertSink& alerts);

}  // namespace broker_exec::options
