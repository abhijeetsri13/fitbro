#pragma once

// broker_exec::priceband — the PRE-SUBMISSION CIRCUIT/LPP PRICE-BAND VALIDATOR
// (a pure decision).
//
// PROBLEM (real Kite developer complaints): the exchange rejects any order whose
// price falls outside the instrument's DYNAMIC circuit / LPP / DPR price band —
// "price out of LPP range" — and the published band LAGS fast moves. The cruel
// part is the timing: a limit entry or a protective exit priced outside the band
// is rejected EXACTLY during the volatility you most need it, wasting a rejected
// round-trip on the entry and (worse) silently failing the exit. Even a MARKET
// order can be rejected out-of-band on a fast instrument, though there is no
// limit price to pre-clamp there.
//
// This module does a CHEAP pre-submission check against the band the caller
// already holds and returns a verdict. It NEVER sends, never reads I/O, never
// throws. Integer Money paise only (no float in any price path), no OS API, no
// `#ifdef`. It mirrors the sibling `broker_exec::modes` / `broker_exec::modifyguard`
// pure-decision style (enum + to_string + result struct + fail-closed + a thin
// Result<Ok> gate wrapper). It depends inward only on `domain` (Side/OrderType/
// Money), `errors`, and `ports`.
//
// THE TWO LOAD-BEARING INVARIANTS (asserted in tests, stated here loudly):
//   * AN EXIT IS NEVER BLOCKED. A protective exit must still reach the broker
//     during volatility — so an out-of-band exit is CLAMPED to the band edge
//     (priced where the exchange will accept it), never refused. Blocking an exit
//     would strand a position naked.
//   * AN OUT-OF-BAND ENTRY IS ALWAYS BLOCKED. A new entry priced outside the band
//     would only earn an exchange rejection; we block it pre-submission to save
//     the wasted round-trip. There is NO (side, order_type) path on which an
//     out-of-band entry slips through with blocked == false.
//
// FAIL-CLOSED, with one deliberate exception. Every defaulted field of
// BandCheckResult is the SAFE non-acting value (verdict BandUnknown, blocked =
// false, no suggestion). The one place "fail-closed" is intentionally NOT "block
// everything" is a MISSING band: blocking all trading because the band feed is
// unavailable would turn a data outage into a self-inflicted trading freeze, so a
// BandUnknown verdict lets the order through UNVALIDATED but flags it for the
// caller to escalate. The out-of-band ENTRY path explicitly overrides the default
// to blocked = true, so the non-freezing default can never let a *validated*
// out-of-band entry through.

#include <string>
#include <string_view>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::priceband {

// The band-check verdict vocabulary. Stable, log/serialization-friendly names
// that are part of the observability contract (NFR-8): renames are breaking.
//   WithinBand               — every checked price is inside [lower, upper]; OK.
//   OutsideBandBlocked       — an ENTRY priced outside the band; blocked pre-
//                              submission (it would be exchange-rejected).
//   OutsideBandExitClamped   — an EXIT priced outside the band; NOT blocked —
//                              the suggested limit is clamped to the band edge so
//                              the protective exit still survives.
//   MarketUnchecked          — a PLAIN market order carries no price of ours to
//                              enforce (the exchange may still reject it, but we
//                              cannot pre-clamp a market price). An SL-M does NOT
//                              land here: its TRIGGER is enforceable.
//   BandUnknown              — band unavailable or malformed; price NOT validated
//                              (non-freezing default — let it through, but flag).
enum class BandVerdict {
  WithinBand,
  OutsideBandBlocked,
  OutsideBandExitClamped,
  MarketUnchecked,
  BandUnknown
};

// Stable, log/serialization-friendly name (NFR-8 observability contract).
[[nodiscard]] std::string_view to_string(BandVerdict verdict) noexcept;

// The instrument's current circuit / LPP / DPR price band, as the caller holds
// it. Integer Money paise only (no float). `known == false` means the band feed
// is unavailable; an INVERTED/degenerate band (known && lower > upper) is treated
// as not-known because a malformed band cannot validate anything.
struct PriceBand {
  domain::Money lower;
  domain::Money upper;
  bool known = false;
};

// The band check's decision. DEFAULTS ARE THE SAFE NON-ACTING VALUE: an
// unassigned result is BandUnknown / not-blocked / no-suggestion, so a forgotten
// assignment denies nothing AND clamps nothing (it cannot fabricate a bogus
// suggested price). `blocked` is the single boolean a gate acts on; `verdict`
// explains WHY; `suggested_limit` is meaningful ONLY when `has_suggestion` is
// true (the limit pulled inside the band for the caller to apply); `detail` is a
// redaction-safe, human-readable summary (verdict / side + Money only — never a
// secret).
struct BandCheckResult {
  BandVerdict verdict = BandVerdict::BandUnknown;
  bool blocked = false;
  domain::Money suggested_limit;
  bool has_suggestion = false;
  // For a STOP-LIMIT order, the trigger must be brought in-band too — clamping only
  // the limit while leaving the trigger outside would still earn the exact "price
  // out of LPP range" rejection. `suggested_trigger` is the trigger pulled inside
  // the band, meaningful ONLY when `has_trigger_suggestion` is true.
  domain::Money suggested_trigger;
  bool has_trigger_suggestion = false;
  std::string detail;
};

// Validate an order's price(s) against the band. PURE / NO-THROW.
//
// `limit_price` is the order's limit (meaningful for Limit / stop-limit orders);
// `trigger_price` is additionally validated for a stop-limit. `is_exit` flips the
// out-of-band branch between CLAMP (exit) and BLOCK (entry) — the spine of the
// design.
//
// Logic (fail-closed, exit-priority; first match wins):
//   1. INVERTED/degenerate band (known && lower > upper) -> treated as NOT known
//      (a malformed band cannot validate) -> falls into the BandUnknown branch.
//   2. band NOT known -> BandUnknown: blocked = false (a missing band is its own
//      outage; we do NOT freeze trading on it), no suggestion. The caller MAY
//      escalate — we fail toward letting the order through, but flag it.
//   3. PLAIN MARKET order (no price of ours at all) -> MarketUnchecked:
//      blocked = false. CAVEAT: the exchange may still reject a market order
//      out-of-band; we cannot pre-clamp a market price. NOTE an SL-M is NOT here
//      — see step 4.
//   4. Otherwise, validate exactly the prices the type USES (mirroring the shape
//      matrix the validation gate owns):
//        Limit -> the limit.  SL -> the limit AND the trigger.
//        SL-M  -> the TRIGGER alone (it has no limit; `limit_price` is ignored).
//      An SL-M is band-checked even though it FIRES at market, because the
//      exchange rejects a stop whose TRIGGER is outside the band regardless of
//      how the order fires — skipping it left the one order type meant to survive
//      a violent move as the only type never validated.
//      A price is IN BAND iff lower <= price <= upper (INCLUSIVE). All checked
//      prices in band -> WithinBand, blocked = false.
//   5. OUT OF BAND (any checked price outside [lower, upper]):
//        * is_exit == true  -> OutsideBandExitClamped: blocked = FALSE (a
//          protective exit must NEVER be blocked), with a clamped suggestion for
//          each price the type uses.
//        * is_exit == false -> OutsideBandBlocked: blocked = TRUE (an out-of-band
//          entry would be exchange-rejected), with the same clamped suggestions
//          for the caller's information.
//      Suggestions are offered ONLY for prices the type uses: `has_suggestion` is
//      false for an SL-M (no limit field exists to apply it to) and
//      `has_trigger_suggestion` is false for a plain Limit.
[[nodiscard]] BandCheckResult check_price_band(domain::Side side, domain::OrderType order_type,
                                               domain::Money limit_price,
                                               domain::Money trigger_price, const PriceBand& band,
                                               bool is_exit);

// Thin gate wrapper over check_price_band: returns a typed Validation /
// BlockStrategy Error (naming the verdict + side, redaction-safe) IFF the check
// blocks (an out-of-band ENTRY), else ok(). An exit clamp is NOT blocked, so this
// returns ok() for the clamp case — the caller then applies `suggested_limit`.
// This lets a validation gate call require_band_ok() directly as a chokepoint.
[[nodiscard]] Result<ports::Ok> require_band_ok(domain::Side side, domain::OrderType order_type,
                                                domain::Money limit_price,
                                                domain::Money trigger_price, const PriceBand& band,
                                                bool is_exit);

// ── INTENT-DRIVEN OVERLOADS (IMP-11) ────────────────────────────────────────
//
// The four-price-argument forms above exist because OrderIntent used to carry ONE
// price: a caller had to SYNTHESIZE the trigger (usually by passing `price`
// twice), which meant the band check validated a number the broker would never
// see. OrderIntent now carries a distinct `std::optional<Price> trigger_price`,
// so these overloads read the REAL pair straight off the intent — the caller can
// no longer pair the wrong two numbers, or forget to pass a trigger at all.
//
// PREFER THESE. The explicit-price forms are KEPT (removing them would ripple
// through every caller for no safety gain) and are the right tool when the prices
// under test are not an intent's own — e.g. probing a hypothetical replacement
// price before building the amended intent.
//
// An ABSENT trigger maps to a zero Money. That is harmless by construction: the
// trigger is only ever CHECKED for a stop-limit (SL), and the shape matrix at the
// validation gate refuses an SL with no trigger before it can reach a band check.
[[nodiscard]] BandCheckResult check_price_band(const domain::OrderIntent& intent,
                                               const PriceBand& band, bool is_exit);

[[nodiscard]] Result<ports::Ok> require_band_ok(const domain::OrderIntent& intent,
                                                const PriceBand& band, bool is_exit);

}  // namespace broker_exec::priceband
