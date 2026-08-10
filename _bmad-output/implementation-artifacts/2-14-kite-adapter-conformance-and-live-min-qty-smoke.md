# Story 2.14: Kite adapter conformance and live-min-qty smoke

Status: ready-for-dev

## Story

As a platform maintainer,
I want the Kite adapter certified and smoke-tested live with minimum quantity,
so that real-money Kite trading is proven before scale.

## Acceptance Criteria

1. **Given** the Kite adapter **When** the conformance kit runs against it (recorded fixtures) **Then** it passes
   green and every Kite `unknown` capability/error is resolved with a committed fixture.
2. **And** a documented live-min-qty smoke places, reconciles, and squares off one lot with zero duplicates.
3. **And** paper and dry-run modes run the same path without live execution.

## Tasks / Subtasks

- [ ] Task 1: `KiteBrokerAdapter : ports::BrokerPort` (AC: 1) — the real adapter
  - [ ] `include/broker_exec/adapters/kite/kite_broker_adapter.hpp` + `src/adapters/kite/kite_broker_adapter.cpp`; add to
        `broker_exec_kite`. Wraps a `KiteRestClient` (Story 2.3) and translates Kite JSON ↔ domain types.
  - [ ] `place(intent)`: POST place; on success parse `data.order_id` -> `BrokerAck{broker_order_id, client_ref=intent.client_ref}`.
        Send a short correlation `tag` derived from the client_ref (Kite tag ≤ ~20 chars — use the `sig8`/a short token) AND
        register `tag -> client_ref` in an in-memory correlation map BEFORE returning, so an ack-lost order is still
        recoverable via fetch_orders (the architecture's match-key precedence: order_id > short correlation token). On a
        transport/Kite error -> the typed taxonomy Error (already scrubbed) so the dispatcher marks UNKNOWN + reconciles.
  - [ ] `modify(broker_order_id, intent)` / `cancel(broker_order_id)` / `square_off(broker_order_id)`: map to the REST client.
  - [ ] `fetch_orders()`: GET orderbook; for each Kite order build a `domain::Order` — recover `intent.client_ref` from the
        correlation map via the order's `tag` (fallback: the broker_order_id->client_ref map populated on a successful ack);
        map Kite `status` -> `OrderState`, fill qty/avg price (integer paise, no float). `fetch_trades()`/`fetch_positions()`/
        `fetch_funds()`: translate the respective Kite payloads to domain types / `FundsSnapshot` (paise).
  - [ ] Kite `unknown`s resolved with committed fixtures: every status string / error_type the adapter maps is covered by a
        recorded fixture (AC-1) — an unmapped Kite status falls to `OrderState::Unknown` (fail-closed), never a wrong state.
- [ ] Task 2: Recorded fault-aware Kite transport (test support) (AC: 1)
  - [ ] In the conformance test (or a small test header), a `RecordedKiteServer : adapters::kite::HttpClient` — a STATEFUL,
        deterministic fake Kite HTTP endpoint that models an order book at the JSON level and reproduces the fault matrix the
        kit drives (it is the Kite-HTTP analog of the FakeBroker). Given a `fake::FaultConfig`, POST place / GET orders behave:
        clean = place returns order_id + the order appears COMPLETE in /orders; `ack_lost_but_placed` / `drop_ack` =
        POST returns a 5xx/transport-shaped response (caller sees failure) BUT the order IS recorded and appears in /orders
        (the headline duplicate-risk); `rate_limit` = POST 429 and NOTHING recorded; `duplicate_fill` = two trades, one order;
        `out_of_order` = reversed views; `delay_ack` = POST 5xx until the injected clock advances N ticks, then success — and
        in EVERY case the recorded order carries the request's `tag` so the adapter recovers the client_ref. Deterministic
        (injected ClockPort, internal counters; no real time/rand/#ifdef).
- [ ] Task 3: Kite conformance test (AC: 1) — `tests/conformance/kite_conformance_test.cpp`
  - [ ] A `conformance::BrokerFactory` that, per scenario, builds a `RecordedKiteServer` for the given FaultConfig + a
        `KiteRestClient` over it + a fake `SecretProvider` (synthetic api_key/access_token, NO live creds) and returns a
        `KiteBrokerAdapter`. Call `conformance::run_conformance(factory)`; assert `report.ok()` and `report.duplicate_orders == 0`
        across the WHOLE matrix — the SAME kit that certifies the FakeBroker, now gating Kite (reused verbatim).
  - [ ] (orchestrator wires a `broker_exec_kite_conformance_tests` target in tests/CMakeLists.txt linking broker_exec_kite +
        the conformance stack.)
- [ ] Task 4: Paper / dry-run same-path (AC: 3) + smoke runbook (AC: 2)
  - [ ] A test (in the kite module or the conformance test) proving the SAME place→reconcile path runs without live execution:
        e.g. a `dry-run` flag on the adapter (or a thin wrapper) that validates/records but the transport performs NO live
        send (returns a synthetic accepted ack / a no-op), and the conformance-style assertions still hold (zero duplicates).
        Keep it minimal — paper/dry-run as a transport-level no-op is enough to prove the path is identical.
  - [ ] `docs/kite-min-qty-smoke.md`: the documented live-min-qty smoke runbook — establish session (operator request_token),
        safe-start, place ONE lot at min qty, reconcile, square off, assert zero duplicates; and the paper/dry-run equivalents.
        (Live execution is operator-run, NOT in CI — no live creds in CI.)
- [ ] Task 5: Mark the Kite adapter certified (tier-1) (AC: 1)
  - [ ] The conformance test passing in CI is tier-1 certification (fake-broker-grade fault matrix against the Kite adapter via
        recorded fixtures). Note in the test/docs that tier-2 (live-SDK verification resolving real `unknown`s + capturing real
        error payloads as VCR fixtures) is the operator-run production-checklist step. [architecture.md#TO-6]

## Dev Notes

- **Reuse the kit verbatim** (broker-agnostic): pass a different `BrokerFactory`; the kit's PASS/FAIL logic is identical.
  [tests/conformance/conformance_kit.hpp, architecture.md#FR-37, #TO-6]
- **Correlation for ack-lost recovery**: the adapter registers `tag -> client_ref` at place time so fetch_orders recovers the
  signal even when the ack was lost — this is what lets the resolver dedupe (match-key precedence: order_id > short token).
  [architecture.md#NFR-1 client-ref↔broker anchor, #B UNKNOWN match precedence]
- **No live creds in CI**: a fake SecretProvider + the RecordedKiteServer; tier-2 live verification is operator-run. [architecture.md#TO-6]
- **No float / typed errors / scrubbed**: integer paise; Kite errors via the taxonomy (Story 2.3 map_http_error); no secret in any error.
- **Reuse:** `ports::BrokerPort`/`BrokerAck`/`FundsSnapshot`, `adapters::kite::KiteRestClient`/`HttpClient`,
  `conformance::run_conformance`, `adapters::fake::FaultConfig` (as the factory's fault selector), domain types.

### References
- [Source: epics.md#Story 2.14] [architecture.md#FR-37, #TO-6, #NFR-1, #B] [Source: tests/conformance/conformance_kit.hpp, include/broker_exec/adapters/kite/*]
- [Source: docs/conventions.md]

## Dev Agent Record
### Agent Model Used
Opus 4.8 (1M context) — BMAD dev agent.

### Completion Notes List
- PART A — production adapter `KiteBrokerAdapter : ports::BrokerPort` over `KiteRestClient`.
  `place()` registers a deterministic short `tag -> client_ref` (FNV-1a, 18 chars) BEFORE the
  REST call so an ack-lost order is recoverable; on success registers `broker_order_id -> client_ref`.
  `fetch_orders()` recovers client_ref (id map first, then tag), maps Kite `status -> OrderState`
  (COMPLETE→Filled, REJECTED→Rejected, CANCELLED→Cancelled, OPEN/TRIGGER PENDING/…→Sent, else
  Unknown/fail-closed), integer-paise prices via a no-float decimal parser. No throw, no float,
  no secret in errors.
- PART B — `tests/conformance/kite_conformance_test.cpp`: a stateful `RecordedKiteServer : HttpClient`
  (Kite-HTTP twin of FakeBroker) reproducing the fault matrix; an `OwningKiteAdapter` that owns the
  server + REST client + secret provider + adapter behind one BrokerPort; the kit is reused verbatim
  and asserts `report.ok()` + `duplicate_orders == 0` across all 7 scenarios.
- `docs/kite-min-qty-smoke.md`: tier-1 (CI recorded conformance) + tier-2 (operator live min-qty
  smoke) runbook, with the paper/dry-run same-path equivalence (AC-3).

### File List
- include/broker_exec/adapters/kite/kite_broker_adapter.hpp (new)
- src/adapters/kite/kite_broker_adapter.cpp (new)
- src/adapters/kite/CMakeLists.txt (modified — added kite_broker_adapter.cpp to broker_exec_kite)
- tests/conformance/kite_conformance_test.cpp (new)
- docs/kite-min-qty-smoke.md (new)
