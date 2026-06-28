#pragma once

// broker_exec::options — pre-trade margin/SPAN shock simulation (Story 5.4,
// FR-18), capability-gated and fail-closed.
//
// THE WHOLE POINT IS "DON'T OPEN WHAT A VOL SPIKE FORCE-LIQUIDATES": before a
// basket is submitted, the margin it requires NOW and under a configured
// volatility shock is modeled against the deployable funds. If the SHOCKED
// requirement would exceed available margin, the broker's RMS would auto-square-
// off the position the moment volatility spikes — so the basket is flagged and
// BLOCKED pre-submission, never opened (AC-1).
//
// THE LOAD-BEARING INVARIANTS:
//   * CAPABILITY-GATED, FAIL-CLOSED (AC-2): the SPAN/basket margin number is only
//     trustworthy when the broker actually advertises it. SPAN is treated as
//     "available" ONLY when `span_support == capabilities::Support::Supported`
//     AND a non-null `span_margin_source` seam returns a value. `Unknown` (the
//     tri-state fail-closed default) / `Unsupported`, a null source, OR a source
//     that errors all collapse to UNAVAILABLE — we cannot model the shock.
//   * NEVER SUMMED LEGS (AC-2): when SPAN is unavailable a NET-SHORT basket FAILS
//     CLOSED (blocked). We deliberately do NOT approximate by summing per-leg
//     margins — that badly UNDER-counts a short spread's true requirement and is
//     the exact trap FR-18 guards against. A basket that is NOT net-short (a
//     defined-risk / long basket) is not force-liquidatable the same way and is
//     allowed with the sim reported unavailable.
//   * AUDITED ON EVERY PATH (AC-3): the final result — the decision plus the
//     modeled Money — is the evidence trail. The `audit` seam (when present) is
//     ALWAYS handed the result before return, on EVERY path: within-limit,
//     blocked-shock, blocked-unavailable-net-short, and allowed-unavailable. A
//     null audit seam is simply skipped and never crashes.
//
// Conventions: no-throw across the boundary (return an outcome, never propagate;
// never call `.value()` on an errored Result), NO double/float (integer
// domain::Money paise throughout — the crossing test is a strict integer `>`),
// redaction-safe `detail` (only Money.to_string() / outcome / capability names —
// never a secret or raw broker text). Cross-platform: C++20 standard library
// only — NO OS APIs, NO `#ifdef`. Depends inward on `domain` (Money), `errors`
// (Result/Error), and `capabilities` (Support, in the public signature).

#include <functional>
#include <string>
#include <string_view>

#include "broker_exec/capabilities/capabilities.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::options {

// What the broker SPAN/basket-margin source returns: the blocked margin the
// basket requires NOW and under the configured volatility shock. Integer paise
// (domain::Money), no float.
struct ShockMarginModel {
  domain::Money margin_now;
  domain::Money margin_under_shock;
};

// The pre-trade inputs the evaluator weighs the shock against.
//   available_margin   — the deployable funds the broker RMS measures the
//                        requirement against (the auto-square-off line).
//   basket_is_net_short — flags an undefined-risk (force-liquidatable) basket;
//                        drives the AC-2 fail-closed rule when SPAN is absent.
struct MarginShockInputs {
  domain::Money available_margin;
  bool basket_is_net_short;
};

// The terminal decision. Stable, log-friendly names (see to_string):
//   WithinShockLimit          — SPAN available; the shocked requirement FITS
//                               within available margin: basket allowed (AC-1).
//   BlockedMarginShock        — SPAN available; the shocked requirement would
//                               CROSS the RMS auto-square-off threshold (exceeds
//                               available): BLOCK pre-submission (AC-1).
//   BlockedUnavailableNetShort — no usable SPAN source AND the basket is
//                               net-short: FAIL CLOSED, block, never sum legs
//                               (AC-2).
//   AllowedUnavailableBounded — no usable SPAN source but the basket is NOT
//                               net-short (defined-risk/long): allowed, sim
//                               reported unavailable (AC-2).
enum class MarginShockOutcome {
  WithinShockLimit,
  BlockedMarginShock,
  BlockedUnavailableNetShort,
  AllowedUnavailableBounded
};

// Stable, log/serialization-friendly outcome names (NFR-8 observability).
[[nodiscard]] std::string_view to_string(MarginShockOutcome outcome) noexcept;

// The evaluation result — the decision plus the modeled evidence handed to the
// audit seam. `span_available` reflects whether the SPAN gate passed (the source
// produced a value). On the UNAVAILABLE path `margin_now` / `margin_under_shock`
// are zero (`Money::from_paise(0)`) — documented as NOT-MODELED, not "zero
// margin". `available_margin` echoes the input (always set). `blocked` is a
// convenience = the outcome is one of the two Blocked* (the gate reads this).
// `detail` is a redaction-safe summary (Money + outcome/capability names only).
struct MarginShockResult {
  MarginShockOutcome outcome;
  bool span_available;
  domain::Money margin_now;
  domain::Money margin_under_shock;
  domain::Money available_margin;
  bool blocked;
  std::string detail;
};

// The injected seams that keep the evaluator broker-neutral and unit-testable
// with NO real broker.
//   span_margin_source — the broker SPAN/basket-margin source. A null seam, OR a
//                        seam that returns an Error, is treated as UNAVAILABLE
//                        (cannot model => fail-closed for a net-short basket).
//   audit              — the evidence sink: handed the FINAL result before
//                        return on EVERY path (AC-3). A null seam is skipped.
struct MarginShockSeams {
  std::function<Result<ShockMarginModel>()> span_margin_source;
  std::function<void(const MarginShockResult&)> audit;
};

// Evaluate the margin shock for a basket over the injected seams. NO throw:
// every path returns a populated MarginShockResult rather than propagating. The
// steps (see the file header for the invariants):
//   1. CAPABILITY GATE (fail-closed) — SPAN is available ONLY when
//      `span_support == Support::Supported` AND `span_margin_source` is non-null
//      AND the source call returns a value. Anything else => UNAVAILABLE.
//   2. AVAILABLE PATH — record margin_now / margin_under_shock from the model;
//      span_available = true. CROSSING test (AC-1): the basket crosses the RMS
//      auto-square-off threshold iff `margin_under_shock > available_margin`
//      (STRICT integer `>`; exactly-equal is WithinShockLimit). Crossing =>
//      BlockedMarginShock (blocked); else WithinShockLimit.
//   3. UNAVAILABLE PATH (AC-2) — span_available = false; margin_now /
//      margin_under_shock = Money::from_paise(0) (not-modeled). net-short =>
//      BlockedUnavailableNetShort (blocked, never sum legs); else
//      AllowedUnavailableBounded (allowed, bounded risk).
//   4. AUDIT + RETURN (AC-3) — always set available_margin from the input, then
//      call `audit(result)` if present, then return.
[[nodiscard]] MarginShockResult evaluate_margin_shock(capabilities::Support span_support,
                                                      const MarginShockInputs& inputs,
                                                      const MarginShockSeams& seams);

}  // namespace broker_exec::options
