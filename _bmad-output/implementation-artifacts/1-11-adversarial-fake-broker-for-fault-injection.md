# Story 1.11: Adversarial fake broker for fault injection

Status: review
Epic: 1 — Order-Safety Substrate

## Story

As a test engineer,
I want an in-test fake broker that injects the dangerous faults,
So that the safety invariants can be exercised deterministically. (FR-37)

## Acceptance Criteria

1. **Given** the fake broker fixture, **when** a test configures a fault, **then**
   it can inject delayed acks, dropped acks, duplicate fills, out-of-order events,
   429s (rate-limit), and ack-lost-but-placed.
2. **And** faults are deterministic under the injected Clock (no real time, no
   randomness).
3. **And** it implements the same `ports::BrokerPort` as real adapters.

## Tasks

- [x] `include/broker_exec/adapters/fake/fake_broker.hpp` — `FaultConfig`,
      `BookEntry`, and `FakeBroker final : public ports::BrokerPort`.
- [x] `FaultConfig` — explicit, independently-toggleable faults: `drop_ack`,
      `delay_ack_ticks`, `ack_lost_but_placed`, `duplicate_fill`,
      `out_of_order_events`, `rate_limit_after`, `tick_duration`.
- [x] `FakeBroker` implements every `BrokerPort` method (`place`, `modify`,
      `cancel`, `square_off`, `fetch_orders/trades/positions/funds`) honoring the
      configured faults.
- [x] Internal broker-truth order book + trade list; `fetch_*` reflect broker
      truth INCLUDING ack-lost-but-placed orders (so reconciliation finds them).
- [x] Test/conformance hooks: `set_fault()`, `fault()`, `set_funds()`, raw
      `book()` and `request_count()` inspection.
- [x] Tick-based faults keyed off the injected `ports::ClockPort` (elapsed steady
      ticks since construction) — no real time, no randomness, no `#ifdef`.
- [x] `src/adapters/fake/fake_broker.cpp` — implementation.
- [x] `src/adapters/fake/fake_broker_test.cpp` — co-located Catch2 tests, one per
      fault, all driven by `clock::TestClock` and asserted reproducible.
- [x] `src/adapters/fake/CMakeLists.txt` — `broker_exec_fake_broker` STATIC +
      alias `broker_exec::fake_broker` + `broker_exec_fake_broker_tests` under
      `if(BROKER_EXEC_BUILD_TESTS)` with `add_test`. Mirrors `src/platform`.

## Dev Notes

### Determinism model (AC-2)

No real time, no `<random>`, no OS APIs, no `#ifdef`. Every fault is keyed off
(a) the explicit `FaultConfig` and (b) the injected `ports::ClockPort` plus
internal monotonic counters (`next_broker_seq_`, `next_trade_seq_`,
`request_count_`). Tick-based faults measure `floor(elapsed_steady /
tick_duration)` since construction, read from the clock — a test advances
`clock::TestClock` to cross a threshold. Two brokers built with the same config,
driven over the same clock timeline with the same call sequence, produce
byte-identical results (broker ids `FAKE-ORD-<n>`, trade ids `FAKE-TRD-<n>`).

### "Broker truth" vs caller-observed (the safety story)

The fake keeps an internal order book that represents what the BROKER believes,
independent of what the caller observed. On a placed order the entry always
enters the book; only whether (and when) the caller sees the ack varies:

- `ack_lost_but_placed` — `place()`/`modify()`/`cancel()`/`square_off()` return
  `Timeout` (action `ReconcileFirst`) but the mutation IS applied to broker
  truth. A blind retry would duplicate; `fetch_orders()` reveals the order so the
  safety core reconciles instead. This is the headline danger (NFR-3).
- `drop_ack` — same shape as ack-lost (order placed, caller times out); kept as a
  separate readable toggle.
- `delay_ack_ticks` — order placed immediately, ack withheld until the clock has
  advanced N ticks; before the threshold `place()` returns `Timeout`, after it
  returns the ack normally.
- `rate_limit_after` — after N ACCEPTED requests (mutations and queries alike),
  every subsequent request returns `RateLimited` (HTTP-429 shaped). A throttled
  request consumes no budget and never reaches the book.
- `duplicate_fill` — `fetch_trades()` returns two byte-identical `Trade` rows
  (same `trade_id`/qty/price) for the placed order.
- `out_of_order_events` — `fetch_orders()`/`fetch_trades()` return rows in
  reversed sequence; raw `book()` stays in insertion order. Exercises the
  forward-progressing / ordering-key apply logic (stale view must be dropped).

### Execution model

A placed order is treated as immediately, fully filled at its intent price
(`Filled`, `filled_qty == quantity`, `avg_price == price`) — the fake's simple,
deterministic execution. `fetch_positions()` derives net signed quantity per
symbol from filled book entries. `cancel`/`square_off` set the book entry to
`Cancelled` as broker truth (the caller may still have timed out on the ack).
`fetch_funds()` returns a seeded `FundsSnapshot` (`set_funds`, default zero).

### Hexagonal boundary (adapters are the OUTER layer)

This is an adapter: it links `broker_exec_ports` PUBLIC (it implements
`BrokerPort`) and `broker_exec_domain`, `broker_exec_errors`, `broker_exec_clock`
PRIVATE. It is NOT linked by `domain`/`ports` — the `enforce_no_dependency()`
boundary check forbids that. The fake injects faults using the SAME
`ports::BrokerPort` surface real adapters implement (AC-3), so the Story 1.12
conformance kit drives it exactly as it will drive Kite/Kotak.

### Cross-platform / warnings

C++20 stdlib only (`<cstddef>`, `<cstdint>`, `<string>`, `<vector>`, `<chrono>`,
`<algorithm>`, `<utility>`). No OS APIs, no `#ifdef`. `#pragma once`. 2-space /
100-col clang-format. Written to compile clean under MSVC `/W4 /permissive- /WX`
and gcc/clang strict (`-Wall -Wextra -Wpedantic -Werror -Wshadow`): no narrowing,
signed/size comparisons use `std::size_t`, no shadowed names, every error path
returns a typed `Result`.

### Scope boundary

Owned paths only: `include/broker_exec/adapters/fake/*.hpp`,
`src/adapters/fake/*.cpp` (+ co-located test), `src/adapters/fake/CMakeLists.txt`.
The top-level `CMakeLists.txt`, `tests/`, `src/adapters/CMakeLists.txt`, and other
modules are NOT edited — the orchestrator adds
`add_subdirectory(src/adapters/fake)`. Did not run cmake/conan/build (orchestrator
builds centrally).

## Completion Record

Files created:

- `include/broker_exec/adapters/fake/fake_broker.hpp` — `FaultConfig`,
  `BookEntry`, `FakeBroker : public ports::BrokerPort` + test hooks.
- `src/adapters/fake/fake_broker.cpp` — fault-injecting `BrokerPort`
  implementation over the internal broker-truth order book + trade list.
- `src/adapters/fake/fake_broker_test.cpp` — Catch2 tests, one per fault
  (ack-lost-but-placed, drop_ack, delay_ack_ticks, rate_limit_after,
  duplicate_fill, out_of_order_events) + reconfigure/not-found/funds, all driven
  by `clock::TestClock` and asserted deterministic/reproducible.
- `src/adapters/fake/CMakeLists.txt` — `broker_exec_fake_broker` STATIC + alias
  `broker_exec::fake_broker` + tests target.

Integration note for the orchestrator: add `add_subdirectory(src/adapters/fake)`
to the top-level `CMakeLists.txt` integration point (after
`add_subdirectory(src/adapters)`). No other module is touched.

Verification: not built locally by design (orchestrator owns the central build).
Code mirrors the established `src/platform` module pattern and the `/W4 /WX` +
gcc/clang-strict bar.
