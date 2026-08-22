#pragma once

// broker_exec::lifecycle — the order lifecycle state machine (Story 1.8, FR-9).
//
// WHAT THIS IS: the single authority that owns each order's OrderState. Broker
// truth arrives from two sources — an asynchronous push update and a reconciler
// snapshot (Story 3.1) — and those two streams can deliver views out of order,
// duplicated, or stale. This module makes applying them safe by three rules:
//
//   1. TERMINAL-ABSORBING. Filled / Rejected / Cancelled are sinks. Once an
//      order is terminal nothing moves it out — a later view (even a "fresher"
//      one) is dropped and reported so the caller can log it (FR-9).
//   2. FORWARD-PROGRESSING (apply-ordering). Every BrokerView carries a
//      monotonic `ordering_key` (the broker/exchange update sequence). A view is
//      applied only if its key is >= the last key already applied for that
//      order; a lower key is a stale/duplicate view and is dropped.
//   3. LEGAL TRANSITIONS ONLY. State may only move along the documented
//      transition table (see is_valid_transition); an illegal jump is refused.
//
// PARENT/CHILD MODEL: a sliced parent order (freeze-slicer, Story 2.9) has
// children "<parent>#<k>". The parent has no broker state of its own — it is a
// fold over its children's states (see fold_parent_state). Any child Unknown
// pulls the parent to PartiallyPlaced (the "unknown present" posture) so the
// engine pauses new risky entries until the ambiguity is reconciled (FR-10).
//
// SINGLE WRITER (NFR-2): the FSM is applied ONLY by the main loop. The decision
// core is synchronous and single-threaded; the reconciler fetches off-loop but
// the main loop is the sole applier of diffs. LifecycleEngine is therefore NOT
// thread-safe by design and must never be touched off the main loop.
//
// CROSS-PLATFORM: C++20 standard library only (<unordered_map>, <vector>,
// <optional>, <cstdint>, <string>, <string_view>). No OS APIs, no `#ifdef`, no
// floating point.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"

namespace broker_exec::lifecycle {

// A broker's view of a single order, from a push update or a reconciler
// snapshot. The `ordering_key` is the engine's apply-ordering discriminator:
// a monotonic broker/exchange update sequence where a higher value is newer.
// Two views with the same key are the same observation (idempotent re-apply).
struct BrokerView {
  std::string client_ref;       // The order's client-ref (Story 1.7).
  std::string broker_order_id;  // Broker-assigned id; empty until acknowledged.
  domain::OrderState observed_state{domain::OrderState::Unknown};
  domain::Quantity filled_qty;   // Cumulative filled quantity in this view.
  domain::Price avg_price;       // Volume-weighted average fill price in this view.
  std::int64_t ordering_key{0};  // Monotonic update sequence; higher = newer.
};

// What apply() did, for logging / metrics. Exactly one is returned per call.
enum class ApplyOutcome {
  Applied,          // The view advanced the order; `order` was mutated.
  DroppedStale,     // view.ordering_key < last applied key — an older/duplicate view.
  DroppedTerminal,  // The order is already terminal (absorbing) — view ignored.
  NoChange          // The view is legal and current but does not change the state.
};

// Stable, log-friendly name for an ApplyOutcome (observability contract, NFR-8).
[[nodiscard]] std::string_view to_string(ApplyOutcome outcome) noexcept;

// True iff `state` is terminal/absorbing: Filled, Rejected, or Cancelled. An
// order in a terminal state never moves again.
[[nodiscard]] bool is_terminal(domain::OrderState state) noexcept;

// Whether `to` is a legal successor of `from` per the transition table below.
// A self-transition (from == to) is legal (it is the NoChange case). Terminal
// states have no outgoing transitions (they are absorbing). The full matrix is
// documented in lifecycle.cpp.
[[nodiscard]] bool is_valid_transition(domain::OrderState from, domain::OrderState to) noexcept;

// Fold a parent's child slice states into the parent's state.
//
// Rules (documented; see lifecycle.cpp for the matrix):
//   - Empty children            -> Created (no slices registered yet).
//   - ANY child Unknown         -> PartiallyPlaced (the "unknown present" parent
//                                  posture — the parent pauses on an ambiguous
//                                  child until reconciled).
//   - ANY child Rejected        -> ManualInterventionRequired (a slice was
//                                  refused: the parent is partial-and-broken and
//                                  needs a human; no automatic square-off).
//   - All children Filled       -> Filled (the whole parent is done).
//   - All children Cancelled    -> Cancelled.
//   - All children terminal but mixed Filled/Cancelled
//                               -> PartiallyFilled (some legs filled, some not).
//   - Otherwise (at least one child still active / in-flight)
//                               -> PartiallyPlaced (placement is still in
//                                  progress across the slices).
[[nodiscard]] domain::OrderState fold_parent_state(const std::vector<domain::OrderState>& children);

// The apply engine. Holds the per-order last-applied ordering key and the
// registered child states for parents. SOLE WRITER = the main loop (NFR-2);
// not thread-safe by design.
class LifecycleEngine {
 public:
  LifecycleEngine() = default;

  // Apply a broker view to `order` (mutated in place). Enforces, in order:
  //   1. terminal-absorbing : if `order` is already terminal -> DroppedTerminal
  //      (never moved out of a sink), regardless of the view's ordering_key.
  //   2. forward-progressing : if view.ordering_key < the last key applied for
  //      view.client_ref -> DroppedStale (an older / duplicate observation).
  //   3. legal transition    : if observed_state is not a legal successor of the
  //      current state -> NoChange (the illegal jump is refused; the order is
  //      left untouched but the key is still recorded as seen).
  // On a legal, current, state-changing view -> Applied: the order's state,
  // broker_order_id, filled_qty and avg_price are updated and the last-applied
  // key advances. A legal current view that does not change the state ->
  // NoChange (the key still advances so a later equal key is idempotent).
  ApplyOutcome apply(domain::Order& order, const BrokerView& view);

  // Register/refresh a child slice's state and recompute the parent's folded
  // state. `parent_client_ref` is the parent ref ("<parent>"); `child_client_ref`
  // is a child ref ("<parent>#<k>"). Returns the recomputed parent state
  // (fold_parent_state over all known children of this parent). The parent state
  // is also cached and queryable via parent_state().
  domain::OrderState apply_child(std::string_view parent_client_ref,
                                 std::string_view child_client_ref, domain::OrderState child_state);

  // The last ordering key applied for `client_ref`, if any view has been applied
  // to it yet. (A DroppedStale call does not advance it; an Applied/NoChange does.)
  [[nodiscard]] std::optional<std::int64_t> last_key(std::string_view client_ref) const;

  // The current folded parent state for a registered parent, if any child has
  // been registered for it via apply_child(). Empty if the parent is unknown.
  [[nodiscard]] std::optional<domain::OrderState> parent_state(
      std::string_view parent_client_ref) const;

 private:
  // Per-order monotonic high-water mark of the applied ordering key.
  std::unordered_map<std::string, std::int64_t> last_key_;

  // Per-parent map of child_client_ref -> last-known child state. The parent's
  // folded state is recomputed from this set on every apply_child().
  std::unordered_map<std::string, std::unordered_map<std::string, domain::OrderState>>
      children_by_parent_;
};

}  // namespace broker_exec::lifecycle
