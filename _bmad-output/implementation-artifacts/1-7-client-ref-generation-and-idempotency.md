# Story 1.7 — Client-ref generation and idempotency

**Status:** review

## Story

As a strategy author,
I want each order intent uniquely referenced with duplicate detection,
So that a repeated request never creates a second order. (FR-7)

## Acceptance Criteria

1. A client-ref is `"<strategy>-<sig8>-<uuid>"`, with the deterministic child form
   `"<parent>#<k>"` reserved for slicing.
2. Submitting the same intent twice (including after a restart) returns the existing
   order via the in-memory index + `UNIQUE(client_ref)` — exactly one order.
3. A deterministic signal hash catches a duplicate strategy signal.

## Module ownership

New module `broker_exec::idempotency` only:
- `include/broker_exec/idempotency/idempotency.hpp`
- `include/broker_exec/idempotency/uuid.hpp`
- `src/idempotency/idempotency.cpp`
- `src/idempotency/uuid.cpp`
- `src/idempotency/idempotency_test.cpp`
- `src/idempotency/CMakeLists.txt` (target `broker_exec_idempotency` + alias `broker_exec::idempotency`)

Composes (does not modify): `domain` (OrderIntent/Order), `intentlog` (vendored SHA-256 +
IntentRecord), `store` (insert/find_order + UNIQUE(client_ref)), `errors`/`result`.

## Tasks

- [x] `signal_signature(OrderIntent)` — deterministic SHA-256 over the order-defining
      fields (strategy, symbol, side, quantity, price-in-paise, order_type, product),
      tag-and-separator canonical form, `client_ref` excluded. Reuses `intentlog::sha256_hex`.
- [x] `make_client_ref(strategy, signature_hex, uuid)` -> `"<strategy>-<sig8>-<uuid>"`
      (`sig8` = first 8 hex of the signature) + `sig8_of()` helper.
- [x] `child_ref(parent, k)` -> `"<parent>#<k>"` (k clamped to >= 1, deterministic);
      `is_child_ref()`, `parent_of()` parse helpers for the FSM (Story 1.8) / slicer (Story 2.9).
- [x] `UuidGenerator` seam + `RandomUuidGenerator` (random_device-seeded mt19937_64,
      never time-based) + `SeededUuidGenerator` (deterministic for tests/replay);
      canonical 8-4-4-4-12 lowercase-hex v4 via `format_uuid_v4()`.
- [x] `IdempotencyIndex` — signature -> client_ref; `rebuild_from_log(records)` (PlaceOrder
      only), `existing_ref`, `register_ref`/`register_signature` (first-writer-wins), `size`.
- [x] `intent_payload_json(intent)` — the canonical PlaceOrder log payload carrying the
      precomputed `sig` so rebuild needs no domain types.
- [x] `reserve(index, store, uuids, strategy, intent)` -> `Reservation{client_ref, is_new,
      existing}`: index hit -> existing; mint + UNIQUE(client_ref) store backstop on a torn
      restart -> existing; else register + new.
- [x] CMake target `broker_exec_idempotency` (STATIC) + tests, mirroring `src/platform`.
- [x] Tests (a) format/child round-trip/uuid, (b) signature determinism, (c) dedup = one
      order + UNIQUE backstop, (d) restart rebuild-from-log.

## Dev Notes

### Signature determinism (AC-3)
`signal_signature` hashes only the order-*defining* fields, each tagged
(`strategy=…\x1fsymbol=…\x1fside=…\x1fqty=…\x1fprice=…\x1forder_type=…\x1fproduct=…`) and
joined with the `0x1f` unit separator so field boundaries are unambiguous. Money/price use
exact integer paise — **no float** anywhere (binding convention). `client_ref` is excluded
(it is idempotency's *output*, not an input). Same intent -> same 64-char hex; any one
order-defining field change -> a different hex (one assertion per field in the tests).

### Rebuild-from-log assumption (the load-bearing decision)
The intent-log `payload_json` is **opaque** to the log itself — Story 1.5 imposes no schema
(its own tests append `{"qty":1,"side":"BUY"}`, `{}`, `{"qty":5}`, …). So on replay the
**only reliably-present field across all PlaceOrder records is `client_ref`**; a signature is
**not** recoverable from an arbitrary payload.

Decision: this module **owns a canonical PlaceOrder payload** (`intent_payload_json`) that the
live dispatch path (Story 1.9) is expected to record. It is a compact JSON object carrying a
`schema` marker, the precomputed `sig`, and the order-defining fields. `rebuild_from_log`:
- considers `IntentOp::PlaceOrder` records only;
- parses the payload non-throwing; requires `schema == 1` and a string `sig`;
- maps `sig -> client_ref` (first-writer-wins);
- **skips** any record it cannot interpret (opaque/foreign/parse-error) — it never guesses a
  signature from arbitrary bytes (a wrong guess could mask a real duplicate, violating NFR-3).

This keeps it correct and well-documented rather than silently guessing. If the dispatcher
records a different payload, rebuild recovers nothing for that record and dedup falls back to
the `UNIQUE(client_ref)` store backstop (still zero duplicates, just no signature-level catch).
The canonical payload is the integration contract the dispatcher must honor.

### Three layers of dedup (defense in depth, NFR-3)
1. Signal signature — catches a duplicate signal before a ref is even minted.
2. In-memory index — signature -> ref, rebuilt from the log on boot (restart dedup).
3. `UNIQUE(client_ref)` — the store's hard backstop; `reserve` also probes `find_order` on the
   minted ref so a torn restart (order in store, index not rebuilt) still returns the existing
   order instead of re-dispatching.

### UUID approach (cross-platform)
`<random>` only: `RandomUuidGenerator` seeds `std::mt19937_64` from a `std::random_device`
`seed_seq` (never time-based — binding constraint). `format_uuid_v4` forces the RFC-4122
version (`4`) and variant (`10xx`) bits and emits canonical 8-4-4-4-12 lowercase hex.
`SeededUuidGenerator` gives reproducible refs for tests/replay. No OS APIs, no `#ifdef`.

### CMake / linking
`broker_exec_idempotency` STATIC + alias. PUBLIC include `${PROJECT_SOURCE_DIR}/include`;
PRIVATE include `${PROJECT_SOURCE_DIR}/src/intentlog` so the vendored `sha256.hpp` (not a
public header) resolves — the symbol comes from the linked `broker_exec_intentlog`. Links
`broker_exec_domain` PUBLIC (OrderIntent/Order in the API); PRIVATE `broker_exec_errors`,
`broker_exec_intentlog`, `broker_exec_store`, `nlohmann_json::nlohmann_json`,
`broker_exec_warnings`, `broker_exec_sanitizers`. Tests add `Catch2::Catch2WithMain` +
`broker_exec_clock` (TestClock for the restart test) and register with `add_test`.

### Consumers (binding names)
- Story 1.8 (FSM): `is_child_ref` / `parent_of` to fold a child slice into its parent.
- Story 2.9 (freeze-slicer): `child_ref(parent, k)` for bit-identical re-slicing + UNIQUE dedupe.

## Completion Record

**Files created**
- `include/broker_exec/idempotency/idempotency.hpp`
- `include/broker_exec/idempotency/uuid.hpp`
- `src/idempotency/idempotency.cpp`
- `src/idempotency/uuid.cpp`
- `src/idempotency/idempotency_test.cpp`
- `src/idempotency/CMakeLists.txt`

**Orchestrator wiring** (top-level `CMakeLists.txt`, NOT edited by this story):
`add_subdirectory(src/idempotency)`

**Not built locally** (orchestrator builds centrally). Code targets MSVC `/W4 /permissive- /WX`
and gcc/clang `-Wall -Wextra -Wpedantic -Werror -Wshadow`; cross-platform C++20 stdlib +
nlohmann_json only; no OS APIs / `#ifdef`.
