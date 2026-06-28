#pragma once

// broker_exec::marginsafety — the MARGIN SAFETY BUFFER: never trust the broker
// margin/basket-margin API at face value.
//
// THE WHOLE POINT IS "THE BROKER UNDER-COUNTS THE MARGIN IT WILL ACTUALLY
// BLOCK". A real, recurring Kite-developer failure mode: the margin / basket-
// margin API returns a number that is LESS than the margin the RMS blocks at
// execution, for two well-understood reasons —
//   * SPAN IS NOT FIXED YET: for an expiry-day / pre-9:20 contract the exchange
//     SPAN parameters are not finalized; the API serves a provisional (lower)
//     figure that firms UP after 9:20 AM. Quoting it as gospel under-counts.
//   * LEG BENEFIT IS COUNTED THEN REVOKED: a multi-leg basket is quoted with the
//     spread/leg margin-benefit netted in (a small NET number), but at execution
//     the benefit is restricted (legs cross at different instants, pre-position
//     the offset does not yet exist), so the margin actually blocked is closer to
//     the SUM of the per-leg margins than to the netted quote.
// The consequence either way is the same: an order is REJECTED at submit on a
// margin shortfall, or — worse — the account is silently OVER-LEVERAGED. This
// module refuses to be the victim of that under-count.
//
// THE LOAD-BEARING INVARIANTS (all one direction — OVER-estimate, never under):
//   * FAIL-CLOSED BUFFER (a): the requirement we enforce is the broker figure
//     PLUS a safety buffer (a multiplicative bps cushion + an optional flat
//     cushion). We require MORE free margin than the API claims, so a firmed-up
//     SPAN or a revoked leg-benefit does not turn an "approved" order into a
//     shortfall reject. The buffer can only ever RAISE the requirement.
//   * WORST-CASE MULTI-LEG (b): when the leg benefit cannot be trusted (the
//     SPAN-not-fixed / pre-position boundary), a multi-leg basket is sized
//     against the WORST CASE — the SUMMED per-leg margins with NO benefit — never
//     the netted quote. A "sufficient" account against the netted number can be
//     BLOCKED against the worst case, which is exactly the point.
//   * STALENESS FLAGGED (c): when the benefit is not trusted (near the 9:20 /
//     expiry boundary) the result carries `boundary_flagged` so the caller/audit
//     sees the figure was treated as stale.
//   * ROUND THE BUFFER UP (fail-closed): the bps cushion is computed in integer
//     paise and ROUNDED UP (ceil), never down — a rounded-down buffer would shave
//     paise off the cushion in the unsafe direction.
//   * DEFENSIVE FLOOR: the enforced base can NEVER drop below what the broker
//     already reported (`api_required`); the buffer is strictly additive on top.
//   * NO INTEGER WRAP: the bps product is overflow-guarded so an absurd margin can
//     never wrap to a SMALLER (negative) effective requirement — overflow
//     saturates UP (a larger requirement is always the safe direction).
//
// This module is COMPLEMENTARY to options::margin_shock (Story 5.4): that one
// models a forward volatility SHOCK against available funds; this one buffers the
// REPORTED requirement and picks the worst-case multi-leg figure. Both fail
// closed; neither trusts a single broker number.
//
// Conventions: namespace broker_exec::marginsafety; no-throw across the boundary
// (return a populated result / typed Error, never propagate); NO double/float
// (integer domain::Money paise throughout — bps math is integer-only); redaction-
// safe `detail` (only Money.to_string() / verdict name / boolean flags — never a
// secret or raw broker text). Cross-platform: C++20 standard library only — NO OS
// APIs, NO `#ifdef`. Depends inward on `domain` (Money), `errors` (Result/Error),
// and `ports` (Ok).

#include <string>
#include <string_view>

#include "broker_exec/domain/money.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::marginsafety {

// The safety cushion applied OVER the broker-reported requirement.
//   buffer_bps  — a multiplicative cushion in basis points (1 bp = 1/10000).
//                 Default 500 bps = 5%. CLAMPED to >= 0 at use (a negative buffer
//                 would shrink the requirement — the unsafe direction — so it is
//                 floored to zero, never applied negatively).
//   flat_buffer — an optional flat paise cushion added on top (e.g. an absolute
//                 pad for fixed slippage in the blocked margin). A negative flat
//                 cushion is treated as zero at use (same fail-closed reasoning).
struct MarginSafetyConfig {
  int buffer_bps = 500;
  domain::Money flat_buffer;
};

// The pre-trade inputs weighed by the evaluator. All Money is integer paise.
//   api_required      — the broker-reported required margin (a single order, or
//                       the NETTED basket figure with leg-benefit already applied).
//   summed_leg_margin — the WORST CASE: the sum of the per-leg margins with NO leg
//                       benefit. For a genuine spread this is >= api_required.
//   available         — the deployable funds the requirement is tested against.
//   is_multi_leg      — a basket / spread (true) vs a single order (false).
//   benefit_trusted   — whether the leg-benefit (and the SPAN figure behind it) can
//                       be trusted. FALSE near the 9:20 / expiry SPAN-not-yet-fixed
//                       boundary OR pre-position (the offsetting leg does not exist
//                       yet) — in which case the netted quote must NOT be trusted.
struct MarginInputs {
  domain::Money api_required;
  domain::Money summed_leg_margin;
  domain::Money available;
  bool is_multi_leg = false;
  bool benefit_trusted = true;
};

// The terminal verdict. Stable, log-friendly names (see to_string):
//   Sufficient          — available funds cover the buffered (effective)
//                         requirement: the order/basket may proceed.
//   InsufficientBlocked — available funds do NOT cover the buffered requirement:
//                         BLOCK pre-submission (avoid a margin-shortfall reject /
//                         over-leverage). This is also the FAIL-CLOSED default.
enum class MarginVerdict { Sufficient, InsufficientBlocked };

// Stable, log/serialization-friendly verdict name (NFR-8 observability contract).
[[nodiscard]] std::string_view to_string(MarginVerdict verdict) noexcept;

// The evaluation result — the verdict plus the Money evidence trail.
//
// FAIL-CLOSED DEFAULTS: the struct default-constructs to InsufficientBlocked /
// blocked = true so a forgotten field assignment BLOCKS rather than silently
// approving an order. `evaluate_margin` always sets every field; the defaults
// exist only to make a partially-built result safe.
//   verdict          — the terminal decision.
//   blocked          — convenience = (verdict == InsufficientBlocked); the gate
//                      reads this. Kept in lock-step with verdict in one place.
//   base_required    — the requirement chosen BEFORE the buffer (the worst-case
//                      summed figure for an untrusted multi-leg, else api_required;
//                      never below api_required).
//   effective_required — base_required + the flat cushion + the bps cushion: what
//                      we actually require `available` to cover.
//   used_worst_case  — true when the worst-case (summed-leg, no-benefit) rule was
//                      engaged instead of trusting the netted/api figure.
//   boundary_flagged — true when the leg benefit was not trusted (near the 9:20 /
//                      expiry boundary): the figure is treated as stale.
//   detail           — a redaction-safe summary (verdict + Money + flags only; no
//                      secret, no raw broker text).
struct MarginSafetyResult {
  MarginVerdict verdict = MarginVerdict::InsufficientBlocked;
  bool blocked = true;
  domain::Money base_required;
  domain::Money effective_required;
  bool used_worst_case = false;
  bool boundary_flagged = false;
  std::string detail;
};

// Evaluate the margin safety buffer for an order / basket. NO throw: always
// returns a fully populated MarginSafetyResult. The steps (fail-closed = always
// OVER-estimate the requirement, never under — see the file header):
//   1. BASE REQUIRED:
//        * single-leg                       -> in.api_required.
//        * multi-leg + benefit trusted      -> in.api_required (the netted quote).
//        * multi-leg + benefit NOT trusted  -> WORST CASE:
//              max(in.summed_leg_margin, in.api_required), used_worst_case = true.
//      Then a DEFENSIVE FLOOR is applied ALWAYS: base = max(base, in.api_required)
//      — the buffer must never enforce LESS than the broker already states.
//   2. EFFECTIVE REQUIRED = base + flat cushion + bps cushion, where the bps
//      cushion is ROUNDED UP (ceil): buffer_amount =
//        (base.paise() * clamp(buffer_bps, >=0) + 9999) / 10000.
//      The bps product is OVERFLOW-GUARDED (a near-INT64_MAX margin saturates the
//      cushion UP rather than wrapping to a smaller requirement); the additions
//      saturate at INT64_MAX. effective_required is therefore ALWAYS >= base.
//   3. VERDICT: available >= effective_required -> Sufficient (blocked = false);
//      else InsufficientBlocked (blocked = true).
//   4. boundary_flagged = !in.benefit_trusted (staleness near the 9:20 / expiry
//      boundary).
[[nodiscard]] MarginSafetyResult evaluate_margin(const MarginInputs& in,
                                                 const MarginSafetyConfig& cfg);

// Gate adapter: ok() iff the result is not blocked; otherwise a typed, redaction-
// safe Error (RiskRejected / BlockStrategy) naming the shortfall as Money only
// (effective_required vs available are non-secret). Never throws.
[[nodiscard]] Result<ports::Ok> require_margin_ok(const MarginSafetyResult& r);

}  // namespace broker_exec::marginsafety
