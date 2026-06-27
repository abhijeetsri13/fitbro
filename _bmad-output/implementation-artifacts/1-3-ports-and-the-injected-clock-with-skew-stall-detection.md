# Story 1.3: Ports and the injected Clock with skew/stall detection

Status: review
Epic: 1 — Order-Safety Substrate

## Story

As a platform maintainer,
I want the core to depend only on abstract ports including an injected Clock,
So that time-dependent behavior is deterministic and adapters are swappable. (FR-23)

## Acceptance Criteria

1. **Given** the ports library, **when** the core needs time, broker access,
   storage, alerts, secrets, or reference data, **then** it calls a
   `ClockPort`/`BrokerPort`/`StorePort`/`AlertSink`/`SecretProvider`/`RefDataPort`
   abstraction, never a concrete library.
2. **And** a system `Clock` impl and a test `Clock` impl exist; direct
   `*_clock::now()` outside the system impl is lint-banned (documented).
3. **And** an induced clock skew or main-loop stall beyond threshold is detected
   and surfaced (drives exit-only later).

## Tasks

- [x] `include/broker_exec/ports/ports_common.hpp` — `Ok = std::monostate`
      void-substitute + `ports::ok()` helper (the `expected<void>` workaround).
- [x] `include/broker_exec/ports/clock_port.hpp` — `ClockPort` with
      `now_steady()` (monotonic) and `now_wall()` (wall).
- [x] `include/broker_exec/ports/broker_port.hpp` — `BrokerPort` +
      `BrokerAck` + `FundsSnapshot`; place/modify/cancel/square_off and
      fetch_orders/trades/positions/funds, all returning `Result<T>`.
- [x] `include/broker_exec/ports/store_port.hpp` — `StorePort`: append /
      replay_all (callback) / find_by_client_ref. Abstract only (impl 1.5/1.6).
- [x] `include/broker_exec/ports/alert_sink.hpp` — `AlertSink` + `AlertLevel`;
      `send(level, message)` and `send_test_alert()`.
- [x] `include/broker_exec/ports/secret_provider.hpp` — `SecretProvider`:
      `get(key)` -> `Result<std::string>`.
- [x] `include/broker_exec/ports/refdata_port.hpp` — `RefDataPort`:
      `resolve(symbol)` -> `Result<Instrument>` + `as_of()` staleness accessor.
- [x] `include/broker_exec/ports/ports.hpp` — umbrella include.
- [x] `include/broker_exec/clock/system_clock.hpp` + `src/clock/system_clock.cpp`
      — `SystemClock : ports::ClockPort`. The ONLY place `steady_clock::now()` /
      `system_clock::now()` are called (documented + lint-banned elsewhere).
- [x] `include/broker_exec/clock/test_clock.hpp` — `TestClock : ports::ClockPort`,
      deterministic (`advance`, `advance_both`, `set_wall`, `set_steady`).
- [x] `include/broker_exec/clock/skew_stall_detector.hpp` +
      `src/clock/skew_stall_detector.cpp` — `SkewStallDetector` +
      `ClockStatus { Healthy, Stalled, Skewed }` + `reason()`.
- [x] `src/clock/clock_test.cpp` — Catch2 tests driving the detector via
      `TestClock`: Stalled past threshold, Skewed past threshold, Healthy within
      thresholds, stall>skew precedence, latch-until-reset (no real sleeps).
- [x] `src/ports/CMakeLists.txt` — keep `broker_exec_ports` INTERFACE; link
      `broker_exec_domain` AND `broker_exec_errors`; add `broker_exec_ports_tests`
      (mock implements every port) under `if(BROKER_EXEC_BUILD_TESTS)`.
- [x] `src/clock/CMakeLists.txt` — `broker_exec_clock` STATIC + alias
      `broker_exec::clock`, PUBLIC `broker_exec_ports`, PRIVATE warnings/sanitizers,
      `broker_exec_clock_tests` + `add_test`. Mirrors `src/platform`.

## Dev Notes

### The Ok / void decision (READ — affects every void-returning port)

`broker_exec::expected<T, E>` (`include/broker_exec/expected.hpp`) is the
project's minimal C++20 vocabulary type and does **NOT** specialize for `void`:
it stores a `T` in a union and `value()` returns `T&`, so `expected<void, E>`
will not compile. The instructions explicitly forbid editing `expected.hpp` /
`result.hpp`.

**Decision:** ports that logically return "success or Error" with no payload use
`Result<Ok>` where **`using Ok = std::monostate;`** (defined in
`ports/ports_common.hpp`). `std::monostate` is a regular, trivially
constructible "no value". A `ports::ok()` helper returns `Result<Ok>{Ok{}}` so
impls write `return ports::ok();`. This affects `BrokerPort::cancel`,
`BrokerPort::square_off`, `StorePort::append`, `StorePort::replay_all`,
`AlertSink::send`, `AlertSink::send_test_alert`. (If the project later moves to
C++23 / `std::expected`, these can switch to `expected<void, Error>` mechanically.)

### Architecture boundary

The core depends only on these abstractions; no concrete broker SDK / DB / OS API
appears in `ports`. `ports` links `domain` + `errors` only (never `adapters`),
preserved by `enforce_no_dependency(broker_exec_ports FORBIDS broker_exec_adapters)`
in the top-level CMake. `broker_exec_ports` stays an INTERFACE target; it now
links `broker_exec_errors` PUBLICly because port headers include
`broker_exec/result.hpp` (= `expected<T, errors::Error>`).

### The injected clock (FR-23) and the single now() site

All time flows through `ports::ClockPort`. `SystemClock` (in `src/clock/`) is the
**only** translation unit that calls `std::chrono::steady_clock::now()` /
`std::chrono::system_clock::now()`; this is documented at the top of both
`system_clock.hpp` and `system_clock.cpp` and is lint-banned everywhere else
(conventions.md). Two distinct clocks, never interchanged: `now_steady()`
(monotonic — timeouts/durations/loop-tick gaps) and `now_wall()` (system — audit
timestamps; may jump). `TestClock` controls both independently with zero real
time read, so tests are reproducible and sleep-free.

### Skew / stall detection (drives exit-only later)

`SkewStallDetector` is fed periodic samples (from a `ClockPort&` or explicit time
points) and surfaces `ClockStatus`:

- **Stalled** — gap between consecutive `now_steady()` samples exceeds
  `stall_threshold` (the main loop didn't tick).
- **Skewed** — `|wall_elapsed - steady_elapsed|` between samples exceeds
  `skew_threshold` (wall clock jumped vs monotonic — NTP step, manual set, VM pause).

Design choices: the **first** sample only establishes a baseline (returns
Healthy). **Stall takes precedence over skew** in a single interval (a non-ticking
loop is the more fundamental fault). Status **latches** to the worst observation
until `reset()` so a transient fault is not silently cleared by a later healthy
sample. `reason()` is a redaction-safe, log-friendly explanation. This status is
the input that later degrades the runtime to exit-only.

### Cross-platform / warnings

C++20 stdlib only: `<chrono>`, `<memory>`-free (none needed), `<string>`,
`<string_view>`, `<vector>`, `<variant>`, `<functional>`, `<optional>`. No OS
APIs, no `#ifdef`. `#pragma once`; 2-space / 100-col clang-format. Written for
MSVC `/W4 /permissive- /WX` and gcc/clang strict
(`-Wall -Wextra -Wpedantic -Werror -Wshadow`): duration math is normalized to
`std::chrono::nanoseconds` with explicit `duration_cast` (the two clocks have
different native periods), sign handled by an explicit `abs_ns`, every enum
switch is total with a trailing fallback `return` (no C4715), `[[nodiscard]]`
accessors.

### Scope boundary

Owned paths only: `include/broker_exec/ports/*.hpp`, `src/ports/CMakeLists.txt`
(+ `ports_test.cpp`), `include/broker_exec/clock/*.hpp`, `src/clock/*`. Did NOT
edit top-level `CMakeLists.txt`, `tests/`, `domain`, `errors`, `platform`,
`expected.hpp`, or `result.hpp`. Removed the now-obsolete
`include/broker_exec/ports/.keep` placeholder. Did not run cmake/conan/build
(orchestrator owns the central build).

## Completion Record

Files created:

- `include/broker_exec/ports/ports_common.hpp` — `Ok` + `ok()`.
- `include/broker_exec/ports/clock_port.hpp` — `ClockPort`.
- `include/broker_exec/ports/broker_port.hpp` — `BrokerPort`, `BrokerAck`, `FundsSnapshot`.
- `include/broker_exec/ports/store_port.hpp` — `StorePort`.
- `include/broker_exec/ports/alert_sink.hpp` — `AlertSink`, `AlertLevel`.
- `include/broker_exec/ports/secret_provider.hpp` — `SecretProvider`.
- `include/broker_exec/ports/refdata_port.hpp` — `RefDataPort`.
- `include/broker_exec/ports/ports.hpp` — umbrella include.
- `include/broker_exec/clock/system_clock.hpp` + `src/clock/system_clock.cpp` — `SystemClock`.
- `include/broker_exec/clock/test_clock.hpp` — `TestClock`.
- `include/broker_exec/clock/skew_stall_detector.hpp` + `src/clock/skew_stall_detector.cpp`
  — `SkewStallDetector`, `ClockStatus`, `to_string(ClockStatus)`.
- `src/clock/clock_test.cpp` — clock + detector Catch2 tests.
- `src/clock/CMakeLists.txt` — `broker_exec_clock` STATIC + tests.
- `_bmad-output/implementation-artifacts/1-3-ports-and-the-injected-clock-with-skew-stall-detection.md` — this file.

Files modified:

- `src/ports/CMakeLists.txt` — added `broker_exec_errors` to the INTERFACE link;
  added `broker_exec_ports_tests` (mock implements every port).

Files removed:

- `include/broker_exec/ports/.keep` — obsolete placeholder (dir now has headers).

Integration note for the orchestrator: add `add_subdirectory(src/clock)` to the
top-level `CMakeLists.txt` integration point (it links `broker_exec_ports`, which
must already be added). No `enforce_no_dependency` change is required.

Verification: not built locally by design (orchestrator owns the central build).
Code mirrors the `src/platform` module pattern and targets the `/W4 /WX` +
gcc/clang-strict bar; tests are deterministic (no real sleeps).
