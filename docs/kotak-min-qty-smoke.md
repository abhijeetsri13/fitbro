# Kotak Neo live min-qty smoke + capability promotion runbook (Story 6.2)

This runbook is the **tier-2 gate** for the Kotak Neo adapter. It mirrors
[`kite-min-qty-smoke.md`](kite-min-qty-smoke.md), with one structural difference
that is the whole point of this document:

> **Tier-1 conformance did NOT flip a single capability.**
> `broker_exec_kotak_conformance_tests` is green, and `kotak_capabilities()` is
> still **entirely `Unknown`**. Nothing in this repository has ever spoken to a
> real Kotak endpoint. Completing the steps below — and only that — is what
> promotes an entry to `Supported`.

That asymmetry is deliberate. Kite's capabilities were promoted on a live smoke
plus documented behavior; Kotak's endpoint paths, `jData` field names, auth
handshake and status vocabulary are all still **our recorded assumption** taken
from the public Neo documentation (see `kotak_transport.hpp`). Green fixtures
prove our parser agrees with our own fixture. They cannot certify a URL nobody
has ever called.

---

## Tier-1: what CI runs (recorded-fixture conformance)

```
ctest -R broker_exec_kotak_conformance_tests
```

The recorded Kotak server (`RecordedKotakServer`, in
`tests/conformance/kotak_conformance_test.cpp`) reproduces, at the Kotak
HTTP/JSON level, every fault the broker-agnostic kit drives: `clean`, `drop_ack`,
`ack_lost_but_placed`, `rate_limit`, `duplicate_fill`, `out_of_order`,
`delay_ack` — plus three Kotak-specific hazards the kit does not model: the
**HTTP-200 `stat:"Not_Ok"` trap**, a **partial fill reported under a "complete"
status**, and a **colliding manual order**.

The kit is reused **verbatim**; only the `BrokerFactory` differs. It asserts
`report.ok()` and `duplicate_orders == 0` across the whole matrix.

**What tier-1 proves:** the adapter's logic — correlation, fail-closed state
mapping, integer-paise money, no blind retry, and zero duplicates *measured from
the recorded broker's own order book*.
**What tier-1 does NOT prove:** that any endpoint, field name, envelope or
capability below is real.

**Gaps tier-1 deliberately leaves open** (verify or accept these at tier-2):

- **The total-outage lane is untested.** `RecordedKotakServer` throttles the order
  endpoints but not the reads, and that is load-bearing rather than cosmetic:
  because `KotakRestClient` downgrades a mutation's 429 to `ReconcileFirst`, a
  throttled Kotak place becomes an UNKNOWN order, and the kit answers an UNKNOWN by
  reading broker truth. Throttling reads too makes the kit report an
  *infrastructure* failure rather than a safety finding. So the case where a 429
  storm blocks **both** the place and the reconcile read is not covered. Exercise
  it live if you can provoke it, and confirm the engine holds the UNKNOWN under an
  alert and makes no progress. (Covering it in CI needs the shared conformance kit
  to treat a failed resolve as a legitimate fail-closed outcome — a change to the
  kit, out of scope for Story 6.2.)
- **Restart recovery** — see "Recovery after a restart" in step 9.
- **`square_off()` is a typed refusal**, not a flatten — see step 6.

### The correlation caveat you are about to test live

Kite anchors an ack-lost order with a `tag` the broker echoes back. **Kotak has
no verified tag echo**, so this adapter deliberately sends **no client tag at
all** and correlates on a two-rung ladder:

1. **broker order id** (`nOrdNo`), bound on a successful ack — strong;
2. **attribute corroboration** on `(symbol, side, quantity)` for an ack-lost
   order — weak, and admitted only when the pairing is unambiguous in *both*
   directions.

Anything ambiguous stays **UNKNOWN with an operator alert**. Step 3a below is
what could replace rung 2 with a real correlation token.

Three consequences you must carry into the live run:

- **A `CORRELATION_TOKEN` match on Kotak is not a token echo.** When rung 2
  succeeds the adapter stamps the recovered `client_ref` onto the row, so
  `UnknownResolver` logs `MatchKind::CorrelationToken`. No broker echoed anything
  — the evidence was attribute corroboration. Read that log line as "order id **or**
  corroborated attributes" until `TagCarry` is resolved in step 3a. (There is no
  field on `domain::Order` to signal a weak match without rippling through
  ports/store/lifecycle, so this is documented rather than encoded.)
- **A row the adapter could not correlate is published without its correlation
  tuple** (`side`/`quantity`/`price` left at defaults; id, symbol, state and fill
  progress are always published). This is deliberate: the resolver's rung 3 re-runs
  attribute corroboration first-match-wins with no ambiguity check, and would
  otherwise overturn the adapter's refusal and adopt a colliding manual order.
- **Correlation state is in-memory only.** See "Recovery after a restart" in step 9.

---

## Tier-2: the live min-qty smoke (operator-run, NOT in CI)

> Pre-req: a funded Kotak Neo account, the operator's consumer key/secret,
> mobile, password and MPIN loaded into the configured `SecretProvider`
> (`kotak_consumer_key`, `kotak_consumer_secret`, `kotak_mobile`,
> `kotak_password`, `kotak_mpin`). **One lot at the instrument's minimum
> quantity** only. Live credentials never enter CI.

Run the steps **in order** — each one gates the next. Abort on the first
mismatch and file the delta rather than working around it.

### Step 1 — Verify the endpoint paths exist at all

Hit each path in `kotak_transport.hpp::endpoints` with a valid session and record
the HTTP status and body shape. A 404 here means the path is wrong, not that the
resource is missing (which is exactly why `map_kotak_error` maps 404 to
`ReconcileFirst`, never `DoNotRetry`).

**Resolves:** nothing on its own — it is the precondition for every step below.
**Capture:** one VCR fixture per endpoint.

### Step 2 — Establish the session, then probe its renewal

Run `KotakSessionEstablisher::establish()` (OAuth → login → 2FA MPIN). Confirm a
**complete** bundle (`access_token`, `token`, `sid`, `hs_server_id`) is persisted
encrypted, and that a wrong MPIN persists **nothing**.

Then let the session age, or deliberately invalidate it, and determine whether it
can be renewed **server-to-server without operator input**.

**Resolves:** `HeadlessSessionRefresh`.
- renews headlessly → `Supported`
- requires a fresh MPIN → `Unsupported` (a *certified* absence — this is the one
  entry a negative result may legitimately promote)

**Capture:** the session-death payload, so `is_session_death()` is tested against
the real text rather than our phrasing guess.

### Step 3 — Place ONE lot at minimum quantity

Submit a single `OrderIntent` (min lot; a liquid near-the-money option or a 1-lot
future) through the **same `dispatch()` path** the core always uses:
record-intent → fsync → send → record. Capture the returned
`BrokerAck{broker_order_id, client_ref}`.

Confirm on the wire that:
- the `jData` field names in `build_place_params()` are accepted verbatim;
- `es` (exchange segment) routed correctly — **note the known mis-route risk**:
  the segment is inferred from the symbol by substring, so a cash equity whose
  name contains `CE`/`PE`/`FUT` (e.g. `PETRONET`) routes to `nse_fo` and will be
  rejected. If this bites, resolve the segment from the instrument master
  (Story 2.6) before promoting `PlaceOrder`.

**Resolves:** `PlaceOrder`.

### Step 3a — Does Kotak echo a client tag?

With the order live, inspect the order book row for **any** field carrying a
value we supplied (`GuiOrdId`, `tag`, `remarks`, …). Then try one additional
place with a candidate tag field added to `jData` and see whether it is
(a) accepted and (b) echoed.

**Resolves:** `TagCarry`.
- accepted **and** echoed → `Supported`; promote it to correlation **rung 2** in
  `KotakBrokerAdapter` and demote attribute corroboration to rung 3, matching the
  Kite ladder and the architecture's match-key precedence (NFR-1).
- rejected or silently dropped → `Unsupported`; the current two-rung ladder is
  the correct final design and the weak rung stays load-bearing.

### Step 4 — Reconcile

Run `fetch_orders()` / the reconciler. Confirm:
- the placed order appears **exactly once** in broker truth;
- it correlates by `nOrdNo` (rung 1);
- the local projection holds exactly one row for the signal;
- the real `ordSt` string is one this adapter **recognizes**. Any status that
  maps to `OrderState::Unknown` is a gap — record the exact string, add it to the
  table in `classify_kotak_status()`, and commit the payload as a fixture. It is
  fail-closed by design, so an unrecognized status is safe but noisy.

**Two parsing gaps that will announce themselves as `Unknown` rows.** Both are
safe (nothing is guessed) but both are *noisy*, and both are cheap to fix once you
have seen the real payload:

- **The order TOTAL must arrive under one of** `qty` / `qt` / `ordQty` / `totQty`.
  Under any other spelling the adapter treats the total as ABSENT — which is
  handled correctly (a working order can then never be reported terminal) but means
  it will never reach `Filled` either. Record the real key and add it.
- **Quantities are parsed STRICTLY as integers.** A broker sending `"50.0"` rather
  than `"50"` is a parse failure, and the row fails closed to `Unknown`. That is
  deliberate — no best-effort number reading — but if Kotak really does format
  quantities as decimals, capture it and widen the parser explicitly with a fixture
  rather than letting every row go Unknown.

**Capture:** the real order-book row for every status the order passes through,
plus one `/positions` and one `/limits` payload (both of those reads fail CLOSED
in their entirety on an unparseable number, so a field-shape surprise there is a
hard read failure, not a noisy row).

### Step 5 — Modify, then cancel

Amend the order (price or quantity) and confirm the `nOrdNo` is preserved and the
book reflects the change. Then cancel it and confirm terminal state.

**Resolves:** `ModifyOrder`, `CancelOrder`.

### Step 6 — Square off

Take the position live (a fill), then flatten it — **by hand, for now.**

`KotakBrokerAdapter::square_off()` does **not** flatten: it returns a typed
`NotSupported` / `DoNotRetry` error and makes no broker call. That is deliberate.
It previously issued a cancel and returned `ok`, which is **fail-open** in exactly
the case the call exists for — against a *filled* position a cancel is a no-op, so
the engine was told "flat" while the position was still on, and stopped managing
it. An explicit refusal sends the operator to the position instead of to a lie.

A real flatten is a MARKET exit, side-flipped and sized off the live net position
(`fetch_positions()` + instrument ref-data). Until that is implemented **and**
exercised here, `SquareOff` stays `Unknown`. A successful live run is *not*
sufficient evidence for this entry — the feature has to exist first.

**Resolves:** `SquareOff` — *only after* the flatten exists.

### Step 7 — Order-update websocket

Connect a real socket, subscribe with `build_subscribe_frame()`, and confirm that
(a) frames arrive, (b) `classify_message()` labels a real order update as
`OrderUpdate`, and (c) the resubscribe-on-reconnect guard re-issues the full set.

**Remember the push rule:** a websocket order update is **never authoritative**.
It may lie about fill state (a partial arrives as an `UPDATE`) and may arrive out
of order or not at all. An exit is sized only off a reconciled read
(`fillnorm::exit_qty_for`). Verify this holds live: force a partial fill and
confirm the engine reads it as `PartiallyFilled` from the reconcile, not from the
push.

**Resolves:** `OrderUpdateWebsocket`.

### Step 8 — Basket / multi-leg margin

Call the multi-leg margin preview endpoint for a two-leg basket and compare its
answer against the sum of the per-leg margins.

**Resolves:** `BasketMargin`. Note that promoting it does **not** relax the margin
safety buffer — the broker margin API is assumed to under-count regardless.

### Step 9 — Assert zero duplicates, end to end

Across steps 3-6 there must be **at most one broker order per signal**. Record
every broker order id observed and diff against the intent log. Any second order
for one signal is a hard **FAIL** and blocks all promotion.

Count from **broker truth** (the order book), not from `client_ref` matches. On
Kotak those are different measurements, and the difference runs the dangerous way:
two duplicate ack-lost orders make the attribute key *ambiguous*, so the adapter
correctly refuses to name either row — and a client_ref-based count would then read
**zero duplicates for the scenario that produced two live orders**. The tier-1
suite has the same split (`ConformanceTally` in the conformance test measures the
book directly for this reason); reproduce it here by diffing order ids.

#### Recovery after a restart — what actually protects you

**Do not expect correlation to survive a restart.** `id_to_ref_` and the pending
set are in-memory only, so after a crash **neither rung can name a previously
placed order**: every such row comes back with an empty `client_ref`. Rehydrating
the maps from the IntentLog on startup is a tracked follow-up, not current
behaviour.

What protects you instead — and what this step verifies — is the durability
posture, not correlation:

1. kill the process between fsync and send (the SIGKILL-harness shape);
2. restart, and confirm the intent is **enumerable** from the IntentLog (it was
   fsync'd before the send, so it cannot be lost);
3. confirm the order is held **UNKNOWN under an operator alert** and that
   **nothing re-fires** — the reconcile-first posture, not an automatic re-match;
4. resolve it by hand against the broker order book, and record how long that took.

A restart that silently *re-attributed* the order would be the surprising outcome
here, not the expected one. If you need automatic post-restart re-attribution
before go-live, the IntentLog rehydration follow-up is the blocker to raise.

---

## Capability promotion table

| Capability | Promoted by | Promote to |
|---|---|---|
| `PlaceOrder` | Step 3 | `Supported` |
| `ModifyOrder` | Step 5 | `Supported` |
| `CancelOrder` | Step 5 | `Supported` |
| `SquareOff` | Step 6 — **blocked**: the flatten is not implemented and the call is a typed refusal | `Supported` |
| `HeadlessSessionRefresh` | Step 2 | `Supported` **or** `Unsupported` |
| `TagCarry` | Step 3a | `Supported` **or** `Unsupported` |
| `OrderUpdateWebsocket` | Step 7 | `Supported` |
| `BasketMargin` | Step 8 | `Supported` |
| everything else | not exercised here | stays `Unknown` |

**Promotion rules, binding:**

1. An entry moves to `Supported` **only** on evidence from a real Kotak endpoint.
   Never from a fixture, never from documentation, never from "it should work".
2. An entry moves to `Unsupported` only on a *certified absence* — we tried and
   the broker demonstrably cannot. "We did not test it" is `Unknown`.
3. Every promotion lands in the **same change** as the captured VCR fixtures, so
   tier-1 thereafter covers the real payload shapes.
4. Until an entry is promoted, `CapabilitySet::require()` rejects it **early**
   (at load / safe-start), never mid-trade. That rejection is a feature.
5. A live run promotes a capability only when the underlying feature EXISTS.
   `SquareOff` is the standing example: no amount of live evidence promotes a call
   that is currently a typed refusal.

`tests/conformance/kotak_conformance_test.cpp` contains a guard test asserting
every capability is still `Unknown`. Promoting an entry will fail that test —
update it deliberately, in the same change as the live evidence.

---

## Paper / dry-run equivalents (same path, no live execution)

Paper and dry-run modes exercise the **identical** gate → `dispatch()` →
reconcile path; only the transport differs:

- **Dry-run:** the transport validates and records but performs no live send. The
  tier-1 conformance test is precisely this shape — a synthetic, no-live-send
  transport (`RecordedKotakServer`) driving the real adapter through the real
  dispatch/reconcile path. The `clean` scenario is the dry-run happy path.
- **Paper:** the same, but the synthetic transport also models fills, so the
  reconcile/lifecycle path runs end to end with simulated executions.

The invariant to verify in every mode is the one tier-1 gates on: **at most one
broker order per client signal.**

---

## Certification status

- **Tier-1 certified** when `broker_exec_kotak_conformance_tests` is green in CI
  (fake-broker-grade fault matrix against the real Kotak adapter via recorded
  fixtures). **This flips no capability.**
- **Tier-2 pending.** Every entry in `kotak_capabilities()` is `Unknown`, so the
  capability gate rejects Kotak order flow. The adapter is **not cleared for live
  order flow** until this runbook has been executed and its capabilities promoted
  on live evidence. [architecture.md#TO-6, #CAP-13]
