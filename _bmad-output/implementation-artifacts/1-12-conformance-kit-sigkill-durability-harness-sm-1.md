# Story 1.12 — Conformance kit + SIGKILL durability harness (SM-1)

Status: review

## Story

As a platform maintainer,
I want a reusable conformance kit and a SIGKILL durability test,
So that zero-duplicate-orders is a proven, regression-guarded invariant. (FR-37, NFR-3)

This is the capstone of Epic 1. It composes the already-green safety modules
(Dispatcher, IntentLog, Store, IdempotencyIndex, LifecycleEngine, UnknownResolver,
UnknownPause, FakeBroker) into two cross-module integration suites that PROVE the
headline invariant — zero duplicate orders across the full fault matrix, including
a SIGKILL between fsync and send.

## Acceptance Criteria

1. **Given** the conformance kit run against the fake broker
   **When** the full fault matrix executes
   **Then** UNKNOWN-handling, no-blind-retry, and reconciliation pass, with
   duplicate-order count = 0.
2. **And** a separate harness SIGKILLs the process between fsync and send (via an
   injectable pre-send barrier) and asserts zero duplicates on replay + reconcile.
3. **And** the kit runs in CI and gates every adapter (it is reusable + invoked
   from ctest).

## Tasks

- [x] Build the reusable, broker-agnostic conformance kit
  (`tests/conformance/conformance_kit.hpp`, `namespace broker_exec::conformance`):
  a `BrokerFactory` (clock + FaultConfig → `unique_ptr<ports::BrokerPort>`) driven
  through the full fault matrix, returning a structured `ConformanceReport`.
- [x] Per scenario, compose a fresh temp data dir + IntentLog + Store + Dispatcher
  + IdempotencyIndex + LifecycleEngine + UnknownResolver + UnknownPause, place an
  order, re-submit the same signal on ambiguity, resolve any UNKNOWN, reconcile,
  and assert the three properties (no-blind-retry, UNKNOWN handling, ≤ 1 broker
  order per signal).
- [x] Catch2 driver (`tests/conformance/conformance_test.cpp`) running the kit
  against the adversarial `FakeBroker` and asserting `report.ok()` +
  `duplicate_orders == 0` + every scenario passed.
- [x] Plain `int main` SIGKILL worker (`tests/sigkill/sigkill_worker.cpp`, NO
  Catch2) with `worker <datadir> kill` (installs a `std::_Exit(42)` pre-send
  barrier, then `place()`) and `recover <datadir>` (replay + rebuild index +
  reconcile against a fresh broker; prints `DUPLICATES=<n>` / `ORDERS=<n>`; exit 0
  iff zero duplicates).
- [x] Catch2 SIGKILL test (`tests/sigkill/sigkill_test.cpp`) that spawns the
  worker, asserts it died non-zero in the barrier AND the intent log already holds
  the PlaceOrder record (fsync-before-death), then runs `recover` and asserts exit
  0 (zero duplicates). Cleans up the temp dir.
- [x] Wire `tests/CMakeLists.txt`: `broker_exec_conformance_tests` (Catch2),
  `broker_exec_sigkill_worker` (plain exe), `broker_exec_sigkill_tests` (Catch2,
  with `SIGKILL_WORKER_EXE="$<TARGET_FILE:broker_exec_sigkill_worker>"`), each
  `add_test`; keep the existing `broker_exec_tests` placeholder working.

## Dev Notes

### The conformance kit (reusable across adapters)

The kit is a header (`conformance_kit.hpp`) so any adapter epic can include and run
it with no shared compiled object. Its single public entry point:

```cpp
using BrokerFactory =
    std::function<std::unique_ptr<ports::BrokerPort>(ports::ClockPort&,
                                                     adapters::fake::FaultConfig)>;

[[nodiscard]] ConformanceReport run_conformance(const BrokerFactory& make_broker);
```

`ConformanceReport { scenarios_run; scenarios_passed; duplicate_orders;
vector<string> failures; bool ok(); }`. CI keys on `ok()`.

The fault matrix mirrors every `FaultConfig` knob from Story 1.11: `clean`,
`drop_ack`, `ack_lost_but_placed`, `rate_limit`, `duplicate_fill`, `out_of_order`,
`delay_ack` (7 scenarios). Per scenario the kit asserts three properties:
1. **No blind retry** — the dangerous op is attempted at most once. Probed via the
   broker's accepted-request count; that probe is behind a pointer `dynamic_cast`
   to `FakeBroker` returning `std::optional`, so a real adapter (Kite/Kotak) that
   exposes no counter simply skips this probe (it verifies no-blind-retry from its
   recorded-fixture request log) — the kit stays genuinely broker-agnostic.
2. **UNKNOWN handling** — an ambiguous outcome is recorded `OrderState::Unknown`,
   a re-submit of the same signal fires ZERO additional sends (idempotency guard),
   and the UNKNOWN is either resolved against broker truth by the precedence ladder
   or stays UNKNOWN-with-an-alert (fail-closed). A silent drop fails the scenario.
3. **Zero duplicates** — counted from broker truth: ≤ 1 broker order per signal,
   and ≤ 1 local projection row per signal.

**Adapter reuse (AC-3):** Epic 2 (Story 2.14, Kite) and Epic 6 (Story 6.2, Kotak)
pass a different `BrokerFactory` (a recorded-fixture transport) to the SAME
`run_conformance` and get the SAME gate. This is why the conformance target's link
set is the template every adapter conformance target reuses.

### The SIGKILL kill-point — why `std::_Exit`, not a signal

The Dispatcher exposes `set_pre_send_barrier(std::function<void()>)`, a hook that
runs at exactly `record-intent → fsync → [BARRIER] → send`. The worker installs a
barrier that calls `std::_Exit(42)`.

`std::_Exit` (from `<cstdlib>`) terminates the process IMMEDIATELY: it runs **no
destructors, no `atexit` handlers, no stream flushing**. That is the closest
PORTABLE equivalent to an uncatchable OS kill (SIGKILL / `TerminateProcess`). We
deliberately do NOT `raise(SIGKILL)`: signal availability/numbering differs across
Windows and POSIX and would force a `#ifdef`, which the cross-platform convention
forbids. `std::_Exit` gives the identical observable property on every OS — the
process dies with NOTHING flushed after the fsync — with zero platform branching.

Because `IntentLog::append()` flushes + `durable_sync`s BEFORE returning, the
PlaceOrder record is guaranteed on stable storage when the barrier fires; the
broker send, the Result record, and the Store upsert that would follow the barrier
never happen. That is exactly the SIGKILL-between-fsync-and-send fault SM-1 must
survive. On `recover`, a FRESH FakeBroker holds NO order (the send never happened),
the read-only `UnknownResolver` cannot create a duplicate, and the killed order is
still ENUMERABLE from the durable intent record (`ORDERS=1`) — so we know about it
without ever re-firing it (`DUPLICATES=0`).

### Cross-platform `std::system` handling

The harness spawns the worker with `std::system` (works on Windows/Linux/macOS, no
`#ifdef`). The one platform difference is decoding its return value: on Windows it
is the child's exit code directly; on POSIX it is a wait-status whose exit code is
in the high byte. We AVOID platform macros (`WEXITSTATUS`, …) and assert only the
platform-invariant properties:
- kill run → return value `!= 0` (the child died in the `std::_Exit(42)` barrier;
  POSIX yields `42 << 8`, Windows yields `42`, both non-zero), and
- recover run → return value `== 0` (`DUPLICATES=0`).

Both hold identically on every OS. Exe + datadir paths are quoted to tolerate
spaces.

**Integrator caveat (MSVC central build):** `std::system` on Windows shells through
`cmd.exe`, which has a long-standing quirk — when a command string both *begins*
with a quote and contains *multiple* quoted tokens, `cmd /c` may strip the outer
quotes. In this harness the build-tree exe path (`$<TARGET_FILE:…>`) and the
`temp_directory_path()` datadir are normally space-free on CI, so the quirk does
not bite; the integrator should confirm the two suites pass on the central MSVC
runner, and if a spaced path ever appears, wrap the whole command in an extra outer
quote pair on Windows only (kept out of this story to avoid a `#ifdef`).

## Completion Record

- Files created:
  - `tests/conformance/conformance_kit.hpp` — reusable kit (`run_conformance`).
  - `tests/conformance/conformance_test.cpp` — Catch2 driver vs `FakeBroker`.
  - `tests/sigkill/sigkill_worker.cpp` — plain-`main` kill/recover worker.
  - `tests/sigkill/sigkill_test.cpp` — Catch2 SIGKILL harness.
  - `_bmad-output/implementation-artifacts/1-12-conformance-kit-sigkill-durability-harness-sm-1.md`
    (this file).
- Files edited:
  - `tests/CMakeLists.txt` — added `broker_exec_conformance_tests`,
    `broker_exec_sigkill_worker`, `broker_exec_sigkill_tests` (+ `add_test` for the
    two Catch2 suites + the `SIGKILL_WORKER_EXE` compile definition + the
    worker→test build dependency). Placeholder `broker_exec_tests` left intact.
- Constraints honored: cross-platform (`std::filesystem`, `std::system`,
  `std::_Exit`, no `#ifdef`, no OS API, no floating point); `#pragma once`;
  2-space / 100-col; targets link only what they use + `broker_exec_warnings` /
  `broker_exec_sanitizers` PRIVATE. No `src/` or top-level `CMakeLists.txt` edits;
  no cmake/conan/build run (orchestrator builds centrally).
