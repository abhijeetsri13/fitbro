---
story: "1.9"
title: dispatch() chokepoint with no-blind-retry
epic: "Epic 1: Order-Safety Substrate"
status: review
module: runtime
target: broker_exec_runtime
frs: [FR-8, FR-10]
nfrs: [NFR-1, NFR-2, NFR-3]
---

# Story 1.9 — dispatch() chokepoint with no-blind-retry

## Status

review

## Story

As an operator,
I want every broker mutation to flow through one synchronous chokepoint,
So that fsync-before-send and no-blind-retry are guaranteed structurally. (FR-8, FR-10)

This is the **safety centerpiece** of Epic 1: a single synchronous, single-threaded
path — `runtime::Dispatcher` — through which every place / modify / cancel /
square-off flows as **record-intent → fsync → [pre_send_barrier] → send →
record-result|UNKNOWN**, with **no thread hand-off between the fsync and the
send**, and which **never blindly repeats a dangerous operation**.

## Acceptance Criteria

1. `dispatch` performs record-intent → `fsync` → send → record result|UNKNOWN with
   **no thread hand-off** between the fsync and the send (synchronous,
   single-threaded). — MET. `Dispatcher::place/modify/cancel/square_off` each call
   `log_.append(...)` (which `fflush`+`durable_sync`es before returning) → an
   injectable `pre_send_barrier()` → `broker_.*(...)` → `append_result(...)`, all
   inline on the calling thread. No `std::thread`/async anywhere in the module.
2. A timeout/failure on place/modify/cancel/square-off **marks the order UNKNOWN
   and reconciles — never an immediate repeat.** — MET. A `ReconcileFirst`
   (Timeout/Network) — or an ambiguous `Unknown` — error sets the order to
   `domain::OrderState::Unknown`, persists it (`insert_order`/`upsert_order`),
   appends a `Result(unknown)` record, and returns **without any retry path**. A
   clean `DoNotRetry`/non-reconcile verdict yields `Rejected` (place) or surfaces
   the typed `Error` (modify/cancel/square-off). There is **no loop and no second
   send** in any branch.
3. **No code path sends to a broker except through `dispatch()`** (enforced). —
   MET structurally: the `Dispatcher` holds the runtime's **only**
   `ports::BrokerPort&` (a private member), reachable solely through the four
   public mutation methods; convention (`conventions.md`: "The single
   broker-mutation path is dispatch()") binds it; and a behavioral test proves it
   by asserting `FakeBroker::request_count()` advances by **exactly one per
   mutation** and **zero on a duplicate submit**.

## Tasks

- [x] New module `runtime` (header `include/broker_exec/runtime/dispatcher.hpp`,
      source `src/runtime/dispatcher.cpp`, `src/runtime/CMakeLists.txt` target
      `broker_exec_runtime` + alias `broker_exec::runtime`).
- [x] Implement `Dispatcher` with the committed API (place/modify/cancel/
      square_off + `set_pre_send_barrier`).
- [x] PLACE: idempotent `reserve()` → duplicate returns existing order with ZERO
      sends; else append `PlaceOrder` (canonical `intent_payload_json`) → fsync →
      barrier → `broker.place` → success (persist + FSM Sent→Acknowledged +
      Result) | UNKNOWN-on-reconcile-first | Rejected-on-clean.
- [x] MODIFY/CANCEL/SQUARE-OFF: record op intent → fsync → barrier → send →
      Result|UNKNOWN, never retried.
- [x] Co-located tests (`src/runtime/dispatcher_test.cpp`) covering (a) happy
      place, (b) fsync-before-send via the barrier, (c) idempotency/zero second
      send, (d) no-blind-retry (ack-lost-but-placed → Unknown, sent once,
      broker truth shows the order), (e) cancel/square-off happy + UNKNOWN-on-
      timeout, and a clean-rejection case.
- [x] BMAD story file (this document).

## Dev Notes

### fsync-before-send ordering (NFR-1)

Every mutation records its intent through `intentlog::IntentLog::append`, which
performs `fflush` + `platform::durable_sync` **before it returns** (Story 1.5).
Only after that append succeeds does the dispatcher call `pre_send_barrier()` and
then `broker_.*`. The order is therefore literally:

    append (fsync) → pre_send_barrier() → broker send → append_result

The fsync-before-send test installs a barrier that, at the precise instant
between durability and the send, (1) asserts the broker has **not** been touched
(`request_count() == 0`, `book().empty()`), and (2) opens the on-disk log via a
fresh read-only `std::ifstream` and finds the minted `client_ref` and the
`place_order` op name already present. This proves durability strictly precedes
the send.

### UNKNOWN-on-timeout policy (FR-10, NFR-3)

`is_reconcile_first(error)` is the single decision point: an error is "dangerous,
reconcile-don't-retry" iff `action == ReconcileFirst` **or** `category ∈ {Timeout,
Network, Unknown}`. Such an outcome on a mutation:

- sets the order/local-order state to `OrderState::Unknown`,
- persists it (so a crash/replay enumerates it; the reconcile loop in Epic 3 and
  the precedence rules in Story 1.10 resolve it against broker truth),
- appends a `Result(unknown)` intent record,
- and returns **without retrying** — `place` returns the Unknown `Order`;
  `modify/cancel/square_off` surface the typed `Error` after marking the local
  order Unknown.

This is exactly the `FakeBroker::ack_lost_but_placed` shape: the caller sees a
failure, but the order **is** at the broker. A blind retry there is the canonical
duplicate-creating bug; the dispatcher has **no** retry path, so it cannot occur.
A clean verdict (`DoNotRetry` / a non-reconcile category such as RateLimited
where nothing reached the broker) is treated as a clean rejection — the place
order is `Rejected`, not Unknown.

### Single-path enforcement (the chokepoint invariant)

The `Dispatcher` is constructed once at composition time with the **only**
`ports::BrokerPort&` handed to the runtime; it is a private member. No other
runtime type is given the broker reference, so structurally there is no way to
mutate a broker except through `place/modify/cancel/square_off`, each of which
runs the full record→fsync→send→record sequence. This is the C++ analogue of the
architecture's import-linter rule ("no `store`/`ledger` write outside the main
loop; the single broker-mutation path is `dispatch()`"). The behavioral proof is
`FakeBroker::request_count()`: it is exactly 1 after a successful place, exactly 1
after an ack-lost place (no retry), and unchanged on a duplicate submit (zero
sends).

### Success-path FSM usage

A fresh order is constructed directly in `Sent` (the dispatcher owns the creation
of a new order; the FSM applies broker *views* to an existing order). The
broker's ack is then fed through `LifecycleEngine::apply` as a `BrokerView`
(`Sent → Acknowledged`, ordering_key 1), which records the broker_order_id and
the apply-ordering high-water mark exactly as a real broker view would — so the
order returned is `Acknowledged` and consistent with later reconcile views.

### Canonical payload (restart-dedup composition)

The `PlaceOrder` payload is `idempotency::intent_payload_json(intent_with_ref)`
(the reserved `client_ref` is stamped onto the intent copy first). This is the
canonical payload `IdempotencyIndex::rebuild_from_log` keys on, so a restart that
replays the log re-derives the same signature → same `client_ref` → a repeated
signal still dedups. The result records are small, hand-built, JSON-escaped
objects (no nlohmann dependency in this module).

### Constraints honored

Cross-platform C++20 stdlib only (`<functional>`, `<string>`, `<string_view>`,
`<optional>`); no OS APIs, no `#ifdef`, no floating point. Synchronous and
single-threaded between fsync and send (NFR-2). `#pragma once`, 2-space / 100-col
clang-format. CMake target as specified (PUBLIC `ports` + `domain`; PRIVATE
`intentlog`, `store`, `idempotency`, `lifecycle`, `errors`, `warnings`,
`sanitizers`); test target also links `fake_broker`, `clock`, and Catch2.

## Completion Record

- Files created:
  - `include/broker_exec/runtime/dispatcher.hpp`
  - `src/runtime/dispatcher.cpp`
  - `src/runtime/dispatcher_test.cpp`
  - `src/runtime/CMakeLists.txt`
- Orchestrator integration: add `add_subdirectory(src/runtime)` to the top-level
  `CMakeLists.txt` (in the Wave-2+ block; the module CMake is self-contained).
- Tests: place happy / fsync-before-send / idempotency-zero-send /
  no-blind-retry-UNKNOWN / clean-rejection / cancel happy + UNKNOWN /
  square-off happy + UNKNOWN.
- Not done here (by scope): the full reconcile loop (Epic 3) and the
  match-key precedence (Story 1.10) consume the persisted UNKNOWN + the
  `Result` records this story emits; the SIGKILL durability harness (Story 1.12)
  drives `set_pre_send_barrier` to kill between fsync and send.
