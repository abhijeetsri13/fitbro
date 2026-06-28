#pragma once

// broker_exec::fillnorm — the CANONICAL FILL NORMALIZER (broker-neutral,
// fail-closed fill-state truth).
//
// WHY THIS EXISTS (real Kite / Kotak Neo developer pain):
//   (a) A PARTIAL fill arrives over the Kite websocket as an order-update of type
//       `UPDATE` (NOT `COMPLETE`). A naive strategy switches on the EVENT TYPE,
//       reads "UPDATE" as "not filled", and either RE-ENTERS (doubling the
//       position) or fails to manage the leg it already owns. The event type is a
//       LIE about fill state; the FILLED QUANTITY is the truth.
//   (b) Postback / websocket order events are NOT authoritative: they fire only
//       for app-placed orders, can MISS the first OPEN/COMPLETE entirely, and
//       arrive OUT OF ORDER. A push must therefore never directly drive an exit —
//       only a value read back from an authoritative reconcile is trusted to size
//       a real square-off.
//
// THE TWO LOAD-BEARING RULES (read both twice):
//   1. DRIVE THE CANONICAL STATE OFF THE FILLED QUANTITY, NOT THE EVENT TYPE.
//      filled>0 && pending>0 is PartiallyFilled no matter whether the broker
//      called the event UPDATE, OPEN, or COMPLETE. The quantity is checked first;
//      status only decides terminal-by-reject/cancel and disambiguates the
//      no-quantity case. An unrecognized status with no usable qty signal ⇒
//      Unknown (force a reconcile; never guess).
//   2. A PUSH IS NEVER AUTHORITATIVE. `authoritative` is true ONLY when the
//      snapshot was sourced from a reconcile read (`from_reconcile`). The ONLY
//      basis for SIZING AN EXIT off a fill is `exit_qty_trustworthy` ==
//      (authoritative && filled_qty > 0). `exit_qty_for` yields 0 for any
//      non-authoritative push — do NOT exit off a push; reconcile first.
//
// FAIL-CLOSED: a defaulted FillSnapshot is { Unknown, 0, 0, !authoritative,
// !exit_qty_trustworthy, "" } so a forgotten field forces a reconcile and never
// trusts a fill. Quantities are clamped non-negative; pending can never go
// negative even when a broker reports filled > total.
//
// REDACTION: `raw_status` is UNTRUSTED broker text and may embed account- or
// token-shaped data. This module NEVER retains or echoes it: `detail` is built
// only from the canonical state name + the (integer) filled/pending counts and is
// additionally run through `domain::scrub` (defense in depth).
//
// SCOPE: a pure-decision module. It performs NO I/O, owns no order book, and does
// not itself reconcile — it normalizes ONE broker order-event into one canonical
// (state, filled, pending, trust) tuple and tells the caller whether that tuple
// may size an exit. It REUSES brokerreason::classify_status for the
// unrecognized-status ⇒ reconcile fail-safe rather than re-deriving it.
//
// Conventions: namespace broker_exec::fillnorm; no-throw across the boundary (may
// allocate the detail std::string — only std::bad_alloc could escape, as with any
// std::string op); NO float (integer quantities only); integer enums only.
// Cross-platform: C++20 standard library only — NO OS APIs, NO `#ifdef`.

#include <cstdint>
#include <string>
#include <string_view>

#include "broker_exec/domain/enums.hpp"

namespace broker_exec::fillnorm {

// The single canonical fill view derived from one broker order-event.
//
// EVERY field defaults to the fail-CLOSED value: an unconstructed/forgotten
// snapshot is Unknown, zero-quantity, non-authoritative, and not exit-trustworthy
// — so the caller is driven to reconcile and can never accidentally trust a fill
// it never actually observed.
struct FillSnapshot {
  // The canonical lifecycle state, DERIVED OFF THE QUANTITY FIRST (see
  // normalize_fill). Unknown means "could not determine — reconcile".
  domain::OrderState canonical_state = domain::OrderState::Unknown;

  // Executed quantity, clamped >= 0. THIS — not the event type — is the truth
  // about whether (and how much of) the order filled.
  std::int64_t filled_qty = 0;

  // Outstanding quantity, clamped >= 0 and == max(0, total - filled). Never
  // negative even if a broker reports filled > total.
  std::int64_t pending_qty = 0;

  // True ONLY when this snapshot was sourced from a reconcile read. A push
  // (websocket/postback) event is ALWAYS false — pushes are not authoritative.
  bool authoritative = false;

  // True IFF authoritative AND filled_qty > 0. The ONLY basis for sizing an exit
  // off a fill. False for every push (so a push can never drive an exit) and for
  // a reconcile that observed no fill.
  bool exit_qty_trustworthy = false;

  // Redaction-safe: names the canonical state + the integer filled/pending counts
  // only. NEVER contains any part of the raw broker status text (scrubbed).
  std::string detail;
};

// Normalize ONE broker order-event into a canonical FillSnapshot. NO throw across
// the boundary (may allocate `detail`).
//
// `raw_status`   raw broker order-status text (Kite COMPLETE/OPEN/UPDATE/REJECTED/
//                CANCELLED/TRIGGER PENDING/...; Kotak `ordSt`). Untrusted; never
//                echoed.
// `filled_qty`   broker-reported executed qty (clamped >= 0).
// `total_qty`    broker-reported order qty (clamped >= 0). pending := max(0,
//                total - filled).
// `from_reconcile`  TRUE only when these values came from an authoritative
//                reconcile read; FALSE for a push event. Sets `authoritative`.
//
// State derivation (QUANTITY FIRST — the core fix):
//   * terminal-by-status: a "reject" status ⇒ Rejected; a "cancel" status ⇒
//     Cancelled (carrying whatever filled/pending — a cancel can leave a partial
//     fill, and that filled_qty is what matters downstream).
//   * else by quantity, REGARDLESS of whether the event said UPDATE/OPEN/COMPLETE:
//       filled>0 && pending>0                      ⇒ PartiallyFilled
//       filled>0 && pending==0 && total>0          ⇒ Filled
//       filled==0 && pending>0                     ⇒ Acknowledged (live working
//                                                    order, no fill yet — covers
//                                                    OPEN / TRIGGER PENDING /
//                                                    UPDATE-with-no-fill)
//       filled==0 && pending==0 && total==0        ⇒ Acknowledged if the status is
//                                                    a recognized working state,
//                                                    else Unknown.
//   * if the status is UNRECOGNIZED (brokerreason::classify_status flags it
//     Indeterminate) AND there is no usable quantity signal ⇒ Unknown (force a
//     reconcile; never guess an order's fate).
//
// authoritative = from_reconcile; exit_qty_trustworthy = authoritative && filled>0.
[[nodiscard]] FillSnapshot normalize_fill(std::string_view raw_status, std::int64_t filled_qty,
                                          std::int64_t total_qty, bool from_reconcile);

// The ONLY safe basis for sizing an exit off a fill: returns snap.filled_qty when
// snap.exit_qty_trustworthy, else 0. A non-authoritative push therefore yields 0 —
// do NOT exit off a push; reconcile first. noexcept (reads only).
[[nodiscard]] std::int64_t exit_qty_for(const FillSnapshot& snap) noexcept;

// True iff the snapshot came from an authoritative reconcile read. noexcept.
[[nodiscard]] bool is_authoritative(const FillSnapshot& snap) noexcept;

}  // namespace broker_exec::fillnorm
