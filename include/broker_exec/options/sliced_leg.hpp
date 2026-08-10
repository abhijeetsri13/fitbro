#pragma once

// broker_exec::options — freeze-slicing a basket leg at option size (Story 5.3,
// FR-6 / FR-17 composition).
//
// THE WHOLE POINT IS "BIG, BUT WITHOUT DUPLICATES": an option order above the
// exchange freeze ceiling must be fanned into lot-aligned children and placed —
// but a mid-slice crash (SIGKILL) must leave NO orphan and NO duplicate on
// recovery. Two invariants make that hold, and both are REUSED rather than
// reinvented here:
//   * DETERMINISTIC CHILD REFS (AC-1/AC-2): the children come straight from the
//     real `slicing::FreezeSlicer` — each child carries the binding
//     `<parent>#<k>` client-ref (k from 1). Re-slicing the same parent on replay
//     re-emits bit-identical refs, so an already-placed child is recognizable by
//     ref alone. This file does NOT reimplement the slice or the ref format.
//   * IDEMPOTENT PLACEMENT (AC-2): before (re)sending a child the executor asks
//     the `already_placed` seam — the in-memory analog of the store's
//     UNIQUE(client_ref) backstop — and a child already on the broker is counted
//     as deduped and NEVER re-sent. A crash-and-replay therefore dedupes the
//     children that were already placed and only sends the rest.
//
// THE LOAD-BEARING INVARIANTS:
//   * FAIL-CLOSED SLICE (AC-1, SliceRejected): a null `place_child` seam, or a
//     slicer Validation Error (bad lot/freeze/qty, or a `#`-child re-slice),
//     places NOTHING. Never a partial or odd slice.
//   * UNKNOWN PAUSES THE WHOLE LEG (AC-3): a child that comes back `Unknown` — OR
//     a place / already_placed call that fails with an Error (an ambiguous
//     mutating outcome that must NOT be blindly retried) — STOPS placement at
//     that child. No remaining child is sent, the leg is paused for reconcile,
//     and ONE Critical alert fires. Mirrors the dispatch no-blind-retry rule
//     (Story 1.9) and reconcile-before-resume recovery (Story 3.4); resume is
//     safe because the already-placed children dedupe.
//
// ALERTING IS BEST-EFFORT (mirrors 5.1 / 5.2): the Critical alert is sent AFTER
// the executor has recorded the pause; the AlertSink::send Result is swallowed
// AND the call is wrapped in try/catch so a dead or THROWING alert channel can
// never break this function's no-throw contract.
//
// Conventions: no-throw across the boundary (return an outcome, never propagate),
// no double/float (integer domain::Quantity throughout the slice), redaction-safe
// `detail` + alert messages (only non-secret child client_refs / broker order_ids
// and stable error-category tags — never raw broker text). Cross-platform: C++20
// standard library only — NO OS APIs, NO `#ifdef`. Depends inward on `domain`
// (OrderIntent/Instrument), `errors` (Result/Error), `ports` (AlertSink), and —
// in the .cpp only — `slicing` (FreezeSlicer).

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "broker_exec/domain/types.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::options {

// How a single sliced child ended up. Stable, log-friendly names (see to_string):
//   Acked         — the place_child seam acknowledged the child (a fresh send).
//   AlreadyPlaced — the already_placed seam proved the child was ALREADY on the
//                   broker (idempotent dedupe on replay) — NOT re-sent (AC-2).
//   Unknown       — the child's placement is ambiguous (place returned Unknown,
//                   or a place / already_placed call errored): the leg is paused.
enum class ChildPlacement { Acked, AlreadyPlaced, Unknown };

// Stable, log/serialization-friendly child-placement names (NFR-8 observability).
[[nodiscard]] std::string_view to_string(ChildPlacement placement) noexcept;

// Terminal outcome of the whole sliced leg. Stable, log-friendly names:
//   FilledSliced  — every child ended Acked or AlreadyPlaced (the success
//                   terminal; includes the all-dedup replay case).
//   UnknownPaused — a child came back Unknown (or a mutating call errored):
//                   placement STOPPED, the leg is paused for reconcile (AC-3).
//   SliceRejected — fail-closed BEFORE any placement: a null place_child seam, or
//                   the slicer returned a Validation Error. Nothing placed.
enum class SlicedLegOutcome { FilledSliced, UnknownPaused, SliceRejected };

// Stable, log/serialization-friendly outcome names (NFR-8 observability).
[[nodiscard]] std::string_view to_string(SlicedLegOutcome outcome) noexcept;

// Per-child result. `client_ref` is the deterministic `<parent>#<k>` ref from the
// slicer. `broker_order_id` is the id the place_child seam returned for an Acked
// child (empty for AlreadyPlaced / Unknown). Both fields are non-secret.
struct ChildResult {
  std::string client_ref;
  ChildPlacement placement;
  std::string broker_order_id;
};

// The aggregate result. `children` reports every child the executor TOUCHED in
// slice order (it stops at the paused child on UNKNOWN, so it may be shorter than
// the full slice). `placed_count` / `deduped_count` count fresh Acked sends and
// idempotent dedupes respectively. `paused_at_ref` is the child ref the leg
// paused on (empty unless UnknownPaused). `detail` is a redaction-safe summary
// (only client_refs / order_ids / stable error tags).
struct SlicedLegResult {
  SlicedLegOutcome outcome;
  std::vector<ChildResult> children;
  int placed_count;
  int deduped_count;
  std::string paused_at_ref;
  std::string detail;
};

// The injected seams that keep the executor broker-neutral and unit-testable with
// NO real broker.
//   already_placed — idempotency probe: is this child ALREADY on the broker?
//                    `true` => dedupe (do not re-send). A null seam is treated as
//                    "not known to be placed" (proceed to place — the store's
//                    UNIQUE(client_ref) is the real backstop). An Error is
//                    fail-closed as UNKNOWN: we cannot prove it is safe to (re)send.
//   place_child    — REQUIRED: place one child, returning its placement state
//                    (Acked or Unknown) + broker order id. A null place_child
//                    fails the leg closed (SliceRejected) — never proceed with no
//                    placer. An Error is an ambiguous mutating failure => UNKNOWN.
struct SlicedLegSeams {
  std::function<Result<bool>(const std::string& client_ref)> already_placed;
  std::function<Result<std::pair<ChildPlacement, std::string>>(const domain::OrderIntent& child)>
      place_child;
};

// Execute the sliced leg over the injected seams. NO throw: every path returns a
// populated SlicedLegResult rather than propagating. The steps (see the file
// header for the invariants):
//   0. FAIL-CLOSED — a null place_child seam => SliceRejected, nothing placed.
//   1. SLICE — `FreezeSlicer{}.slice(parent, inst)`. On Error => SliceRejected
//      (error tag in detail), nothing placed. A not-over-freeze parent slices to
//      a single unchanged child and is handled identically (one "child").
//   2. PLACE IN SLICE ORDER (k=1..N) — for each child: `already_placed` true =>
//      AlreadyPlaced + ++deduped_count, CONTINUE (no re-send); Error => UNKNOWN.
//      Else `place_child`: Acked => ++placed_count; Unknown or Error => UNKNOWN.
//   3. UNKNOWN-PAUSE — STOP (place no remaining child), set paused_at_ref,
//      UnknownPaused, send ONE Critical alert (best-effort, swallowed + try/catch).
//   4. SUCCESS — every child Acked/AlreadyPlaced => FilledSliced.
[[nodiscard]] SlicedLegResult execute_sliced_leg(const domain::OrderIntent& parent,
                                                 const domain::Instrument& inst,
                                                 const SlicedLegSeams& seams,
                                                 ports::AlertSink& alerts);

}  // namespace broker_exec::options
