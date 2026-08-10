# Story 5.3: Freeze-slicing at option size

Status: ready-for-dev

## Story

As an option seller,
I want large option orders sliced safely within a basket,
so that big positions execute without duplicates. (FR-6/FR-17 composition)

## Acceptance Criteria

1. **Given** a basket leg above the freeze limit **When** it executes **Then** the leg slices into deterministic-ref
   children (`<parent>#<k>`) that compose with leg dependency and rate-limit/exit-priority.
2. **And** a mid-slice SIGKILL leaves NO orphan/duplicate child on recovery (deterministic refs + idempotent placement: an
   already-placed child is detected and NOT re-sent).
3. **And** any child UNKNOWN engages the parent/basket UNKNOWN-pause (stop placing, no blind retry, alert + reconcile-first).

## Tasks / Subtasks

- [ ] Task 1: Extend the EXISTING `options` module (AC: all) — `include/broker_exec/options/sliced_leg.hpp` /
      `src/options/sliced_leg.cpp`. Add both + `sliced_leg_test.cpp` to `src/options/CMakeLists.txt`.
  - [ ] **REUSE the real `broker_exec::slicing::FreezeSlicer`** for the deterministic slice — do NOT reimplement the
        `<parent>#<k>` ref logic. Link `broker_exec_slicing` (its `slice()` is called in the .cpp -> PRIVATE link). Also
        depends inward on `domain`, `errors`, `ports` (AlertSink). **No new Conan dep.**
- [ ] Task 2: Placement vocabulary (AC: 2, 3) — `sliced_leg.hpp`
  - [ ] `enum class ChildPlacement { Acked, AlreadyPlaced, Unknown };` (AlreadyPlaced = idempotent dedupe on replay).
  - [ ] `enum class SlicedLegOutcome { FilledSliced, UnknownPaused, SliceRejected };` + stable `to_string`:
        - `FilledSliced` — every child Acked or AlreadyPlaced (the success terminal; includes the all-dedup replay case).
        - `UnknownPaused` — a child came back UNKNOWN: placement STOPPED, basket/parent paused for reconcile (AC-3).
        - `SliceRejected` — the slicer returned a Validation Error (bad lot/freeze/qty, or a `#`-child re-slice): nothing
          placed, fail-closed.
  - [ ] `struct ChildResult { std::string client_ref; ChildPlacement placement; std::string broker_order_id; };`
  - [ ] `struct SlicedLegResult { SlicedLegOutcome outcome; std::vector<ChildResult> children; int placed_count;`
        `int deduped_count; std::string paused_at_ref; std::string detail; };`
- [ ] Task 3: Injected seams (AC: 2, 3) — replay-safety + UNKNOWN over seams, no real broker
  - [ ] `struct SlicedLegSeams {`
        `std::function<Result<bool>(const std::string& client_ref)> already_placed;  // idempotency: child already on the broker?`
        `std::function<Result<std::pair<ChildPlacement, std::string>>(const domain::OrderIntent& child)> place_child; };`
        (place_child returns the placement state + broker_order_id; ChildPlacement here is Acked or Unknown.)
  - [ ] Fail-closed: a null `place_child` => `SliceRejected` (never proceed without a placer). A null `already_placed` is
        treated as "not known to be placed" (proceed to place — the store UNIQUE(client_ref) is the real backstop) BUT an
        `already_placed` returning an **Error** is fail-closed as UNKNOWN (can't prove it's safe to (re)send -> pause, AC-3).
- [ ] Task 4: The executor (AC: all) — `sliced_leg.cpp`
  - [ ] `[[nodiscard]] SlicedLegResult execute_sliced_leg(const domain::OrderIntent& parent, const domain::Instrument&,`
        `const SlicedLegSeams&, ports::AlertSink&)` — NO throw.
  - [ ] Step 1: `FreezeSlicer{}.slice(parent, inst)`. On Error -> `SliceRejected`, nothing placed (fail-closed), Error tag in
        detail. (A not-over-freeze parent slices to a single unchanged child — handled identically: one "child".)
  - [ ] Step 2: place the children IN SLICE ORDER (k=1..N — deterministic, composes with rate-limit/exit-priority since the
        caller drives the cadence). For each child:
        1. `already_placed(child.client_ref)`: returns `true` -> `AlreadyPlaced`, ++deduped_count, CONTINUE (no re-send —
           this is the SIGKILL-recovery dedupe, AC-2). Returns an **Error** -> treat as UNKNOWN (Step 4). `false`/null seam ->
           proceed to place.
        2. `place_child(child)`: `Acked` -> ++placed_count, record broker_order_id. `Unknown` -> UNKNOWN-pause (Step 4). A
           place **Error** -> also UNKNOWN-pause (a mutating call that failed ambiguously must NOT be blindly retried — the
           dispatch no-blind-retry rule; reconcile first).
  - [ ] Step 4 — UNKNOWN-pause (AC-3): STOP immediately (do not place any remaining child), set `paused_at_ref` to the child
        ref, outcome `UnknownPaused`, send ONE Critical alert (redaction-safe; "reconcile before resume"), best-effort
        (swallow + try/catch — mirror 5.1/5.2). The basket/parent is now paused; recovery reconciles, then re-runs (the
        already-placed children dedupe).
  - [ ] Step 3 — success: if every child ended Acked/AlreadyPlaced -> `FilledSliced`.
- [ ] Task 5: CMake — add `sliced_leg.cpp` + `sliced_leg_test.cpp` to `src/options/CMakeLists.txt`; link
      `broker_exec_slicing` (PRIVATE) on the library and the test exe. No new dep.
- [ ] Task 6: Tests — `src/options/sliced_leg_test.cpp` (spy AlertSink + lambda seams + a placement log of client_refs)
  - [ ] AC-1 deterministic slice: an over-freeze parent yields children with refs `"<parent>#1".."<parent>#N"` in order
        (assert the actual refs from the real FreezeSlicer); all Acked => `FilledSliced`, placed_count==N, no alert.
  - [ ] AC-2 SIGKILL recovery / no duplicate: `already_placed` returns true for `#1` and `#2` (simulating a crash after two
        children were placed) => on re-run, place_child is called ONLY for `#3..#N` (placement-log assertion — `#1/#2` NEVER
        re-sent), deduped_count==2, outcome `FilledSliced`. Re-running an ALL-already-placed leg => place_child count 0,
        deduped_count==N, `FilledSliced` (idempotent replay is a no-op).
  - [ ] AC-3 child UNKNOWN: place_child returns `Unknown` for `#2` => STOP (place_child NEVER called for `#3..#N`),
        outcome `UnknownPaused`, paused_at_ref=="<parent>#2", Critical alert sent.
  - [ ] AC-3 place Error => same UNKNOWN-pause (ambiguous mutating failure is not blindly retried).
  - [ ] AC-3 already_placed Error => UNKNOWN-pause (can't prove safe to send), place_child NEVER called for that child.
  - [ ] Fail-closed: slicer Validation Error (e.g. freeze_qty < lot_size, or a parent client_ref already containing `#`) =>
        `SliceRejected`, place_child count 0; null place_child seam => `SliceRejected`.

## Dev Notes

- **Without duplicates is the point** (FR-6/FR-17): deterministic `<parent>#<k>` refs (from the real FreezeSlicer) + an
  idempotent `already_placed` check mean a crash-and-replay re-emits the SAME refs and the already-placed children dedupe —
  no orphan, no duplicate. This is the in-memory analog of the store's `UNIQUE(client_ref)` backstop. [architecture.md#FR-6/FR-17]
- **A child UNKNOWN pauses the whole basket** (AC-3) — an ambiguous placement (Unknown OR an Error on a mutating call) is
  NEVER blindly retried; stop, alert, reconcile-first, then resume (dedupe makes resume safe). Mirrors the dispatch
  no-blind-retry rule (Story 1.9) and the reconcile-before-resume recovery (Story 3.4).
- **Reuse, don't reinvent:** call `slicing::FreezeSlicer::slice` for the children; reuse 5.1/5.2 seam + swallow/try-catch
  alert discipline; `ports::BrokerAck` isn't needed (place_child returns its own pair). No throw, no float, redaction-safe.

### References
- [Source: epics.md#Story 5.3, FR-6/FR-17] [architecture.md#FR-6 freeze slice, #FR-17 basket, #1.9 no-blind-retry, #3.4 reconcile-before-resume]
- [Source: include/broker_exec/slicing/freeze_slicer.hpp] [include/broker_exec/domain/types.hpp (OrderIntent/Instrument)]
- [Source: include/broker_exec/options/basket.hpp (5.2 sibling)] [docs/conventions.md]

## Dev Agent Record
### Agent Model Used
claude-opus-4-8[1m] (Claude Opus 4.8, 1M context)

### Completion Notes List
- Implemented `execute_sliced_leg` over injected seams, REUSING the real
  `broker_exec::slicing::FreezeSlicer{}.slice(parent, inst)` for the deterministic
  `<parent>#<k>` children — the ref/chunk logic is NOT reimplemented here.
- Fail-closed: null `place_child` => `SliceRejected` before any slice; a slicer
  Validation Error => `SliceRejected` with the stable error-category tag in `detail`.
- Idempotent dedupe (AC-2): `already_placed`==true marks a child `AlreadyPlaced`,
  bumps `deduped_count`, and CONTINUES without re-sending (the SIGKILL-recovery path).
- UNKNOWN-pause (AC-3): a child `Unknown`, a `place_child` Error, OR an
  `already_placed` Error STOPS placement at that child, sets `paused_at_ref`,
  yields `UnknownPaused`, and fires ONE best-effort Critical alert (swallowed +
  try/catch, mirroring basket.cpp 5.2). No remaining child is sent.
- No throw across the boundary, no float, redaction-safe `detail`/alert (only
  client_refs + error-category tags). slicing linked PRIVATE (called only in .cpp);
  the test exe picks it up transitively via the static `broker_exec_options`.
- Did NOT build (orchestrator builds). Touched only the `options` module + its
  CMakeLists.

### File List
- include/broker_exec/options/sliced_leg.hpp (new)
- src/options/sliced_leg.cpp (new)
- src/options/sliced_leg_test.cpp (new)
- src/options/CMakeLists.txt (edited: +sliced_leg.cpp, +sliced_leg_test.cpp, +broker_exec_slicing PRIVATE)
