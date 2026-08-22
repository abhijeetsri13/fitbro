#pragma once

// broker_exec::risk — the pre-submission validation gate (Story 2.8, FR-6).
//
// The gate is the single, non-bypassable checkpoint every order crosses before
// any broker mutation (the runtime's only path to dispatch() runs through it).
// It composes already-built modules (refdata::TradingCalendar) plus INJECTED
// predicates for modules not yet built (funds = Story 2.11, risk-engine = 2.10,
// slicer = 2.9, kill-switch = 3.8, UNKNOWN-pause runtime). Later stories wire the
// real implementations; the gate is complete and testable now.
//
// The checks run in a FIXED, fail-closed order (cheapest / safety first). The
// FIRST failing check returns a typed Error whose message NAMES that check
// ("gate: <check> check failed: ..."). A risk-reducing op (exit / square-off /
// hedge-completion / emergency) is EXEMPT from the entry-only blocks — a
// protective leg is never frozen.
//
// STOP-ORDER SHAPE (IMP-11). OrderIntent carries a distinct
// `std::optional<Price> trigger_price`, so the gate owns the (order_type x price
// fields) matrix and enforces it FAIL-CLOSED as the `order-shape` check:
//
//   Market    limit ignored   trigger FORBIDDEN
//   Limit     limit REQUIRED  trigger FORBIDDEN
//   StopLoss  limit REQUIRED  trigger REQUIRED   (SL — a stop-loss LIMIT)
//   SL-M      limit ignored   trigger REQUIRED   (fires a market order)
//
// An SL's two prices must also be SIDE-ORDERED — SELL requires limit <= trigger,
// BUY requires limit >= trigger (equal allowed). A swapped pair is a shape bug the
// broker accepts happily: the stop arms and then sits unfillable through the whole
// move it existed to escape.
//
// The trigger is tick-checked exactly as the limit is (the pre-IMP-11 gate
// tick-checked SL-M's `price` because the domain had nowhere else to carry a
// trigger; it now reads the real field). SHAPE VALIDATION IS NOT EXIT-EXEMPT: an
// exit skips the entry-only blocks but never the shape/tick checks, because a
// malformed protective order is a broker rejection, not protection.
//
// BOUNDARY: risk depends INWARD only on domain, errors, refdata, capabilities
// and ports. No transport/adapter/SDK dependency.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`. No
// double/float — lot/tick/freeze use the integer Quantity/Price accessors.
// Result<T> is no-throw.

#include <functional>
#include <string>
#include <vector>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/refdata/trading_calendar.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::risk {

// The successful outcome of the gate. A plain Allow, or AllowWithSlicing when an
// over-freeze order in slice-mode must be fanned out into freeze-sized child
// orders (the actual fan-out is Story 2.9). A failing check is an Error, never an
// outcome value.
enum class GateOutcome { Allow, AllowWithSlicing };

// The full input to one gate evaluation: the order under test, its resolved
// instrument (resolved by the caller via the instrument master — the gate keeps
// its resolve responsibility small), the exit/entry classifier, and the injected
// predicates / config for each check. Every injected input defaults to "pass"
// where sensible so a minimal context evaluates a clean entry.
struct GateContext {
  // The order under test and its already-resolved reference data. `instrument`
  // must carry a REAL per-exchange freeze ceiling: a 0 / negative `freeze_qty` is
  // REFUSED by the freeze check, never read as "no ceiling". An unknown ceiling
  // cannot tell an over-freeze order from a safe one, and treating it as absent
  // silently skips the check, discards the `slice_mode` posture below, and leaves
  // AllowWithSlicing unreachable.
  const domain::OrderIntent& intent;
  domain::Instrument instrument;

  // True for a risk-reducing op (exit / square-off / hedge-completion /
  // emergency). EXEMPT from the entry-only blocks: kill-switch(entry),
  // UNKNOWN-pause, time-window(entry-cutoff), duplicate, and funds. The lot /
  // tick / exchange / product / freeze / risk / hedge checks STILL apply.
  bool is_risk_reducing = false;

  // ── Injected safety inputs (entry-only unless noted) ─────────────────────
  // An active soft / strategy / broker / account kill that blocks ENTRIES.
  bool kill_entry_block = false;
  // The runtime UNKNOWN-pause: entries paused while UNKNOWN orders are unresolved.
  bool unknown_pause_active = false;
  // True if this client_ref / signal has already been seen. Empty => not a dup.
  std::function<bool()> is_duplicate;
  // Entry time-window authority. If null, the time-window check is skipped.
  const refdata::TradingCalendar* calendar = nullptr;
  // Margin / funds check (entry-only); fail-closed on stale (DataStale). Empty => pass.
  std::function<Result<ports::Ok>()> funds_check;
  // Account / strategy / instrument / order risk check. Empty => pass.
  std::function<Result<ports::Ok>()> risk_check;
  // Naked-sell / hedge-completion check. Empty => pass.
  std::function<Result<ports::Ok>()> hedge_check;

  // ── Allow-lists (empty => accept any) ────────────────────────────────────
  // Allowed exchanges. If empty, any non-empty instrument exchange is accepted.
  std::vector<std::string> allowed_exchanges;
  // Allowed products. If empty, any product is accepted.
  std::vector<domain::Product> allowed_products;

  // Over-freeze handling: true (default) => AllowWithSlicing; false => reject as
  // a Validation Error (config posture; the real fan-out lands in Story 2.9).
  bool slice_mode = true;
};

// The single non-bypassable surface (AC-2). There is no partial / "skip checks"
// variant: validate() either returns a GateOutcome (Allow / AllowWithSlicing) or
// the named Error of the FIRST failing check.
class ValidationGate {
 public:
  [[nodiscard]] Result<GateOutcome> validate(const GateContext& ctx) const;
};

}  // namespace broker_exec::risk
