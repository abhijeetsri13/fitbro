# Kite live min-qty smoke + paper / dry-run runbook (Story 2.14)

This runbook covers the **two-tier certification** of the Kite adapter:

- **Tier-1 (CI, automated):** the broker-agnostic conformance kit runs against the
  real `KiteBrokerAdapter` over a recorded, fault-injecting Kite HTTP fixture
  (`tests/conformance/kite_conformance_test.cpp`). No network, no live credentials.
  This is the same kit that certifies the FakeBroker — reused verbatim — and it
  proves the **zero-duplicate-orders** invariant (NFR-3) across the full fault
  matrix. CI keys on it. See [architecture.md#FR-37, #TO-6].
- **Tier-2 (operator-run, NOT in CI):** the live min-qty smoke below, plus live-SDK
  verification that resolves any real `unknown` capability/error by capturing the
  real Kite payloads as committed VCR fixtures. **Live credentials never live in
  CI**; this tier is a production-checklist step run by an operator. [architecture.md#TO-6]

---

## Tier-1: what CI runs (recorded-fixture conformance)

```
ctest -R broker_exec_kite_conformance_tests
```

The recorded Kite server (`RecordedKiteServer`) reproduces, at the Kite-HTTP/JSON
level, every fault the kit drives: `clean`, `drop_ack`, `ack_lost_but_placed`,
`rate_limit`, `duplicate_fill`, `out_of_order`, `delay_ack`. The headline danger —
`ack_lost_but_placed` — records the order at the broker (it appears in `/orders`
carrying its correlation `tag`) but returns a 5xx to the caller, so the dispatcher
marks the order `Unknown` and the `UnknownResolver` reconciles it against broker
truth instead of firing a duplicate. The kit asserts `report.ok()` and
`duplicate_orders == 0`.

---

## Tier-2: live min-qty smoke (operator-run)

> Pre-req: a funded Kite account, the operator's API key, and a fresh daily
> `access_token`. **One lot at the instrument's minimum quantity** only.

1. **Establish the session.** The operator runs the Kite login flow (request_token
   → exchange for the daily `access_token`) and loads it into the configured
   `SecretProvider` (`kite.api_key`, `kite.access_token`). No token is logged or
   stored in plaintext; errors are scrubbed (`domain::scrub`).
2. **Safe-start.** Bring the engine up in its normal start posture (funds-freshness
   gate green, instrument master loaded, idempotency index rebuilt from the intent
   log). Confirm there are no pre-existing `Unknown` orders.
3. **Place ONE lot at min qty.** Submit a single `OrderIntent` (min lot, a liquid
   near-the-money option or a 1-lot future) through the SAME `dispatch()` path the
   core always uses: record-intent → fsync → send → record. Capture the returned
   `BrokerAck{broker_order_id, client_ref}`.
4. **Reconcile.** Run `fetch_orders()` / the reconciler. Confirm the placed order
   appears exactly **once** in broker truth, correlated by `broker_order_id` (or by
   the `tag → client_ref` map if the ack was lost), and that the local projection
   holds exactly one row for the signal.
5. **Square off.** Flatten the position (cancel if still working, else a market
   exit) and confirm flat.
6. **Assert zero duplicates.** Across place → reconcile → square-off there must be
   **at most one broker order per signal**. Record the broker order ids observed and
   diff against the intent log. Any second order for the same `client_ref` is a
   FAIL.

During the smoke, capture the real Kite JSON for each order `status` and any error
payloads encountered and commit them as VCR fixtures so Tier-1 covers the exact
real shapes (resolving any `unknown` status to a mapped `OrderState`, fail-closed
to `OrderState::Unknown` otherwise).

---

## Paper / dry-run equivalents (AC-3: same path, no live execution)

Paper and dry-run modes exercise the **identical** gate → `dispatch()` →
reconcile path; only the transport differs:

- **Dry-run:** the transport is a no-op that **validates and records** the intent
  but performs **no live send** — it returns a synthetic accepted ack (or a no-op
  for reads). Because the safety core above the transport is unchanged, the same
  conformance-style assertions hold: the intent is durably recorded, the order is
  enumerable, and **zero duplicates** are produced. The Tier-1 conformance test is
  precisely this shape: a synthetic, no-live-send transport (`RecordedKiteServer`)
  driving the real adapter and the real dispatch/reconcile path — the `clean`
  scenario is the dry-run happy path.
- **Paper:** same as dry-run but the synthetic transport also models fills, so the
  reconcile/lifecycle path is exercised end-to-end with simulated executions. No
  live order ever reaches the exchange.

The invariant to verify in every mode is the same one Tier-1 gates on: **at most
one broker order per client signal.**

---

## Certification status

- **Tier-1 certified** when `broker_exec_kite_conformance_tests` is green in CI
  (fake-broker-grade fault matrix against the Kite adapter via recorded fixtures).
- **Tier-2** (live min-qty smoke + real-payload VCR fixtures) is the operator-run
  production-checklist step and is explicitly **out of CI** (no live creds in CI).
  [architecture.md#TO-6]
