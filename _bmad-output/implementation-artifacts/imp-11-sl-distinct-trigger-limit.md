# IMP-11: Stop-loss orders carry a distinct trigger price (domain ripple)

Status: ready-for-dev
Tracked since Epic-2 tier-2 list. FR-6/FR-9 · gate 2-8 · adapters 2-14/6-2 · priceband IMP-6.

## Problem

`domain::OrderIntent` has one `price`. Kite/Kotak SL (stop-loss-limit) orders need
BOTH `trigger_price` (activation) and `price` (limit after trigger); SL-M needs
`trigger_price` only. Today adapters can only send one number — a live SL order
built from an intent either mis-uses `price` as trigger or cannot express the pair.
The 2-8 gate already tick-validates a trigger for StopLossMarket, and IMP-6
priceband clamps both — but the DOMAIN cannot carry the pair, so those checks run
on synthesized values. This is the known "ripples across all constructors" change.

## Acceptance Criteria

1. `OrderIntent` (and any order-shaped domain type that mirrors it) gains
   `std::optional<Price> trigger_price` (absent = not a stop order). Invariants
   enforced at the validation gate: StopLoss (SL) REQUIRES both trigger+price;
   StopLossMarket (SL-M) REQUIRES trigger and IGNORES/forbids limit price;
   Limit/Market FORBID trigger (fail-closed Validation errors, exits still exempt
   from entry-only blocks but never from shape validation). Trigger tick-checked
   same as price (the existing 2-8 SL-M check now reads the REAL field).
2. Adapters map the pair to the wire: Kite `trigger_price` form field; Kotak
   jData `trgPrc`. Recorded servers echo/accept them; conformance stays green.
   Round-trip: fetch_orders parses the broker trigger back into the domain order
   where the payload carries it (optional stays nullopt when absent).
3. Priceband (IMP-6) + modifyguard consume the REAL trigger field (drop any
   synthesized/duplicated trigger params in their signatures where they existed
   only because the domain lacked the field — keep API compat where cheap).
4. Slicer/basket/hedge paths propagate trigger_price to children unchanged
   (child intents inherit parent trigger; slicing must not drop it).
5. Whole tree builds; ALL existing tests stay green (update constructors/fixtures
   mechanically); new tests: gate shape matrix (SL/SL-M/Limit/Market ×
   with/without trigger), tick-check on trigger, adapter wire-field emission both
   brokers, round-trip parse, slicer propagation.

## Notes for dev

- `OrderIntent` is an aggregate — adding an optional field with a default does
  NOT break aggregate init unless code uses positional init covering all fields:
  grep for brace-init of OrderIntent and fix positionally-dependent sites.
- intent-log/store serialization: if OrderIntent is serialized (intentlog),
  version-stamp compat: absent field on replay of old records -> nullopt (check
  the intentlog schema handling; do not break replay of committed fixtures).
- Do NOT widen scope: no new order types, no GTT, no trailing stops.

## Residual risk accepted at implementation (NOT closed by the ACs)

STOP ORDERS DO NOT SURVIVE THE UPGRADE UNCHANGED, and the spec should not be read
as claiming otherwise. A pre-IMP-11 stop stored its activation level in `price`;
this build stores it in `trigger_price` (and canonicalizes an SL-M's untransmitted
`price` to 0). `idempotency::signal_signature` hashes those fields, so the SAME
economic stop hashes DIFFERENTLY across the upgrade: restart dedupe misses it and
the working stop can be placed a SECOND time — both fire, and the position
inverts rather than going flat.

No signature scheme closes this; the two builds genuinely disagree about which
field carries the level. What IS closed:

- NON-stop orders (Market / Limit) are byte-stable — the trigger contributes to
  the canonical bytes only when present, and a golden-hex test pins it.
- The residual is caught OPERATIONALLY at cold boot rather than left to a release
  note: `session::require_no_legacy_stops` (wired as
  `SafeStartContext::legacy_stop_check`) refuses to start while the projection
  holds a WORKING stop with no trigger — a combination only the old binary could
  have written, since migration 2 backfills NULL and the gate refuses
  trigger-less stops. Rollback is symmetric.
- Procedure + rollback: `docs/upgrade-imp-11-stops.md`.
