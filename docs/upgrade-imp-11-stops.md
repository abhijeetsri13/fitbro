# Upgrade procedure: IMP-11 (stop orders gain a distinct trigger price)

**Applies to:** any deployment upgrading across the release that adds
`domain::OrderIntent::trigger_price` (store schema 1 -> 2).

**One-line summary:** flatten or cancel every outstanding stop order BEFORE you
deploy, in either direction. The engine enforces this at cold boot and will
refuse to start if you skip it.

## Why this one needs a procedure

Almost every schema change in this codebase is transparent. This one is not, and
the reason is worth stating plainly rather than burying.

A stop order placed by a **pre-IMP-11** binary stored its activation level in
`OrderIntent::price`, because there was nowhere else to put it. The **post-IMP-11**
binary stores that level in `OrderIntent::trigger_price`, and canonicalizes an
SL-M's meaningless `price` to zero.

The order's *signal signature* — the SHA-256 over its order-defining fields that
makes restart dedupe work (`idempotency::signal_signature`) — is computed from
those fields. So the two binaries compute **different signatures for the same
economic order**.

The consequence is a duplicate-order hazard, which is the one class of bug this
whole library exists to prevent:

1. The old binary places a stop; the intent log records signature `A`.
2. You deploy the new binary and it replays the log.
3. The strategy re-emits the same stop. The new binary computes signature `B`.
4. `B` matches nothing in the rebuilt index, so `reserve()` reports a **fresh
   signal**, mints a new `client_ref`, and sends it.
5. Two live stops now sit on the same position. Both fire. The position does not
   go flat — it **inverts**, leaving you naked in the opposite direction.

No signature scheme can fix this. The two builds genuinely disagree about which
field holds the stop level, and a hash cannot reconcile a disagreement about
meaning. Non-stop orders (Market / Limit) are unaffected: their canonical bytes
are byte-identical across the upgrade, and a golden-hex test pins that.

## The procedure

1. **Stop emitting new orders.** Put the strategy into a no-entry posture.
2. **Flatten or cancel every outstanding stop** (SL and SL-M) at the broker,
   using the OLD binary. Plain Limit/Market orders may be left working.
3. Confirm the order book holds no working stop.
4. **Deploy the new binary.** Store migration 2 runs automatically on first open
   and backfills `orders.trigger_price_paise` as `NULL` for every existing row.
5. Start. The cold-boot gate verifies step 2 for you (below).

## What the engine enforces for you

`session::require_no_legacy_stops`, wired into `SafeStartGate` as the
`legacy_stop_check`, scans the projection for the exact fingerprint of a
pre-upgrade stop:

* order type is `StopLoss` or `StopLossMarket`, **and**
* `trigger_price` is absent, **and**
* the order is still **working** (not Filled / Rejected / Cancelled).

A new binary can never produce that combination — the validation gate refuses a
trigger-less stop outright — so any row matching it was written by the old build.
On a hit the gate returns a fail-closed `Validation` / `BlockStrategy` error
naming the count and the first offending `client_ref`, and the runtime does not
trade. Terminal stops are ignored: they cannot fire again, so blocking on them
would wedge the gate forever on a database that merely *remembers* an old stop.

## Rollback is symmetric

Rolling **back** past this release carries the same hazard in the same shape, so
the same step 2 applies before you roll back:

* a stop placed by the new binary carries its level in `trigger_price`, which the
  old binary cannot read at all — it would see a stop with a meaningless `price`;
* independently, the old binary **refuses to open a schema-2 database** (a newer
  schema is a hard refuse-to-start, by design), so a rollback needs a restored
  pre-upgrade projection or a rebuild from the intent log.

**Flatten stops before moving in either direction.**
