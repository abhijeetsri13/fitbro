#include "broker_exec/lifecycle/lifecycle.hpp"

#include "broker_exec/idempotency/idempotency.hpp"

namespace broker_exec::lifecycle {

namespace {

using domain::OrderState;

// ── The transition table ────────────────────────────────────────────────────
//
// is_valid_transition(from, to) is true iff `to` is a documented successor of
// `from`. A self-transition (from == to) is always legal (the NoChange case).
// Terminal states (Filled/Rejected/Cancelled) are ABSORBING — they have no
// outgoing transitions; apply() also short-circuits them before this is ever
// consulted, but the table encodes the same fact for is_valid_transition's
// own callers (Story 1.9 dispatch).
//
// Forward-progressing happy path (matches domain/enums.hpp):
//   Created -> Validated -> PendingSend -> Sent -> Acknowledged ->
//   PartiallyFilled -> Filled
//
// Resilience edges (Epics 1/3):
//   * From any non-terminal state the order may go to Unknown (a send/observe
//     result is uncertain — reconcile, never blindly retry; FR-10).
//   * From Unknown the reconciler may resolve to any live or terminal state, or
//     escalate to ManualInterventionRequired (a double fault — needs a human).
//   * Reconciled is the "confirmed-against-broker-truth" marker reachable from
//     the live states and from Unknown; from it the order may continue to any
//     live/terminal state.
//   * PartiallyPlaced is the sliced-parent posture (children not all placed,
//     possibly an Unknown child); it may progress to PartiallyFilled/Filled or,
//     on a refused/ambiguous slice, to ManualInterventionRequired.
//   * ManualInterventionRequired is a near-sink: only an operator-driven path
//     (modeled as a reconcile) may move it, so it transitions only to
//     Reconciled or to a terminal Cancelled.
//
// The switch is TOTAL (no default): adding an OrderState fails to compile under
// -Werror=switch until its row is filled in.
[[nodiscard]] bool table_allows(OrderState from, OrderState to) noexcept {
  switch (from) {
    case OrderState::Created:
      return to == OrderState::Validated || to == OrderState::Rejected || to == OrderState::Unknown;
    case OrderState::Validated:
      return to == OrderState::PendingSend || to == OrderState::Rejected ||
             to == OrderState::Unknown;
    case OrderState::PendingSend:
      return to == OrderState::Sent || to == OrderState::Rejected || to == OrderState::Unknown;
    case OrderState::Sent:
      return to == OrderState::Acknowledged || to == OrderState::PartiallyFilled ||
             to == OrderState::Filled || to == OrderState::Rejected ||
             to == OrderState::Cancelled || to == OrderState::Unknown;
    case OrderState::Acknowledged:
      return to == OrderState::PartiallyFilled || to == OrderState::Filled ||
             to == OrderState::Rejected || to == OrderState::Cancelled || to == OrderState::Unknown;
    case OrderState::PartiallyFilled:
      return to == OrderState::PartiallyFilled || to == OrderState::Filled ||
             to == OrderState::Cancelled || to == OrderState::Rejected || to == OrderState::Unknown;
    case OrderState::Filled:
    case OrderState::Rejected:
    case OrderState::Cancelled:
      // Terminal / absorbing: no outgoing transitions.
      return false;
    case OrderState::Unknown:
      // The reconciler resolves an Unknown to broker truth — any live or
      // terminal state, or escalation to manual intervention.
      return to == OrderState::Acknowledged || to == OrderState::PartiallyFilled ||
             to == OrderState::Filled || to == OrderState::Rejected ||
             to == OrderState::Cancelled || to == OrderState::Reconciled ||
             to == OrderState::PartiallyPlaced || to == OrderState::ManualInterventionRequired;
    case OrderState::Reconciled:
      // Confirmed against broker truth; may continue to any live/terminal state.
      return to == OrderState::Acknowledged || to == OrderState::PartiallyFilled ||
             to == OrderState::Filled || to == OrderState::Rejected ||
             to == OrderState::Cancelled || to == OrderState::Unknown ||
             to == OrderState::ManualInterventionRequired;
    case OrderState::PartiallyPlaced:
      // Sliced-parent posture; progresses as children fill, or escalates.
      return to == OrderState::PartiallyPlaced || to == OrderState::PartiallyFilled ||
             to == OrderState::Filled || to == OrderState::Cancelled ||
             to == OrderState::Rejected || to == OrderState::Unknown ||
             to == OrderState::ManualInterventionRequired;
    case OrderState::ManualInterventionRequired:
      // Near-sink: only an operator/reconcile path moves it.
      return to == OrderState::Reconciled || to == OrderState::Cancelled;
  }
  return false;
}

// ── Canonicalizing an adapter's "still working at the broker" view ──────────
//
// `Sent` means one thing in THIS machine — "we sent it; no acknowledgement has
// been processed yet" — and something else in an adapter. The Kite adapter maps
// EVERY working broker status (OPEN, TRIGGER PENDING, OPEN PENDING, MODIFY
// PENDING, ...) onto OrderState::Sent and publishes it carrying the row's
// filled_quantity. Read literally, that view is a BACKWARD jump out of
// Acknowledged / PartiallyFilled / Unknown / Reconciled: the table refuses it and
// apply() returns before writing anything, so for the whole working life of every
// Kite order the fill, the average price and the broker order id the view carried
// were discarded in silence. That is broker truth — which this library declares
// the source of truth — thrown away on the primary live broker's normal path, and
// a locally-frozen filled_qty==0 then walks straight past modifyguard's
// shrink-below-filled check and cancels the working remainder of a real position.
//
// So an observed `Sent` arriving at an order that has ALREADY reached the broker
// is not a state transition at all: it says "still working", and the only new
// fact in it is the fill count. Canonicalize it exactly the way
// fillnorm::normalize_fill does for a working row — which is exactly how the
// Kotak adapter already publishes one, and why Kotak never had this defect:
//   filled > 0 -> PartiallyFilled, otherwise Acknowledged.
// Both are legal successors of every from-state below, so the transition table is
// left untouched: no backward edge is punched through the forward-progressing
// invariant, and the order never lands in `Sent` — a state that boot's
// require_no_unreconciled_orders reads as "send result never confirmed" and that
// modifyguard reads as "never modifiable", neither of which is true of an order
// the broker has just confirmed working.
//
// NEVER Filled: the broker is saying the order is still WORKING. Concluding a
// terminal, absorbing sink from our own arithmetic is the one mistake here that
// cannot be walked back.
//
// From which states does an observed `Sent` mean "still working" rather than a
// forward step? Exactly those in which the order is known to have reached the
// broker — or, for Unknown, in which a broker snapshot listing it as working is
// itself the proof that it did. The exclusions are as deliberate as the
// inclusions: from PendingSend and earlier, `Sent` is a genuine FORWARD edge on
// the documented happy path (PendingSend -> Sent) and must keep meaning that;
// ManualInterventionRequired is a near-sink only a human clears, so laundering it
// into Acknowledged would be fail-OPEN; PartiallyPlaced is a parent fold with no
// broker order of its own. Terminal states never reach here (apply() drops them
// first). The switch is TOTAL for the same reason table_allows' is.
[[nodiscard]] bool sent_means_still_working(OrderState from) noexcept {
  switch (from) {
    case OrderState::Sent:
    case OrderState::Acknowledged:
    case OrderState::PartiallyFilled:
    case OrderState::Unknown:
    case OrderState::Reconciled:
      return true;
    case OrderState::Created:
    case OrderState::Validated:
    case OrderState::PendingSend:
    case OrderState::Filled:
    case OrderState::Rejected:
    case OrderState::Cancelled:
    case OrderState::PartiallyPlaced:
    case OrderState::ManualInterventionRequired:
      return false;
  }
  return false;
}

// The observed state the transition table should actually be asked about.
[[nodiscard]] OrderState canonical_observed(OrderState from, const BrokerView& view) noexcept {
  if (view.observed_state != OrderState::Sent || !sent_means_still_working(from)) {
    return view.observed_state;
  }
  return view.filled_qty.value() > 0 ? OrderState::PartiallyFilled : OrderState::Acknowledged;
}

// Fold a child_ref -> state map into the parent state (collects the values and
// defers to fold_parent_state). A free helper so apply_child() and
// parent_state() share one definition of "the parent is the fold of its kids".
[[nodiscard]] OrderState fold_children_map(
    const std::unordered_map<std::string, OrderState>& children) {
  std::vector<OrderState> states;
  states.reserve(children.size());
  for (const auto& entry : children) {
    states.push_back(entry.second);
  }
  return fold_parent_state(states);
}

}  // namespace

std::string_view to_string(ApplyOutcome outcome) noexcept {
  switch (outcome) {
    case ApplyOutcome::Applied:
      return "APPLIED";
    case ApplyOutcome::DroppedStale:
      return "DROPPED_STALE";
    case ApplyOutcome::DroppedTerminal:
      return "DROPPED_TERMINAL";
    case ApplyOutcome::NoChange:
      return "NO_CHANGE";
  }
  return "NO_CHANGE";
}

bool is_terminal(OrderState state) noexcept {
  switch (state) {
    case OrderState::Filled:
    case OrderState::Rejected:
    case OrderState::Cancelled:
      return true;
    case OrderState::Created:
    case OrderState::Validated:
    case OrderState::PendingSend:
    case OrderState::Sent:
    case OrderState::Acknowledged:
    case OrderState::PartiallyFilled:
    case OrderState::Unknown:
    case OrderState::Reconciled:
    case OrderState::PartiallyPlaced:
    case OrderState::ManualInterventionRequired:
      return false;
  }
  return false;
}

bool is_valid_transition(OrderState from, OrderState to) noexcept {
  if (from == to) {
    // A self-transition is the NoChange case and is always legal — except out of
    // a terminal state, which is absorbing and cannot even "stay" via a new view
    // semantically; but a terminal == terminal compare is benign here (apply()
    // guards terminals first), so treat equality as legal uniformly.
    return true;
  }
  return table_allows(from, to);
}

OrderState fold_parent_state(const std::vector<OrderState>& children) {
  // No slices registered yet -> the base posture for a fresh parent.
  if (children.empty()) {
    return OrderState::Created;
  }

  bool any_unknown = false;
  bool any_rejected = false;
  bool all_filled = true;
  bool all_cancelled = true;
  bool all_terminal = true;

  for (const OrderState child : children) {
    if (child == OrderState::Unknown) {
      any_unknown = true;
    }
    if (child == OrderState::Rejected) {
      any_rejected = true;
    }
    if (child != OrderState::Filled) {
      all_filled = false;
    }
    if (child != OrderState::Cancelled) {
      all_cancelled = false;
    }
    if (!is_terminal(child)) {
      all_terminal = false;
    }
  }

  // Precedence is deliberate and documented:
  //   1. ANY Unknown wins -> PartiallyPlaced ("unknown present"): the parent
  //      must pause on the ambiguity before anything else is concluded (FR-10).
  if (any_unknown) {
    return OrderState::PartiallyPlaced;
  }
  //   2. ANY Rejected (with no Unknown) -> ManualInterventionRequired: a slice
  //      was refused; the parent is partial-and-broken and needs a human.
  if (any_rejected) {
    return OrderState::ManualInterventionRequired;
  }
  //   3. All Filled -> Filled.
  if (all_filled) {
    return OrderState::Filled;
  }
  //   4. All Cancelled -> Cancelled.
  if (all_cancelled) {
    return OrderState::Cancelled;
  }
  //   5. All terminal but mixed (some Filled, some Cancelled) -> PartiallyFilled.
  if (all_terminal) {
    return OrderState::PartiallyFilled;
  }
  //   6. Otherwise at least one child is still active / in-flight -> placement
  //      across the slices is still in progress.
  return OrderState::PartiallyPlaced;
}

ApplyOutcome LifecycleEngine::apply(domain::Order& order, const BrokerView& view) {
  // Rule 1: terminal-absorbing. Once the order is in a sink, NOTHING moves it —
  // not even a numerically-newer view. Drop and report for logging.
  if (is_terminal(order.state)) {
    return ApplyOutcome::DroppedTerminal;
  }

  // Rule 2: forward-progressing. A view older than (or, for a strict <, behind)
  // the last applied key is a stale / duplicate observation. Equal or higher
  // keys are admitted (an equal key re-applies idempotently -> NoChange below).
  if (const auto it = last_key_.find(view.client_ref); it != last_key_.end()) {
    if (view.ordering_key < it->second) {
      return ApplyOutcome::DroppedStale;
    }
  }

  // The observed state, CANONICALIZED first: an adapter's "still working at the
  // broker" spelling of `Sent` is resolved against the fill count the same view
  // carries before the table is asked to judge it (see canonical_observed).
  // Everything below uses this value and never view.observed_state directly, or
  // the order would be written with a state the table was never asked about.
  const OrderState observed = canonical_observed(order.state, view);

  // Rule 3: legal transitions only. An illegal jump is REFUSED — the order is
  // left untouched AND the ordering key is deliberately NOT advanced. The key is
  // the high-water mark of what was APPLIED, not of what was seen: raising it on
  // a view the machine never believed would make a later, lower-keyed view that
  // IS legal — and that carries real fill data — get dropped as stale, so one
  // refusal would silently cost us the next good observation as well.
  //
  // KNOWN GAP (deliberate, not an oversight): this returns the same NoChange a
  // benign unchanged view returns, so a caller cannot distinguish "nothing to do"
  // from "the broker is telling us something this machine refuses to believe" —
  // a first-class divergence that should count as a mismatch and block new
  // entries. Separating them needs a new ApplyOutcome value, which every switch
  // over the enum must then handle; see the note on ApplyOutcome in the header.
  if (!is_valid_transition(order.state, observed)) {
    return ApplyOutcome::NoChange;
  }

  // A legal, current view that does not change the state is a NoChange. We
  // still record progress (fill qty / avg price can refine within the same
  // state, e.g. successive PartiallyFilled views) — but only update the
  // mutating fields when the state actually advances, to keep "NoChange"
  // honest about the state field. Fill progress within the same state IS
  // applied (it is real new information) and reported as Applied.
  const bool state_changes = order.state != observed;
  const bool fill_advances =
      view.filled_qty != order.filled_qty || view.avg_price != order.avg_price ||
      (!view.broker_order_id.empty() && view.broker_order_id != order.broker_order_id);

  last_key_[view.client_ref] = view.ordering_key;

  if (!state_changes && !fill_advances) {
    return ApplyOutcome::NoChange;
  }

  order.state = observed;
  order.filled_qty = view.filled_qty;
  order.avg_price = view.avg_price;
  if (!view.broker_order_id.empty()) {
    order.broker_order_id = view.broker_order_id;
  }
  return ApplyOutcome::Applied;
}

OrderState LifecycleEngine::apply_child(std::string_view parent_client_ref,
                                        std::string_view child_client_ref, OrderState child_state) {
  // Recover the true parent from the child ref when it is a well-formed
  // "<parent>#<k>" slice ref (Story 1.7 model), so a caller cannot accidentally
  // file a child under the wrong parent. If the child ref is not a slice ref we
  // fall back to the caller-supplied parent (e.g. a synthetic test ref). Either
  // way the parent key is the bucket the fold is computed over.
  std::string parent_key;
  if (idempotency::is_child_ref(child_client_ref)) {
    parent_key = idempotency::parent_of(child_client_ref);
  }
  if (parent_key.empty()) {
    parent_key = std::string(parent_client_ref);
  }

  auto& children = children_by_parent_[parent_key];
  children[std::string(child_client_ref)] = child_state;
  return fold_children_map(children);
}

std::optional<std::int64_t> LifecycleEngine::last_key(std::string_view client_ref) const {
  if (const auto it = last_key_.find(std::string(client_ref)); it != last_key_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<OrderState> LifecycleEngine::parent_state(std::string_view parent_client_ref) const {
  const auto it = children_by_parent_.find(std::string(parent_client_ref));
  if (it == children_by_parent_.end()) {
    return std::nullopt;
  }
  return fold_children_map(it->second);
}

}  // namespace broker_exec::lifecycle
