# IMP-13: Real square_off position flatten + exchange via instrument master

Status: ready-for-dev
Closes two tier-2 items tracked since 2-14: (a) Kite square_off cancels instead
of flattening (Kotak refuses NotSupported); (b) Kite adapter derives exchange
from a symbol heuristic instead of the instrument master.

## Problem

`BrokerPort::square_off` is the EMERGENCY exit used by hedge-first recovery
(5-1), panic kills (3-8), and the protective-stop supervisor. Today Kite's impl
cancels the order and returns ok (a filled position stays OPEN after a
"successful" square-off), Kotak's refuses. A real flatten = cancel working
remainder + place an opposite MARKET (or band-clamped limit) order for the
FILLED quantity, driven by broker-truth position/fill data, idempotent, and
duplicate-safe. Exchange resolution: the Kite adapter guesses NSE/NFO from
symbol shape; the instrument master already knows the real exchange+segment.

## Acceptance Criteria

1. **Flatten semantics (both adapters)**: square_off(client_ref):
   (a) resolve the order and its CANONICAL filled qty (fillnorm semantics,
   broker-truth via fetch, not the local cache);
   (b) cancel the working remainder if any (already-terminal cancel outcome
   tolerated — not an error);
   (c) if filled qty > 0, place ONE opposite-side exit order for exactly the
   filled qty with a deterministic exit client_ref derived from the parent
   (`<parent_ref>#X` style so a replay/retry dedupes via UNIQUE(client_ref)
   and the idempotency index — mirror the slicer child-ref discipline);
   (d) zero filled -> cancel-only is a COMPLETE square-off (ok);
   (e) any UNKNOWN leg -> return the typed indeterminate error (ReconcileFirst),
   never a second exit order.
   The exit order is MARKET by default; if a price band is supplied (optional
   param or injected band source), a band-clamped LIMIT is allowed. Exit orders
   are exempt from entry gates by construction (they go through the adapter,
   not the strategy gate) — document that the DISPATCHER remains the intended
   final chokepoint (tier-2: route through dispatch() when the runtime loop
   lands).
2. **No duplicate exit**: replaying square_off for the same parent (crash
   between cancel and place, or double invocation) never places a second exit
   for the same filled qty: the deterministic exit ref + a fetch-first check
   (exit ref already at the broker -> report ok/in-progress, do not re-place).
   Conformance-style tests prove it (RecordedKiteServer + RecordedKotakServer:
   double square_off -> exactly one exit order on the book; crash-replay sim).
3. **Kotak parity**: same semantics over KotakRestClient; capabilities comment
   updated (SquareOff stays Unknown until tier-2 live, but the impl now exists —
   the runbook flips it; the per-call capability gate from 6-3 still refuses
   until certified — state this composition explicitly in the header).
4. **Exchange via instrument master (Kite)**: the adapter accepts an optional
   exchange-resolver seam (std::function<Result<Exchange/segment string>(symbol)>
   built from refdata::InstrumentMaster::resolve); when present it AUTHORITATIVELY
   supplies exchange/segment for order build + square-off exit; the symbol-shape
   heuristic remains ONLY as the documented fallback when no resolver is wired,
   and emits a distinguishable marker in the wire params? NO - keep wire clean:
   just document fallback. Tests: resolver wins over heuristic; resolver error ->
   fail-closed (no order with guessed exchange when a resolver IS wired).
5. All ctests green; conformance suites extended, not weakened.

## Notes for dev

- fillnorm canonical filled qty already exists — reuse.
- The exit ref suffix must not collide with the slicer's `#<k>` children:
  use `#X` (or `#exit`) and add a collision test vs child_ref parity rules.
- Recorded servers need: positions/orderbook reflecting fills, accept the exit
  order, expose place_count/book for the duplicate assertions.
- Do NOT touch domain types; this is adapter-layer + seams only.
