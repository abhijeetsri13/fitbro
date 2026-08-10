# Story 2.9: Freeze-quantity slicer

Status: ready-for-dev

## Story

As a strategy author,
I want over-freeze orders sliced into child orders automatically,
so that large orders execute safely without duplicates. (FR-6)

## Acceptance Criteria

1. **Given** an order above the instrument's freeze limit **When** it is dispatched in slice-mode (default)
   **Then** it fans out to children (qty ≤ freeze, ≥ lot, lot-aligned remainder) each with deterministic
   `<parent>#<k>` client-ref.
2. **And** re-slicing on replay is bit-identical and `UNIQUE(client_ref)` dedupes a placed child.
3. **And** any child UNKNOWN engages the parent UNKNOWN-pause.

## Tasks / Subtasks

- [ ] Task 1: `slicing` module (AC: all)
  - [ ] `include/broker_exec/slicing/` + `src/slicing/`; target `broker_exec_slicing` (+ alias). Depends inward on
        `domain`, `errors` ONLY — a PURE deterministic function. It forms the child client-ref inline as `<parent>#<k>`
        (the binding format documented by `idempotency::child_ref`, k starts at 1) so the production target does NOT pull in
        idempotency's store/SQLite deps; a test asserts parity with `idempotency::child_ref`.
- [ ] Task 2: `FreezeSlicer::slice` (AC: 1, 2)
  - [ ] `Result<std::vector<domain::OrderIntent>> slice(const domain::OrderIntent& parent, const domain::Instrument& inst) const`.
  - [ ] Inputs: `qty = parent.quantity`, `freeze = inst.freeze_qty`, `lot = inst.lot_size` (integer Quantity; NO float).
  - [ ] Validate (fail-closed Validation Error, no partial output): lot > 0; qty > 0; qty is lot-aligned (qty % lot == 0) and
        qty >= lot; freeze > 0 and freeze >= lot (a freeze ceiling below one lot cannot be sliced). Refuse to slice a parent
        whose client_ref already contains '#' (a child cannot be re-sliced) -> Validation Error.
  - [ ] `chunk = (freeze / lot) * lot` (the largest lot-aligned quantity that fits under the freeze ceiling).
  - [ ] If `qty <= freeze` (not over-freeze): return a single-element vector `{parent}` UNCHANGED (original client_ref, no `#`).
  - [ ] Over-freeze: `full = qty / chunk`, `rem = qty % chunk` (rem is a non-negative multiple of lot, and rem>0 => rem>=lot).
        Emit children k = 1..full each with quantity = chunk, then (if rem > 0) one final child with quantity = rem. Each child
        = a copy of `parent` with `quantity` replaced and `client_ref = "<parent.client_ref>#<k>"` (k contiguous from 1).
  - [ ] INVARIANT (assert in tests): the child quantities SUM EXACTLY to qty; every child qty in [lot, freeze], lot-aligned;
        child client-refs are unique and contiguous `#1..#N`.
- [ ] Task 3: Determinism + replay (AC: 2)
  - [ ] `slice()` is a pure function of (parent, instrument): calling it twice yields bit-identical vectors (same order,
        same quantities, same client-refs). This is what makes replay re-slicing reproducible and `UNIQUE(client_ref)`
        dedupe a child that was already placed before a crash.
- [ ] Task 4: Parent/child UNKNOWN note (AC: 3)
  - [ ] AC-3 (any child UNKNOWN -> parent UNKNOWN-pause) is realized by the EXISTING lifecycle parent/child fold (Story 1.8)
        + the gate's UNKNOWN-pause check (Story 2.8); the slicer's contribution is the deterministic `<parent>#<k>` refs that
        let the FSM recover the parent (`idempotency::parent_of`). Document this; no FSM change here. (A test may assert the
        child refs are recognizable as children of the parent via the documented format.)
- [ ] Task 5: CMake (orchestrator pre-wires root add_subdirectory(src/slicing); NO new Conan dep)
  - [ ] `src/slicing/CMakeLists.txt`: target links PUBLIC `broker_exec::domain` `broker_exec::errors`; PRIVATE warnings+
        sanitizers. Test exe `broker_exec_slicing_tests` ALSO links `broker_exec::idempotency` (parity assertion only).
- [ ] Task 6: Tests (AC: 1, 2, 3) — `src/slicing/freeze_slicer_test.cpp`
  - [ ] over-freeze: lot 50, freeze 1800 (chunk = 1800), qty 4000 -> children [1800,1800,400], refs parent#1/#2/#3; sum==4000;
        every child qty in [50,1800] and lot-aligned.
  - [ ] exact multiple: qty 3600 -> [1800,1800], no remainder child; refs #1/#2.
  - [ ] not over-freeze: qty 1000 (<=1800) -> single {parent}, original client_ref unchanged (no '#').
  - [ ] freeze not a lot multiple: lot 50, freeze 1825 -> chunk 1800; qty 4000 -> same [1800,1800,400] (chunk floors to lot).
  - [ ] determinism: slice() twice -> identical vectors (deep-equal incl. client_refs).
  - [ ] parity: each child client_ref == `idempotency::child_ref(parent.client_ref, k)` for k=1..N (binding-format consistency).
  - [ ] validation: qty not lot-aligned -> Error; freeze < lot -> Error; parent client_ref containing '#' -> Error; qty 0 -> Error.
  - [ ] (AC-3 doc) a child ref is recognized as a child of the parent via `idempotency::is_child_ref` / `parent_of`.

## Dev Notes

- **Deterministic child ref `<parent>#<k>`** (k from 1) — the binding format owned by `idempotency::child_ref`; the slicer
  reproduces it inline to stay dependency-light, with a test enforcing parity. [architecture.md#IBR-2 freeze slicing, idempotency.hpp]
- **Composes with the gate** (Story 2.8 AllowWithSlicing) and the parent/child FSM (Story 1.8). [architecture.md#B freeze slicing]
- **No float**: all quantities integer `domain::Quantity`. [docs/conventions.md#Money]
- **Fail-closed**: any invalid input -> a single Validation Error, never a partial/odd slice. [docs/conventions.md#Errors]
- **Reuse:** `domain::OrderIntent`/`Instrument`/`Quantity`, `errors`, (test) `idempotency::child_ref`/`is_child_ref`/`parent_of`.

### References
- [Source: epics.md#Story 2.9] [architecture.md#IBR-2, #B] [Source: include/broker_exec/idempotency/idempotency.hpp]
- [Source: docs/conventions.md] [Source: domain/types.hpp]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
