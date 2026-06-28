# Story 5.2: Basket / multi-leg execution

Status: ready-for-dev

## Story

As an option seller,
I want multi-leg trades executed as one logical unit,
so that legs don't get orphaned. (FR-17)

## Acceptance Criteria

1. **Given** a multi-leg basket **When** it executes **Then** leg dependency is honored (a dependent leg is NOT sent if any
   prerequisite leg failed/was skipped) and partial execution is detected.
2. **And** a forced one-leg failure exits/cancels the already-executed legs per the configured policy.
3. **And** the basket is tracked as ONE logical trade unless explicitly configured otherwise.

## Tasks / Subtasks

- [ ] Task 1: Extend the EXISTING `options` module (AC: all) — `include/broker_exec/options/basket.hpp` / `src/options/basket.cpp`
  - [ ] Add `basket.cpp` to `broker_exec_options`; `basket_test.cpp` to the test exe. Pure orchestration over injected seams;
        depends inward on `domain`, `errors`, `ports` (AlertSink). **No new Conan dep.** Reuse the 5.1 seam/fail-closed style.
- [ ] Task 2: Basket model (AC: 1, 3) — `basket.hpp`
  - [ ] `struct BasketLeg { std::string leg_id; std::vector<std::string> depends_on; };` (depends_on = prerequisite leg_ids;
        empty = independent/root leg). leg_id non-empty + unique within a basket.
  - [ ] `enum class LegFailurePolicy { UnwindExecuted, LeaveAndAlert };` — UnwindExecuted = cancel/exit every already-executed
        leg on ANY leg failure (atomic-ish basket); LeaveAndAlert = leave executed legs in place but raise an alert.
  - [ ] `struct BasketConfig { LegFailurePolicy on_leg_failure = LegFailurePolicy::UnwindExecuted; bool track_as_single_unit`
        `= true; std::string basket_id; };` (AC-3: single-unit default true; basket_id is the one logical-trade handle).
  - [ ] `enum class LegStatus { Executed, Failed, SkippedUnmetDependency, Unwound };` + `struct LegResult { std::string`
        `leg_id; LegStatus status; std::string order_id; std::string detail; };` + stable `to_string(LegStatus)`.
  - [ ] `enum class BasketOutcome { Complete, PartiallyExecutedUnwound, PartiallyExecutedLeft, Blocked };` (+ to_string):
        - `Complete` — every leg Executed.
        - `PartiallyExecutedUnwound` — ≥1 leg failed/skipped; executed legs were unwound per UnwindExecuted policy.
        - `PartiallyExecutedLeft` — ≥1 leg failed/skipped; executed legs LEFT in place per LeaveAndAlert (alert raised).
        - `Blocked` — invalid basket (cyclic/missing dependency, dup/empty leg_id): NOTHING placed (fail-closed, pre-flight).
  - [ ] `struct BasketResult { BasketOutcome outcome; std::string basket_id; std::vector<LegResult> legs; bool`
        `tracked_as_single_unit; std::string detail; };`
- [ ] Task 3: Injected seams (AC: all)
  - [ ] `struct BasketSeams { std::function<Result<ports::BrokerAck>(const BasketLeg&)> place_leg;`
        `std::function<Result<ports::Ok>(const std::string& leg_order_id)> unwind_leg;  // cancel/exit an executed leg };`
  - [ ] Null `place_leg` => the basket is `Blocked` (fail-closed; never partially place with a missing placer). A failing
        `unwind_leg` (Error/null) during remediation is recorded per-leg (that leg stays Executed-not-Unwound) + escalated in
        `detail` — unwind is best-effort but the FAILURE TO UNWIND must be visible, never silently dropped.
- [ ] Task 4: The executor (AC: all) — `basket.cpp`
  - [ ] `[[nodiscard]] BasketResult execute_basket(const std::vector<BasketLeg>&, const BasketConfig&, const BasketSeams&,`
        `ports::AlertSink&)` — NO throw.
  - [ ] **Pre-flight validation (fail-closed, before ANY placement):** reject empty/duplicate leg_id, a `depends_on` naming
        an unknown leg, and any DEPENDENCY CYCLE (Kahn/topological sort; a cycle or unknown dep => `Blocked`, nothing placed).
  - [ ] **Dependency-honoring execution** in topological order: a leg is placed ONLY if EVERY prerequisite is `Executed`. If
        any prerequisite is `Failed` or `SkippedUnmetDependency`, this leg is `SkippedUnmetDependency` (NOT sent — AC-1) and
        its own dependents transitively skip too. `place_leg` Error => that leg `Failed`.
  - [ ] **Partial detection + policy (AC-2):** after the pass, if any leg is Failed/Skipped:
        - `UnwindExecuted`: call `unwind_leg(order_id)` for each `Executed` leg (newest-first); on success mark it `Unwound`.
          A null/Error unwind leaves that leg `Executed` and appends a Critical detail. Outcome `PartiallyExecutedUnwound`,
          plus a Critical alert. (Best-effort alert Result swallowed + try/catch — mirror 5.1.)
        - `LeaveAndAlert`: leave executed legs; raise a Warning alert; outcome `PartiallyExecutedLeft`.
      If all Executed => `Complete`, no alert.
  - [ ] **AC-3 single-unit:** set `result.basket_id` (from config, or a deterministic value if empty) and
        `tracked_as_single_unit = config.track_as_single_unit`. Document: the id is the one logical-trade handle the caller
        persists/reconciles against; `false` means the caller opted each leg out to independent tracking.
- [ ] Task 5: CMake — add `basket.cpp` + `basket_test.cpp` to the existing `src/options/CMakeLists.txt`. No new dep.
- [ ] Task 6: Tests — `src/options/basket_test.cpp` (spy AlertSink + lambda seams + a call/placement log)
  - [ ] AC-1 dependency honored: B depends_on A; A fails => B is `SkippedUnmetDependency` and place_leg was NEVER called for
        B (placement-log assertion, not just the enum). Transitive: C depends_on B => C also skipped.
  - [ ] AC-1 happy: 3 independent legs all Executed => `Complete`, no alert, no unwind.
  - [ ] AC-1 partial detection: 1 of 3 fails => outcome is a partial (not Complete).
  - [ ] AC-2 unwind: A,B Executed then C fails under `UnwindExecuted` => A,B `unwind_leg`-called (assert order newest-first)
        and marked `Unwound`, outcome `PartiallyExecutedUnwound`, Critical alert sent.
  - [ ] AC-2 unwind failure visible: an executed leg whose `unwind_leg` returns Error stays `Executed` (NOT Unwound) and the
        detail/alert escalates — the failure to unwind is never silently dropped.
  - [ ] AC-2 LeaveAndAlert: a leg fails under `LeaveAndAlert` => executed legs NOT unwound (unwind_leg never called),
        outcome `PartiallyExecutedLeft`, Warning alert.
  - [ ] AC-3: `track_as_single_unit` default true => `tracked_as_single_unit==true` + basket_id set; explicit false =>
        `tracked_as_single_unit==false`.
  - [ ] Fail-closed pre-flight: a dependency CYCLE (A->B->A) => `Blocked`, place_leg NEVER called (count 0); an unknown
        dependency id => `Blocked`; a duplicate leg_id => `Blocked`; a null place_leg seam => `Blocked`.

## Dev Notes

- **No orphaned legs** (FR-17): the basket is one logical unit — a dependent leg never fires on a failed prerequisite, and a
  mid-basket failure unwinds (or explicitly leaves+alerts) the executed legs per policy. Pre-flight topological validation
  means an ill-formed basket places NOTHING (fail-closed), never a half-basket. [architecture.md#FR-17 basket]
- **Unwind is best-effort but LOUD** — a failed unwind must surface (leg stays Executed + Critical detail/alert), never a
  silent drop that hides a live orphan. Mirror 5.1's emergency-before-alert + swallow/try-catch alert discipline.
- **Reuse 5.1 conventions:** injected std::function seams, `ports::BrokerAck`/`AlertSink`, fail-closed on null/Error, no
  throw, no float, redaction-safe detail. Real wiring to `BrokerPort` + reconciliation + per-leg slicing (5.3) is later.

### References
- [Source: epics.md#Story 5.2, FR-17] [architecture.md#FR-17 basket/multi-leg]
- [Source: include/broker_exec/options/hedge_first.hpp (5.1 sibling pattern)] [include/broker_exec/ports/broker_port.hpp]
- [Source: docs/conventions.md]

## Dev Agent Record
### Agent Model Used
claude-opus-4-8[1m] (Claude Code C++20 dev agent)

### Completion Notes List
- Implemented `execute_basket` as pure, no-throw orchestration over injected seams, matching the 5.1
  hedge-first sibling style: redaction-safe `detail`/alerts (only leg_ids/order_ids + stable
  `errors::to_string(category)` tags), fail-closed on null seams, best-effort alert via
  `(void)alerts.send(...)` wrapped in `try { ... } catch (...) {}`, remediation BEFORE the alert.
- Pre-flight is a single fused Kahn topological sort (`topological_order`): builds a leg_id->index map
  rejecting empty + duplicate ids, rejects unknown `depends_on`, and seeds in-degree-0 roots in input
  order; if the produced order does not cover all nodes a cycle remains => `Blocked` with NOTHING
  placed. The same order drives execution; unwind walks it in reverse (newest-first).
- Never-orphan invariant: a leg is placed only if every prerequisite ended `Executed`; otherwise it is
  `SkippedUnmetDependency` and `place_leg` is NOT invoked (asserted in tests via a per-leg placement
  log count == 0, not merely the enum). Skips propagate transitively because dependents then observe a
  non-Executed prerequisite.
- Unwind is best-effort but LOUD: a null OR Error `unwind_leg` leaves the leg `Executed` (live orphan)
  and writes a `CRITICAL: ...` per-leg detail; outcome stays `PartiallyExecutedUnwound` with one
  Critical alert. `LeaveAndAlert` never touches `unwind_leg` and sends one Warning alert. All-Executed
  => `Complete`, no alert/unwind.
- AC-3: `basket_id` = config value if non-empty, else deterministic `"basket-" + to_string(leg count)`
  (no clock/random); `tracked_as_single_unit` echoes config (default true).
- Result/Error construction used: success `ports::ok()` and `ports::BrokerAck{...}` (implicitly
  wrapped into `Result<T>`); failure via `broker_exec::fail(errors::make_error(ErrorCategory::..., msg))`
  in tests; error tagging via `errors::to_string(err.category)`.
- Assumption: `result.legs` is emitted in INPUT order (stable for audit). An empty `legs` vector with a
  valid `place_leg` yields `Complete` (no validation rule forbids it); the spec only enumerates the
  Blocked triggers, none of which an empty basket hits.
- Not built locally (orchestrator builds), per instructions.

### File List
- include/broker_exec/options/basket.hpp (new)
- src/options/basket.cpp (new)
- src/options/basket_test.cpp (new)
- src/options/CMakeLists.txt (edited: added basket.cpp + basket_test.cpp)
