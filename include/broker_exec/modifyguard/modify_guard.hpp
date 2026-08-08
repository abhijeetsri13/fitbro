#pragma once

// broker_exec::modifyguard — the MODIFY-ORDER SAFETY GUARD (a pure decision).
//
// PROBLEM (real Kite developer complaints): modifying an order that has already
// executed or PARTIALLY filled is dangerous. On Kite, the modify `quantity`
// field sets the order's TOTAL quantity, not the still-pending remainder. So a
// quantity modify on a partially-filled order can FLIP the order to CANCELLED —
// cancelling the working remainder — which strands the position or, worse,
// leaves unintended over/under exposure. Two distinct hazards:
//   * QTY-ON-PARTIAL: a quantity modify on a partial re-states the TOTAL; the
//     broker reconciles new-total against already-filled and can cancel the
//     working remainder. Only a PRICE-ONLY modify is safe on a partial.
//   * SHRINK-BELOW-FILLED: a quantity modify whose new TOTAL is at or below what
//     is already filled leaves no room for the pending qty, so the broker flips
//     the order to Cancelled — again cancelling the working remainder.
// A third hazard is timing: a fill can RACE IN between the moment the caller
// decided to modify and the moment the modify is sent. The decision is then
// stale and must be re-evaluated against fresh broker truth, never sent blind.
//
// This module gates a modify against the order's last-reconciled fill state and
// returns a verdict. It is a PURE function: no I/O, no throw, integer
// quantities only (no float), no OS API, no `#ifdef`. It mirrors the sibling
// `broker_exec::modes` pure-decision style (enum + to_string + result struct +
// fail-closed + no-throw). It depends inward only on `domain` (OrderState),
// `errors`, and `ports`.
//
// FAIL-CLOSED is the spine of the design: every defaulted field of ModifyResult
// denies the modify (verdict RejectNotModifiable, allowed=false), so a forgotten
// assignment can never silently ALLOW a modify. Each reject branch is checked in
// a fixed precedence order (terminal -> not-modifiable -> raced fill ->
// qty-on-partial -> shrink-below-filled -> allow) so the most fundamental reason
// always wins and the decision is deterministic.

#include <cstdint>
#include <string>
#include <string_view>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/types.hpp"

namespace broker_exec::modifyguard {

// The guard's verdict vocabulary. Stable, log/serialization-friendly names that
// are part of the observability contract (NFR-8): renames are breaking changes.
//   Allow                   — the modify is safe to send.
//   RejectQtyOnPartial      — a quantity modify on a PARTIALLY-FILLED order would
//                             re-state the TOTAL and can cancel the working
//                             remainder; only a price-only modify is safe.
//   RejectShrinkBelowFilled — the requested new TOTAL is at/below the already-
//                             filled qty; the broker flips it to Cancelled.
//   RejectRacedFill         — a fill landed since the caller decided to modify;
//                             the modify is stale -> reconcile first.
//   RejectTerminal          — the order is already Filled/Cancelled/Rejected;
//                             nothing to modify -> reconcile.
//   RejectNotModifiable     — ambiguous/awaiting-reconcile state (never modify a
//                             non-confirmed order). Also the fail-closed default.
enum class ModifyVerdict {
  Allow,
  RejectQtyOnPartial,
  RejectShrinkBelowFilled,
  RejectRacedFill,
  RejectTerminal,
  RejectNotModifiable
};

// Stable, log/serialization-friendly name (NFR-8 observability contract).
[[nodiscard]] std::string_view to_string(ModifyVerdict verdict) noexcept;

// The order as last reconciled against broker truth — the basis for the guard's
// decision. Integer quantities only (no float in any qty path).
struct OrderModifyState {
  // The order's last-reconciled lifecycle state.
  domain::OrderState state = domain::OrderState::Unknown;
  // Quantity already filled (>= 0).
  std::int64_t filled_qty = 0;
  // The order's current TOTAL quantity (filled + pending).
  std::int64_t total_qty = 0;
};

// The requested modify. Kite semantics: `new_total_qty` is the order's new TOTAL
// quantity, NOT the pending remainder — the load-bearing reason a quantity
// modify on a partial is dangerous.
struct ModifyRequest {
  // The modify alters quantity.
  bool changes_quantity = false;
  // The modify alters ANY price the order works at — the limit, the TRIGGER, or
  // the order type that decides which of them the broker reads. All three are one
  // flag because the guard's rule is the same for all of them: a price-only
  // modify is the safe kind, a quantity modify is the dangerous kind.
  bool changes_price = false;
  // Requested new TOTAL quantity (Kite: `quantity` sets the TOTAL, not pending).
  // Ignored when !changes_quantity.
  std::int64_t new_total_qty = 0;
};

// Derive a ModifyRequest by DIFFING the live intent against the amended one.
// PURE / NO-THROW.
//
// WHY THIS EXISTS (IMP-11). `changes_price` has always been hand-set by the
// caller, and a caller who moved a stop's TRIGGER while leaving the limit alone
// could easily set neither flag — the guard would then evaluate a "modify that
// changes nothing" and Allow it on an order it should have refused. Now that the
// trigger is a real, separate field on OrderIntent, the diff can be computed
// instead of asserted:
//
//   changes_quantity <- quantity differs (new_total_qty = the amended TOTAL)
//   changes_price    <- limit price, TRIGGER price, or order_type differs
//
// order_type counts as a price change because it re-points the broker at a
// different number (SL-M -> SL starts honouring a limit that was previously
// ignored). Callers that already compute the flags themselves are unaffected —
// this is additive.
[[nodiscard]] ModifyRequest make_modify_request(const domain::OrderIntent& current,
                                                const domain::OrderIntent& amended) noexcept;

// The guard's decision. DEFAULTS ARE FAIL-CLOSED: an unassigned result denies
// the modify (RejectNotModifiable, allowed=false), so a forgotten assignment on
// any code path can never permit a modify. `allowed` is the single boolean the
// caller gates on; `verdict` explains WHY; `detail` is a redaction-safe,
// human-readable summary (verdict + order state only — never any secret).
struct ModifyResult {
  ModifyVerdict verdict = ModifyVerdict::RejectNotModifiable;
  bool allowed = false;
  std::string detail;
};

// Evaluate whether a modify is safe to send. PURE / NO-THROW.
//
// `observed_filled_qty` is the fill qty the caller saw at the moment it DECIDED
// to modify. If `cur.filled_qty > observed_filled_qty`, a fill RACED IN since the
// decision: the modify is stale and must reconcile first (RejectRacedFill).
//
// Precedence (fail-closed, first match wins):
//   1. TERMINAL          — Filled/Cancelled/Rejected -> RejectTerminal.
//   2. NOT-MODIFIABLE     — Unknown/ManualInterventionRequired/Reconciled/
//                          PartiallyPlaced -> RejectNotModifiable.
//   3. RACED FILL         — cur.filled_qty > observed_filled_qty
//                          -> RejectRacedFill.
//   4. QTY-ON-PARTIAL     — changes_quantity && state==PartiallyFilled
//                          -> RejectQtyOnPartial (price-only on a partial is OK).
//   5. SHRINK-BELOW-FILLED— changes_quantity && new_total_qty <= cur.filled_qty
//                          -> RejectShrinkBelowFilled.
//   6. Otherwise          — Allow (price-only, or a qty grow above filled on a
//                          non-partial working order).
[[nodiscard]] ModifyResult evaluate_modify(const OrderModifyState& cur, const ModifyRequest& req,
                                           std::int64_t observed_filled_qty);

}  // namespace broker_exec::modifyguard
