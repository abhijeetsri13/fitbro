---
story: "1.8"
title: Order lifecycle state machine with apply-ordering and parent/child model
epic: "Epic 1 — Order-Safety Substrate"
fr: FR-9
status: review
module: lifecycle
target: broker_exec_lifecycle
---

# Story 1.8 — Order lifecycle state machine with apply-ordering and parent/child model

## Status

review

## Story

As an operator,
I want a single state machine owning each order including parent/child slices,
So that concurrent broker views never corrupt order state. (FR-9)

## Acceptance Criteria

1. **Given** the lifecycle FSM applied only by the main loop
   **When** broker views arrive (push or reconciler snapshot)
   **Then** terminal states (FILLED/REJECTED/CANCELLED) are absorbing and a
   stale/older-keyed view is dropped + logged.
2. **And** a parent intent folds over child states; any child UNKNOWN ⇒ parent
   PARTIALLY_PLACED-with-UNKNOWN.
3. **And** transitions are forward-progressing (applied only if the broker
   ordering key ≥ last applied).

## Tasks / Subtasks

- [x] New module `lifecycle` (`include/broker_exec/lifecycle/lifecycle.hpp`,
      `src/lifecycle/lifecycle.cpp`, co-located `lifecycle_test.cpp`,
      `src/lifecycle/CMakeLists.txt`).
- [x] `BrokerView` value type (client_ref, broker_order_id, observed_state,
      filled_qty, avg_price, ordering_key) — the binding input shape Story 1.9
      codes against.
- [x] `ApplyOutcome { Applied, DroppedStale, DroppedTerminal, NoChange }` +
      `to_string` (observability contract, NFR-8).
- [x] `is_terminal` (Filled/Rejected/Cancelled) — total switch, no default. (AC1)
- [x] `is_valid_transition` backed by an explicit `static` transition table
      (`table_allows`) — total switch on `from`, no default, so a new OrderState
      fails to compile under `-Werror=switch`.
- [x] `fold_parent_state` — empty→Created; any-Unknown→PartiallyPlaced;
      any-Rejected→ManualInterventionRequired; all-Filled→Filled;
      all-Cancelled→Cancelled; mixed-terminal→PartiallyFilled;
      otherwise→PartiallyPlaced. (AC2)
- [x] `LifecycleEngine::apply` — terminal-absorbing → forward-progressing →
      legal-transition, in that order; returns ApplyOutcome for logging/metrics.
      (AC1, AC3)
- [x] `LifecycleEngine::apply_child` / `parent_state` — register a child state
      and recompute the parent fold; recovers the parent from a `<parent>#<k>`
      child ref via `idempotency::is_child_ref` / `parent_of`. (AC2)
- [x] `LifecycleEngine::last_key` — per-order high-water mark of the applied key.
- [x] Module-local Catch2 suite registered with `add_test`.

## Dev Notes

### Single writer (NFR-2)

The FSM is applied **only by the main loop**. The decision core is synchronous
and single-threaded; the reconciler (Story 3.1) fetches broker truth off-loop and
enqueues an immutable result, but the **main loop is the sole applier of diffs**.
`LifecycleEngine` is therefore deliberately **not** thread-safe and must never be
touched off the main loop. This is documented at the top of `lifecycle.hpp`.

### Apply order (the three gates, in sequence) — `LifecycleEngine::apply`

1. **Terminal-absorbing (AC1).** If `order.state` is terminal (Filled / Rejected
   / Cancelled) the view is dropped → `DroppedTerminal`, regardless of its
   ordering_key — even a numerically-newer view cannot resurrect a sink.
2. **Forward-progressing (AC3).** If `view.ordering_key <` the last key applied
   for `view.client_ref` → `DroppedStale`. Equal or higher keys are admitted (an
   equal key re-applies idempotently).
3. **Legal transition only.** If `view.observed_state` is not a legal successor
   of the current state → `NoChange` (the illegal jump is refused; the order is
   left untouched, but the key is recorded as seen for consistency).

A legal, current view that changes the state (or refines fill qty / avg price /
broker_order_id within the same state) → `Applied`. A legal, current view that
changes nothing → `NoChange`. The key advances on both `Applied` and `NoChange`,
never on a drop.

### Transition table (binding matrix — `table_allows`)

`is_valid_transition(from, to)` — self-transition (`from == to`) is always legal.
Terminal rows are empty (absorbing). `→ Unknown` is legal from every non-terminal
state (a send/observe result is uncertain — reconcile, never blindly retry, FR-10).

| from \ to | Validated | PendingSend | Sent | Ack | PartFilled | Filled | Rejected | Cancelled | Unknown | Reconciled | PartPlaced | ManualInt |
|---|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| Created | ✓ | | | | | | ✓ | | ✓ | | | |
| Validated | | ✓ | | | | | ✓ | | ✓ | | | |
| PendingSend | | | ✓ | | | | ✓ | | ✓ | | | |
| Sent | | | | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | | | |
| Acknowledged | | | | | ✓ | ✓ | ✓ | ✓ | ✓ | | | |
| PartiallyFilled | | | | | ✓ | ✓ | ✓ | ✓ | ✓ | | | |
| Filled | | | | | | | | | | | | | (terminal) |
| Rejected | | | | | | | | | | | | | (terminal) |
| Cancelled | | | | | | | | | | | | | (terminal) |
| Unknown | | | | ✓ | ✓ | ✓ | ✓ | ✓ | | ✓ | ✓ | ✓ |
| Reconciled | | | | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | | | ✓ |
| PartiallyPlaced | | | | | ✓ | ✓ | | ✓ | ✓ | ✓ | ✓ | ✓ |
| ManualIntervention | | | | | | | | ✓ | | ✓ | | |

Forward-progressing happy path:
`Created → Validated → PendingSend → Sent → Acknowledged → PartiallyFilled → Filled`.

Resilience edges: any non-terminal → `Unknown`; `Unknown` is resolved by the
reconciler to any live/terminal state or escalated to
`ManualInterventionRequired`; `Reconciled` is the confirmed-against-broker marker;
`PartiallyPlaced` is the sliced-parent posture; `ManualInterventionRequired` is a
near-sink (only an operator/reconcile path moves it → `Reconciled` or `Cancelled`,
no automatic square-off).

### Parent-fold rule (`fold_parent_state`)

A sliced **parent has no broker state of its own** — it is a pure fold over its
children's states. Precedence (first match wins), documented in code:

1. **empty children → Created** (no slices registered yet — the parent base).
2. **any child Unknown → PartiallyPlaced** (the "unknown present" posture; the
   parent pauses on the ambiguity before any other conclusion — FR-10). (AC2)
3. **any child Rejected (and no Unknown) → ManualInterventionRequired** (a slice
   was refused; the parent is partial-and-broken and needs a human — no
   automatic square-off).
4. **all children Filled → Filled.**
5. **all children Cancelled → Cancelled.**
6. **all children terminal but mixed (Filled+Cancelled) → PartiallyFilled.**
7. **otherwise (≥1 child still active/in-flight) → PartiallyPlaced** (placement
   across slices still in progress).

`apply_child` recovers the true parent from a `<parent>#<k>` child ref via
`idempotency::is_child_ref` / `idempotency::parent_of` (Story 1.7 model), so a
child cannot be filed under the wrong parent; it falls back to the supplied
parent for a non-slice ref. `parent_state` returns the cached fold or `nullopt`.

### Constraints honored

- Cross-platform, C++20 stdlib only (`<unordered_map>`, `<vector>`, `<optional>`,
  `<cstdint>`, `<string>`, `<string_view>`). No OS APIs, no `#ifdef`, no floating
  point (fills use `domain::Quantity`, prices use `domain::Price` — int64 paise).
- Total `switch`es (no `default`) in `is_terminal`, `table_allows`, `to_string` —
  a new `OrderState` triggers `-Wswitch`/`-Werror`.
- `#pragma once`, 2-space / 100-col `.clang-format`.
- Target `broker_exec_lifecycle` (STATIC) + alias `broker_exec::lifecycle`,
  PUBLIC include `${PROJECT_SOURCE_DIR}/include`, `cxx_std_20`; link PUBLIC
  `broker_exec_domain`, PRIVATE `broker_exec_errors`, `broker_exec_idempotency`,
  `broker_exec_warnings`, `broker_exec_sanitizers`. Tests link
  `Catch2::Catch2WithMain` + `add_test`.

### Integration caveats (for Story 1.9 / 1.10 / 2.9)

- **Story 1.9 (dispatch)** is the sole caller of `apply` on the main loop. It
  owns the `ordering_key` source: push updates carry the broker/exchange update
  sequence; reconciler snapshots must carry a monotonic key from the same space
  (or a dedicated reconcile-key lane) — `apply` only compares keys per
  `client_ref`, so the two streams must share a comparable key per order or the
  reconciler will be dropped as stale. This is a 1.9/3.1 wiring decision.
- `apply` mutates `order` in place and updates `state`, `filled_qty`,
  `avg_price`, and `broker_order_id` (only when the view supplies a non-empty
  id). It does **not** persist; the store write remains in the `dispatch()`
  chokepoint (Story 1.9), the sole writer outside-the-loop ban still holds.
- `LifecycleEngine` holds in-memory per-order key + per-parent child state. On
  restart this is rebuilt by replay/reconcile (Stories 1.5/3.4); the engine
  itself has no durability and assumes a fresh process re-seeds it.
- Parent/child folding is the **model** only (per the epics cross-cut note); the
  freeze-slicer that emits children lands in Story 2.9 and consumes `apply_child`.

## Completion Record

- Files created:
  - `include/broker_exec/lifecycle/lifecycle.hpp`
  - `src/lifecycle/lifecycle.cpp`
  - `src/lifecycle/lifecycle_test.cpp`
  - `src/lifecycle/CMakeLists.txt`
- Orchestrator integration line (top-level `CMakeLists.txt`, not edited here):
  `add_subdirectory(src/lifecycle)`
- Tests (module-local Catch2, `broker_exec_lifecycle_tests`): terminal-absorbing
  (post-Filled view dropped), forward-progressing (lower key dropped, equal→
  NoChange, higher→Applied), illegal transition refused, full Created→Filled
  happy path, `fold_parent_state` all-filled / one-unknown / any-rejected /
  mixed-terminal / empty, `apply_child` fold + parent recovery from slice ref,
  `parent_state` nullopt, `to_string` stability.
- Not built locally (orchestrator builds centrally per the story constraints);
  code reviewed against MSVC `/W4 /permissive- /WX` and gcc/clang
  `-Wall -Wextra -Wpedantic -Werror -Wshadow` (total switches, no float, no
  unused, no shadow).
