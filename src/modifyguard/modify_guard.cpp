#include "broker_exec/modifyguard/modify_guard.hpp"

#include <string>

#include "broker_exec/domain/enums.hpp"

namespace broker_exec::modifyguard {

namespace {

// Build a redaction-safe `detail`: the verdict name + the order's state name
// only. Neither is a secret (both are part of the stable observability
// vocabulary), so this is always safe to log. No qty values are echoed — the
// state name is enough to explain the decision without leaking position size.
[[nodiscard]] std::string detail_for(ModifyVerdict verdict, domain::OrderState state) {
  return std::string(to_string(verdict)) + " [state=" + std::string(domain::to_string(state)) + "]";
}

// Construct a result for a given verdict + the order state, deriving `allowed`
// from the verdict so the two can never disagree: ONLY Allow is permissive.
[[nodiscard]] ModifyResult make_result(ModifyVerdict verdict, domain::OrderState state) {
  ModifyResult result;  // fail-closed defaults (RejectNotModifiable / !allowed).
  result.verdict = verdict;
  result.allowed = (verdict == ModifyVerdict::Allow);
  result.detail = detail_for(verdict, state);
  return result;
}

// A terminal-absorbing state: there is nothing left to modify; the caller must
// reconcile rather than touch the order.
[[nodiscard]] bool is_terminal(domain::OrderState state) noexcept {
  switch (state) {
    case domain::OrderState::Filled:
    case domain::OrderState::Cancelled:
    case domain::OrderState::Rejected:
      return true;
    default:
      return false;
  }
}

// A state in which the order is NOT a broker-confirmed working order. We never
// modify one: the true broker state is unknown/unconfirmed, so the locally-held
// filled_qty is unreliable and any modify is a blind action that could race with a
// fill or a reconciliation. This covers BOTH the ambiguous/awaiting-reconcile
// states AND the pre-acknowledgement / in-flight states (Created/Validated/
// PendingSend/Sent) — a `Sent` order is at the broker with no processed ack, so a
// qty modify against a stale filled_qty==0 could set a TOTAL below the true fill
// and cancel the working remainder. Only Acknowledged / PartiallyFilled (a
// confirmed working order) proceed past here. Fail closed -> RejectNotModifiable.
[[nodiscard]] bool is_not_modifiable_base(domain::OrderState state) noexcept {
  switch (state) {
    case domain::OrderState::Created:
    case domain::OrderState::Validated:
    case domain::OrderState::PendingSend:
    case domain::OrderState::Sent:
    case domain::OrderState::Unknown:
    case domain::OrderState::ManualInterventionRequired:
    case domain::OrderState::Reconciled:
    case domain::OrderState::PartiallyPlaced:
      return true;
    default:
      return false;
  }
}

}  // namespace

std::string_view to_string(ModifyVerdict verdict) noexcept {
  switch (verdict) {
    case ModifyVerdict::Allow:
      return "allow";
    case ModifyVerdict::RejectQtyOnPartial:
      return "reject_qty_on_partial";
    case ModifyVerdict::RejectShrinkBelowFilled:
      return "reject_shrink_below_filled";
    case ModifyVerdict::RejectRacedFill:
      return "reject_raced_fill";
    case ModifyVerdict::RejectTerminal:
      return "reject_terminal";
    case ModifyVerdict::RejectNotModifiable:
      return "reject_not_modifiable";
  }
  return "unknown";
}

ModifyResult evaluate_modify(const OrderModifyState& cur, const ModifyRequest& req,
                             std::int64_t observed_filled_qty) {
  // The checks run in a FIXED precedence order; the first match wins so the most
  // fundamental reason to deny always dominates and the decision is deterministic.

  // 1. TERMINAL: already Filled/Cancelled/Rejected — nothing to modify; reconcile.
  if (is_terminal(cur.state)) {
    return make_result(ModifyVerdict::RejectTerminal, cur.state);
  }

  // 2. NOT-MODIFIABLE base: ambiguous / awaiting-reconcile — never modify a
  //    non-confirmed order.
  if (is_not_modifiable_base(cur.state)) {
    return make_result(ModifyVerdict::RejectNotModifiable, cur.state);
  }

  // 3. RACED FILL: a fill landed since the caller DECIDED to modify
  //    (cur.filled_qty > observed_filled_qty). The modify is stale and must be
  //    re-evaluated against fresh broker truth — reconcile first, never send blind.
  if (cur.filled_qty > observed_filled_qty) {
    return make_result(ModifyVerdict::RejectRacedFill, cur.state);
  }

  // 4. QTY MODIFY ON A PARTIAL: on Kite a quantity modify sets the order's TOTAL,
  //    not the pending remainder, so applying it to a PartiallyFilled order can
  //    flip the order to Cancelled and CANCEL THE WORKING REMAINDER. Only a
  //    price-only modify is safe on a partial; a price-only modify (changes_price
  //    && !changes_quantity) falls through to Allow below.
  if (req.changes_quantity && cur.state == domain::OrderState::PartiallyFilled) {
    return make_result(ModifyVerdict::RejectQtyOnPartial, cur.state);
  }

  // 5. SHRINK BELOW FILLED: a quantity modify whose new TOTAL is at or below what
  //    is already filled leaves no room for the pending qty, so the broker flips
  //    the order to Cancelled — again cancelling the working remainder. Applies to
  //    any state where a qty modify is otherwise considered (non-partial here,
  //    since the partial case was already rejected in step 4).
  if (req.changes_quantity && req.new_total_qty <= cur.filled_qty) {
    return make_result(ModifyVerdict::RejectShrinkBelowFilled, cur.state);
  }

  // 6. Otherwise SAFE: a price-only modify, or a quantity modify that GROWS the
  //    total above the already-filled qty on a non-partial working order.
  return make_result(ModifyVerdict::Allow, cur.state);
}

}  // namespace broker_exec::modifyguard
