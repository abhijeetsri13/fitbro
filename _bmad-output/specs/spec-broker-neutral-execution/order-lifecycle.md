# Order Lifecycle, Idempotency & Retry Safety

Backs **CAP-4, CAP-5, CAP-14**. The library owns the full lifecycle of every order. The state-machine diagram lives in `architecture-diagrams.md`; this file holds the states, idempotency rules, and the safe/dangerous retry classification as the testable contract.

## Lifecycle states

| State | Meaning | Blocks new risky orders? |
|---|---|---|
| `CREATED` | Intent recorded with a unique client-side reference; not yet validated. | no |
| `VALIDATED` | Passed the validation + risk gate (CAP-3). | no |
| `SENT` | Dispatched to the broker; awaiting acknowledgement. | no |
| `ACCEPTED` | Broker acknowledged receipt. | no |
| `OPEN` | Live at the exchange, unfilled. | no |
| `PARTIALLY_FILLED` | Some quantity executed. | no |
| `FILLED` | Fully executed. | no |
| `REJECTED` | Broker/exchange refused the order. | no |
| `CANCELLED` | Cancelled (by user, library, or broker). | no |
| `MODIFIED` | A modification was accepted (transient annotation on an open order). | no |
| `UNKNOWN` | True status uncertain — e.g., dispatch timed out, ambiguous response, crash mid-flight. | **yes** |
| `RECONCILED` | An `UNKNOWN` order resolved to a definite state via broker data. | no |

## Idempotency rules (CAP-4)

Every order intent carries a **unique client-side reference** generated before dispatch and persisted before/at the moment of execution.

| Situation | Required behavior |
|---|---|
| Same order requested twice | Return the existing known order; do not place again. |
| Network timeout after placing order | Mark the order `UNKNOWN`; reconcile before any retry. |
| App restarts after an order request | Recover pending state from persistence; reconcile with broker before resuming. |
| Broker response arrives late | Merge it with the existing order intent (do not create a new order). |
| Strategy fires a duplicate signal | Block or merge per configured behavior. |

**Rule:** never blindly retry `place_order`, `modify_order`, `cancel_order`, or `square_off`.

### UNKNOWN match-key precedence (NFR-1)

The long client-ref is kept local; to resolve an `UNKNOWN` against the broker, matching uses, in precedence order:
1. the **broker `order_id`** captured at/after send (persisted with the intent);
2. a **short correlation token** (e.g. `signal_hash8`, fitting the broker tag/remark limit — Kite ~20 chars) carried on the order *where the broker supports order-tag carry/echo*;
3. **attribute corroboration** (symbol, side, qty, price, time window) — used only to corroborate, never to auto-resolve;
4. otherwise **fail closed**: stay `UNKNOWN`, keep the pause active, alert, require manual/configured recovery.

Order-tag carry/echo is a graceful-degradation capability, not a certification gate: a truncated/dropped tag falls back to `order_id` + corroboration + fail-closed — never a second fire.

## UNKNOWN handling (CAP-5)

When an order enters `UNKNOWN`:

1. **Pause new risky entries for the whole account process** — but **exempt risk-reducing operations** (square-off, cancel, hedge-completion, configured emergency hedge/exit), which remain permitted so a protective leg is never frozen. (Per-strategy narrowing is allowed only if the strategies provably share no net broker position for the instrument.)
2. Do **not** assume failure and do **not** assume success.
3. Reconcile by checking, in order: **order book → trade book → positions**.
4. Resolve to a definite state (`OPEN`, `FILLED`, `REJECTED`, `CANCELLED`, …) and mark `RECONCILED`.
5. Resume risky orders only after resolution; raise an alert throughout.

## Lifecycle apply-ordering (CC-7)

State is applied by the single main-loop writer, so transitions are serialized (no lock). Two rules make concurrent broker views (WebSocket order-update push vs reconciler snapshot) safe:
- **Terminal states are absorbing.** Once `FILLED`/`REJECTED`/`CANCELLED`, a later non-terminal view is dropped and logged (`order.update.stale`), never reopening the order.
- **Transitions are forward-progressing.** A broker view is applied only if its broker-sourced ordering key (order-update sequence/timestamp) is ≥ the last applied for that `client_ref`; a reconciler full-orderbook snapshot is authoritative over a single push.

## Freeze-quantity slicing (parent → child) (CAP-3, IBR-2)

An order whose quantity exceeds the exchange **freeze limit** (from the instrument master, CAP-33) is **sliced** (default) into N child orders, not rejected:
- Each child: qty ≤ freeze, ≥ lot size, with the lot-aligned remainder folded into the final child.
- Each child carries a **deterministic** client-ref `<parent_client_ref>#<k>`, so re-slicing on replay is bit-identical and `UNIQUE(client_ref)` dedupes an already-placed child for free (no double-send on restart).
- The **parent** state is a fold over child states; **any child `UNKNOWN` ⇒ parent `PARTIALLY_PLACED`-with-`UNKNOWN`**, engaging the UNKNOWN-pause. Each child is an ordinary single-order lifecycle; `dispatch()` stays one-fsync-one-send per child.
- Reconciliation/recovery enumerates child intents and folds them back to the parent. Slicing composes with basket legs and respects rate-limit/exit-priority for the burst.

## Double-fault: UNKNOWN + broker unreachable (NFR-2)

When an order is `UNKNOWN` **and** the reconciliation read itself cannot complete (broker down): retry reconciliation on a bounded budget with backoff; on exhaustion, move the order to a first-class terminal state **`MANUAL_INTERVENTION_REQUIRED`** (queryable on `/healthz` + the CLI), fire the dead-man's-switch escalation, and keep the UNKNOWN-pause in force. Do **not** auto-square-off a position whose fill is unconfirmed — exit stays operator-initiated.

## Retry safety classification (CAP-14)

| Safe to retry (idempotent reads) | Dangerous — never blind-retry (state-changing) |
|---|---|
| Fetch order book | Place order |
| Fetch positions | Modify order |
| Fetch holdings | Cancel order |
| Fetch quote | Square off |
| Fetch margin | |
| Fetch instruments | |
| Fetch historical data | |

**Dangerous-operation flow on failure/timeout:** `mark state UNKNOWN → reconcile with broker → decide next action`. Reads may retry transparently with backoff.
