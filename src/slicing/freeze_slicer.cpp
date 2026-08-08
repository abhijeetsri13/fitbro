#include "broker_exec/slicing/freeze_slicer.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::slicing {

namespace {

// A single Validation Error with DoNotRetry (the category's default action):
// the input must be fixed, not retried as-is. Fail-closed — the caller never
// gets a partial slice.
[[nodiscard]] broker_exec::Result<std::vector<domain::OrderIntent>> validation_error(
    std::string message) {
  return broker_exec::fail(
      errors::make_error(errors::ErrorCategory::Validation, std::move(message)));
}

// Reproduce the binding child-ref format `<parent>#<k>` (k >= 1) INLINE so the
// production target stays free of the idempotency module's store/SQLite deps.
// A test asserts parity with broker_exec::idempotency::child_ref.
[[nodiscard]] std::string child_ref(const std::string& parent_client_ref, std::int64_t k) {
  return parent_client_ref + "#" + std::to_string(k);
}

}  // namespace

Result<std::vector<domain::OrderIntent>> FreezeSlicer::slice(const domain::OrderIntent& parent,
                                                             const domain::Instrument& inst) const {
  const std::int64_t qty = parent.quantity.value();
  const std::int64_t freeze = inst.freeze_qty.value();
  const std::int64_t lot = inst.lot_size.value();

  // ── Validation (fail-closed; a single Validation Error, no partial output) ──

  // A child ref ("<parent>#<k>") can never be re-sliced — refuse it first so the
  // emitted refs are always exactly one level deep.
  if (parent.client_ref.find('#') != std::string::npos) {
    return validation_error("freeze slicer: cannot slice a child order ref");
  }
  // Lot must be a positive granularity before any lot-alignment math.
  if (lot <= 0) {
    return validation_error("freeze slicer: lot size must be positive");
  }
  // Quantity must be a positive, lot-aligned multiple of at least one lot.
  if (qty <= 0) {
    return validation_error("freeze slicer: order quantity must be positive");
  }
  if (qty < lot || qty % lot != 0) {
    return validation_error("freeze slicer: order quantity is not lot-aligned");
  }
  // Freeze ceiling must admit at least one whole lot, else it cannot be sliced.
  if (freeze <= 0) {
    return validation_error("freeze slicer: freeze quantity must be positive");
  }
  if (freeze < lot) {
    return validation_error("freeze slicer: freeze quantity is below one lot");
  }

  // ── Slice ───────────────────────────────────────────────────────────────────

  // Not over-freeze: emit the parent unchanged (original client_ref, no '#').
  if (qty <= freeze) {
    return std::vector<domain::OrderIntent>{parent};
  }

  // Largest lot-aligned quantity that fits under the freeze ceiling. Integer
  // division floors freeze down to a lot multiple, so chunk is in [lot, freeze]
  // and lot-aligned. (freeze >= lot was validated above, so chunk >= lot > 0.)
  const std::int64_t chunk = (freeze / lot) * lot;

  // EACH CHILD IS A COPY OF THE PARENT with only `quantity` and `client_ref`
  // changed. That is the rule, and it is what makes the trigger price (IMP-11)
  // inherit for free: an over-freeze stop-loss fans out into children that arm at
  // the SAME level as the parent, because slicing changes SIZE, never price. A
  // slicer that rebuilt children field-by-field would drop the trigger the day a
  // new field was added — the children of a stop would place as plain orders,
  // unprotected. Copy-then-override keeps that class of bug impossible.
  //
  // full full-chunk children, then a remainder child iff rem > 0. Because qty is
  // a multiple of lot and chunk is a multiple of lot, rem = qty % chunk is a
  // non-negative multiple of lot; when non-zero it is in [lot, chunk). Hence the
  // child quantities sum EXACTLY to qty and each lies in [lot, chunk].
  const std::int64_t full = qty / chunk;
  const std::int64_t rem = qty % chunk;

  std::vector<domain::OrderIntent> children;
  children.reserve(static_cast<std::size_t>(full + (rem > 0 ? 1 : 0)));

  std::int64_t k = 1;
  for (std::int64_t i = 0; i < full; ++i, ++k) {
    domain::OrderIntent child = parent;
    child.quantity = domain::Quantity::of(chunk);
    child.client_ref = child_ref(parent.client_ref, k);
    children.push_back(std::move(child));
  }
  if (rem > 0) {
    domain::OrderIntent child = parent;
    child.quantity = domain::Quantity::of(rem);
    child.client_ref = child_ref(parent.client_ref, k);
    children.push_back(std::move(child));
  }

  return children;
}

}  // namespace broker_exec::slicing
