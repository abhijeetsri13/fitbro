#pragma once

// broker_exec::slicing — the freeze-quantity slicer (Story 2.9, FR-6, IBR-2).
//
// WHAT THIS IS: a PURE, deterministic function that fans an over-freeze parent
// OrderIntent out into lot-aligned child intents, each ≤ the instrument's
// exchange freeze ceiling, with a deterministic `<parent>#<k>` client-ref
// (k from 1). Bit-identical re-slicing on replay + the store's
// UNIQUE(client_ref) backstop make a sliced child safe to re-emit after a crash
// (any already-placed child dedupes). Composes with the validation gate's
// AllowWithSlicing decision (Story 2.8) and the parent/child lifecycle fold
// (Story 1.8); the deterministic refs are what let the FSM recover the parent
// (idempotency::parent_of).
//
// DEPENDENCY-LIGHT BY DESIGN: the production target depends inward only on
// `domain` + `errors`. The `<parent>#<k>` child-ref format is the binding format
// owned by `idempotency::child_ref`; it is reproduced INLINE here so the slicer
// does not pull in idempotency's store/SQLite dependencies. A test asserts
// parity with `idempotency::child_ref` / `is_child_ref` / `parent_of`.
//
// FAIL-CLOSED: any invalid input yields a single Validation Error — never a
// partial or odd slice. No clock, no randomness, no I/O: same inputs always
// produce bit-identical output.
//
// CROSS-PLATFORM: C++20 stdlib only. Integer `domain::Quantity` throughout — no
// floating point. No OS APIs, no `#ifdef`.

#include <vector>

#include "broker_exec/domain/types.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::slicing {

// Slices an over-freeze parent order into lot-aligned children under the
// instrument's freeze ceiling. Pure and deterministic.
class FreezeSlicer {
 public:
  // Slice `parent` against `inst`. On success returns a non-empty vector:
  //   * not over-freeze (qty <= freeze)  -> a single element { parent } UNCHANGED
  //                                         (original client_ref, no '#').
  //   * over-freeze                      -> children k = 1..N, each a copy of
  //                                         `parent` with `quantity` replaced and
  //                                         `client_ref = "<parent>#<k>"`. The child
  //                                         quantities sum EXACTLY to the parent qty;
  //                                         every child qty is in [lot, chunk] and
  //                                         lot-aligned (chunk = largest lot-aligned
  //                                         qty <= freeze).
  // Returns a single Validation Error (never a partial slice) when: the parent
  // client_ref already contains '#' (a child cannot be re-sliced); lot <= 0;
  // qty <= 0; qty is not lot-aligned (qty < lot or qty % lot != 0); freeze <= 0;
  // or freeze < lot (a freeze ceiling below one lot cannot be sliced).
  [[nodiscard]] Result<std::vector<domain::OrderIntent>> slice(
      const domain::OrderIntent& parent, const domain::Instrument& inst) const;
};

}  // namespace broker_exec::slicing
