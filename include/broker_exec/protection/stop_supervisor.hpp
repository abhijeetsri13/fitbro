#pragma once

// broker_exec::protection — the LIBRARY-OWNED protective-stop supervisor.
//
// THE WHOLE POINT IS "NEVER TRUST THE BROKER GTT/SL AS A DURABLE STOP". A Kite
// (or any broker) GTT is valid only ONCE and is fire-and-forget: when the GTT
// fires it places a single order and then DELETES itself. If that fired order is
// rejected (LPP / circuit band breach during a fast move) or only partially
// fills, the GTT is GONE and the position is silently UNPROTECTED — the broker
// believes it did its job. The same trap bites a protective MARKET exit sent by
// hand: in a volatile move the exchange rejects it "price out of LPP range" and
// the position keeps bleeding with no stop in force.
//
// This module makes the LIBRARY own protection. On every tick the caller hands
// us the protected position + what the broker currently reports, and we answer a
// single question: is this position STILL actually protected, and if a stop
// FIRED-BUT-DID-NOT-FILL, what fresh, band-aware protective exit must we re-arm
// RIGHT NOW. We never assume the broker GTT protected us; we re-verify it every
// tick and re-arm on any doubt.
//
// THE CORE FIX (ReArmNeeded): trigger crossed AND the protective exit is not
// confirmed Filled (no order / Rejected / Cancelled / Unknown / only partially
// filled) => the position is exposed. We EMIT a fresh protective exit clamped
// INTO the current circuit/LPP band so the exchange cannot reject it for price,
// and raise a Critical alert. A partial fill is NOT "closed": the remainder is
// still naked and must be re-armed.
//
// BAND-AWARE FAIL-CLOSED (the LPP/circuit fix): the re-armed exit price is
// clamped into the known band. If the band is UNKNOWN we still emit the exit
// (an unprotected position is strictly worse than an at-risk price) but we
// escalate to Critical and SAY the limit is unclamped — we never ship an
// unclamped price dressed up as safe; the operator must know.
//
// Pure decision core over plain inputs (no real broker, no transport): the
// caller wires ticks + broker order state in and dispatches the returned exit.
// Conventions (mirrors options::hedge_first): NO throw across the boundary
// (return a decision, never propagate); NO double/float — integer paise Money
// only; redaction-safe `detail`/alert text (symbol/side/Money only, never a
// token or raw broker body); C++20 standard library only, NO OS APIs / `#ifdef`.
// Depends inward only on `domain` (Money/Side/OrderState) and `ports` (AlertSink).

#include <cstdint>
#include <string>
#include <string_view>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/ports/alert_sink.hpp"

namespace broker_exec::protection {

// The current circuit / LPP (Last-Price-Protection) price band for the symbol.
// `valid == false` means the band is UNKNOWN this tick (no fresh quote / data
// gap). Unknown is treated FAIL-CLOSED for the price: we still emit a protective
// exit, but unclamped and at Critical (see evaluate_protection). lower/upper are
// the inclusive band edges; a protective limit outside [lower, upper] would be
// rejected by the exchange, which is exactly the GTT-fired-but-unfilled trap.
struct PriceBand {
  domain::Money lower;
  domain::Money upper;
  bool valid = false;
};

// The protected position + the stop we are supervising for it. `position_qty` is
// SIGNED: +N == long N, -N == short N, 0 == flat (nothing to protect). The exit
// side is always the OPPOSITE of the position: a long (+) exits by Sell, a short
// (-) exits by Buy. `protective_limit` is the marketable limit for the exit (a
// limit placed near/through the trigger so it fills like a market but cannot be
// rejected for price once clamped into the band).
//
// CONTRACT — `position_qty` is the SOURCE OF TRUTH for exposure: it MUST be the
// CURRENT, RECONCILED remaining position (broker fills already applied), NOT the
// size the stop was originally armed at. A position the protective exit actually
// flattened is 0 (=> Closed); a partial fill leaves the remaining magnitude. The
// supervisor never trusts a broker order flag over this: a `Filled`
// protective_order_state arriving while position_qty is still non-zero is treated
// as STILL EXPOSED (re-arm), not as protected — so the caller must keep
// position_qty reconciled or risk an over-exit on a position-feed lag.
struct ProtectiveStop {
  std::string position_id;         // the protected position handle (opaque id)
  std::string symbol;              // tradable symbol (redaction-safe to log)
  std::int64_t position_qty = 0;   // signed: +long / -short; 0 == flat (reconciled)
  domain::Money stop_trigger;      // the stop trigger price
  domain::Money protective_limit;  // limit for the protective exit (marketable)
};

// The supervisor verdict for one tick. Stable, log-friendly names (see
// to_string); renaming a returned name is a breaking observability change.
//   Armed       — position open, the trigger has NOT been crossed. Protection is
//                 latent and nothing needs to be sent this tick.
//   Closed      — position is flat (qty 0) OR the protective exit is confirmed
//                 Filled. Genuinely protected; no emit, no alert.
//   ReArmNeeded — THE CORE FIX. The stop trigger crossed but the protective exit
//                 did NOT fill (no order / Rejected / Cancelled / Unknown /
//                 PartiallyFilled): the position is exposed. Emit a fresh
//                 band-aware protective exit + Critical alert.
//   Unprotected — fail-closed danger signal: the configuration cannot form a
//                 valid exit (non-positive exit quantity / qty overflow). No
//                 safe exit can be emitted; raise Critical so a human acts.
enum class ProtectionState { Armed, Closed, ReArmNeeded, Unprotected };

// Stable, log/serialization-friendly state names (observability contract).
[[nodiscard]] std::string_view to_string(ProtectionState state) noexcept;

// The fresh protective exit to send when ReArmNeeded fires. `side` is the
// OPPOSITE of the position (long->Sell, short->Buy); `qty` is the absolute
// position size; `limit_price` is the protective limit AFTER band clamping (or
// the raw protective limit when the band is unknown — see the fail-closed note).
struct ExitOrder {
  std::string symbol;
  domain::Side side = domain::Side::Sell;
  std::int64_t qty = 0;
  domain::Money limit_price;
};

// The full decision returned every tick. `emit_exit` is true ONLY when `exit`
// holds a protective order the caller must dispatch (ReArmNeeded). `alert` is
// true when a Critical operator alert was attempted (best-effort; see below).
// `detail` is a redaction-safe audit note (symbol / side / Money only — never a
// token or raw broker text).
struct ProtectionDecision {
  ProtectionState state = ProtectionState::Armed;
  bool emit_exit = false;
  ExitOrder exit;
  bool alert = false;
  std::string detail;
};

// The per-tick market + broker-order snapshot the caller feeds the supervisor.
struct StopInputs {
  domain::Money last_price;  // current market price (for audit / detail)
  // Has price crossed the stop trigger in the ADVERSE direction? The caller
  // computes this (long: last_price <= stop_trigger; short: last_price >=
  // stop_trigger) and we TRUST the flag — the caller owns tick semantics and may
  // apply hysteresis/confirmation we must not second-guess.
  bool trigger_crossed = false;
  // The broker's state for the protective exit order. Use OrderState::Unknown
  // when there is no order or its state cannot be determined.
  domain::OrderState protective_order_state = domain::OrderState::Unknown;
  // false == NO protective order has ever been placed for this stop. With the
  // trigger crossed this is the worst case (silently unprotected) and drives a
  // re-arm exactly like a Rejected order.
  bool protective_order_known = false;
  // The current circuit / LPP band used to clamp the re-armed exit price.
  PriceBand band;
};

// Evaluate protection for ONE tick. NO throw: every path returns a populated
// ProtectionDecision rather than propagating (a throwing/dead AlertSink is
// caught and swallowed — the protective decision survives a broken alert
// channel). The logic, in order:
//   1. position_qty == 0            -> Closed (flat; no emit, no alert). This is
//      the ONLY Closed path: exposure is read from the live position, never an
//      order flag (a Filled that flattened the position is reported as qty 0 here).
//   2. derive exit side (long->Sell, short->Buy) + exit qty = |position_qty|.
//      A non-positive qty (overflow / mismatch) -> Unprotected + Critical
//      (fail-closed: we cannot form a valid exit).
//   3. !trigger_crossed             -> Armed (latent; nothing to emit).
//   4. trigger_crossed on a non-flat position -> ReArmNeeded REGARDLESS of the
//      protective order flag (Rejected / Cancelled / Unknown / PartiallyFilled /
//      a stale Filled / never placed): the live position is still exposed. Emit a
//      BAND-AWARE protective exit + Critical alert. THE CORE FIX — never trust the
//      broker GTT (a fired-but-unfilled GTT is already deleted) nor a Filled flag
//      over the live position.
// Band clamp on the emitted exit:
//   * Sell (long exit): limit = clamp(protective_limit, [band.lower, band.upper])
//     (never below the floor, never above the ceiling).
//   * Buy  (short exit): limit = clamp(protective_limit, [band.lower, band.upper])
//     (never above the ceiling, never below the floor).
//   * band unusable (band.valid == false OR an inverted lower>upper band) -> emit
//     at the RAW protective_limit (do NOT skip the exit) but escalate the detail
//     to "band unknown — protective limit
//     unclamped" so the operator knows the price was not made safe.
[[nodiscard]] ProtectionDecision evaluate_protection(const ProtectiveStop& stop,
                                                     const StopInputs& in,
                                                     ports::AlertSink& alerts);

}  // namespace broker_exec::protection
