# Story 5.1: Hedge-first execution with naked-sell prevention

Status: ready-for-dev

## Story

As an option seller,
I want the hedge bought before the short is sold,
so that I am never momentarily naked. (FR-16)

## Acceptance Criteria

1. **Given** a hedged short configured hedge-first **When** the basket executes **Then** the hedge is placed FIRST and the
   short is sent ONLY on hedge confirmation.
2. **And** if the hedge FAILS (placement error OR not confirmed), the short is NEVER sent (no naked window ever opens).
3. **And** if the short succeeds but the hedge LATER fails (a confirmed hedge that subsequently vanishes/rejects), an
   IMMEDIATE Critical alert fires AND the configured emergency action runs.

## Tasks / Subtasks

- [ ] Task 1: New module `src/options` + `include/broker_exec/options` (AC: all)
  - [ ] Target `broker_exec_options`; pure orchestration over INJECTED SEAMS. Depends inward only on `domain`, `errors`,
        `ports` (AlertSink). **No new Conan dep.** Mirror `src/modes/CMakeLists.txt`.
- [ ] Task 2: Outcome vocabulary (AC: all) — `include/broker_exec/options/hedge_first.hpp`
  - [ ] `enum class HedgeFirstOutcome { HedgedShortLive, HedgePlacementFailed, HedgeUnconfirmed, ShortPlacementFailed,`
        `NakedShortRemediated };` + stable `to_string`. Meanings:
        - `HedgedShortLive` — hedge confirmed, short placed: the only fully-successful terminal.
        - `HedgePlacementFailed` / `HedgeUnconfirmed` — aborted BEFORE any short was sent (AC-2; never naked).
        - `ShortPlacementFailed` — hedge is live but the short placement failed: SAFE (a lone long hedge is not naked); no
          remediation, return the hedge order id so the caller can decide to keep/close it.
        - `NakedShortRemediated` — short live but hedge later failed: AC-3 fired (Critical alert + emergency action ran).
  - [ ] `struct HedgeFirstResult { HedgeFirstOutcome outcome; std::string hedge_order_id; std::string short_order_id;`
        `bool emergency_action_ran = false; std::string detail; };` (detail redaction-safe).
- [ ] Task 3: Injected seams (AC: all) — keep the executor broker-neutral + unit-testable with NO real broker
  - [ ] `struct HedgeFirstSeams {`
        `std::function<Result<ports::BrokerAck>()> place_hedge;`
        `std::function<Result<bool>(const std::string& hedge_order_id)> confirm_hedge;  // true == confirmed/filled`
        `std::function<Result<ports::BrokerAck>()> place_short;`
        `std::function<Result<bool>(const std::string& hedge_order_id)> recheck_hedge_live;  // AC-3 post-short re-verify`
        `std::function<Result<ports::Ok>()> emergency_action;  // configured square-off/cancel-all etc. };`
  - [ ] Each seam returning an `Error` is treated as failure of THAT step; a null required seam FAILS CLOSED (treated as
        that step failing — never proceed past a missing seam to a send).
- [ ] Task 4: The executor (AC: all) — `hedge_first.cpp`
  - [ ] `[[nodiscard]] HedgeFirstResult execute_hedge_first(const HedgeFirstSeams&, ports::AlertSink&)` — the ordered,
        fail-closed state machine. NO throw (return an outcome, never propagate). Exact order:
        1. `place_hedge`. On Error / null seam -> `HedgePlacementFailed`, short NEVER sent (AC-2). Return.
        2. `confirm_hedge(hedge_id)`. On Error OR `false` OR null seam -> `HedgeUnconfirmed`, short NEVER sent (AC-2).
           (Fail-closed: an ERROR checking confirmation counts as NOT confirmed — never optimistically proceed.) Return.
        3. `place_short`. On Error / null seam -> `ShortPlacementFailed` (hedge stands; SAFE — not naked). Return.
        4. `recheck_hedge_live(hedge_id)`. If it returns `false` OR an Error (fail-closed: can't prove the hedge is live ->
           treat as failed) -> AC-3: send a **Critical** alert (redaction-safe message) AND run `emergency_action` (swallow
           its Result into `emergency_action_ran` + detail; a null emergency seam still alerts + records not-run). Outcome
           `NakedShortRemediated`. If the recheck confirms the hedge is still live -> `HedgedShortLive`.
  - [ ] The alert send Result is swallowed (best-effort; alerting must never derail the safety outcome — mirror `health`/
        Watchdog). Emergency action runs even if the alert send failed.
- [ ] Task 5: CMake — `src/options/CMakeLists.txt` (mirror modes); wire `add_subdirectory(src/options)` into root CMakeLists
      after `src/cli`. `broker_exec_options_tests` (Catch2). No new dep.
- [ ] Task 6: Tests — `src/options/hedge_first_test.cpp` (use a spy AlertSink + lambda seams; assert ORDER via a call log)
  - [ ] AC-1 happy path: place_hedge -> confirm(true) -> place_short -> recheck(true) => `HedgedShortLive`; call order is
        exactly hedge, confirm, short, recheck (a recorded sequence proves hedge precedes short).
  - [ ] AC-2 hedge placement fails: place_hedge Error => `HedgePlacementFailed` AND place_short was NEVER invoked (spy count 0).
  - [ ] AC-2 hedge unconfirmed: confirm returns false => short NEVER invoked; ALSO confirm returns an Error => short NEVER
        invoked (fail-closed-on-error path).
  - [ ] AC-2 null place_short-irrelevant: null confirm seam => `HedgeUnconfirmed`, short never invoked.
  - [ ] short fails: hedge confirmed, place_short Error => `ShortPlacementFailed`, NO emergency action, NO Critical alert
        (a lone hedge is safe).
  - [ ] AC-3 late hedge fail (recheck false): => Critical alert sent (spy saw AlertLevel::Critical) AND emergency_action ran
        AND `emergency_action_ran==true`, outcome `NakedShortRemediated`.
  - [ ] AC-3 recheck Error (can't prove live) => SAME remediation path (fail-closed).
  - [ ] AC-3 emergency seam null => still Critical-alerts, `emergency_action_ran==false`, outcome `NakedShortRemediated`
        (the operator is told even if no auto-remediation is wired).
  - [ ] alert-send failure does NOT suppress emergency action (spy AlertSink returns Error; emergency_action still ran).

## Dev Notes

- **Never-naked is the whole point** (FR-16): the ONLY path that sends the short is hedge-placed-AND-confirmed. Every
  failure or ambiguity before that aborts the short. Fail-closed: an ERROR while confirming/​rechecking counts as "not
  safe", never as "proceed". [architecture.md#FR-16 hedge-first]
- **A lone hedge is SAFE** — `ShortPlacementFailed` is NOT an emergency (long option, no short risk). Only a live short with
  a failed hedge (`NakedShortRemediated`) triggers AC-3.
- **Alerting is best-effort** — swallow the `AlertSink::send` Result; it must never derail or block the emergency action
  (mirror `health::Watchdog`). Emergency action runs regardless of alert delivery.
- **Seam pattern** mirrors the established injected seams (kite HttpClient, alerting POST, marketdata tick). Real wiring to
  `BrokerPort::place`/reconcile + the basket engine is Story 5.2 / composition root, NOT this story.
- **No-throw, no float, redaction-safe `detail`.** Reuse `ports::BrokerAck`, `ports::AlertSink`, `errors` (Result/Error),
  `domain` (scrub for `detail` if it ever embeds broker text).

### References
- [Source: epics.md#Story 5.1, FR-16] [architecture.md#FR-16, #FR-31 emergency action]
- [Source: include/broker_exec/ports/broker_port.hpp (BrokerAck)] [include/broker_exec/ports/alert_sink.hpp]
- [Source: docs/conventions.md] [Source: src/health/watchdog.cpp (best-effort-alert sibling)]

## Dev Agent Record
### Agent Model Used
claude-opus-4-8[1m] (Claude Opus 4.8, 1M context)

### Completion Notes List
- New module `broker_exec_options` (namespace `broker_exec::options`): pure, ordered,
  fail-closed orchestration over injected seams; no real broker, no transport, no new
  Conan dep. Mirrors `src/modes` for CMake + doc-comment density.
- State machine `execute_hedge_first` implements the exact 4-step order
  (place_hedge -> confirm_hedge -> place_short -> recheck_hedge_live). Fail-closed at
  every step: null seam OR Error OR (for the bool seams) `false` is treated as that
  step failing; the short is reached ONLY on a placed-AND-confirmed hedge (AC-1/AC-2).
- `ShortPlacementFailed` is SAFE (lone long hedge): no Critical alert, no emergency
  action, hedge_order_id returned. Only a live short whose hedge can no longer be
  proven live triggers AC-3 (`NakedShortRemediated`): a Critical alert (Result
  swallowed, mirroring `health::Watchdog`) + the emergency action, which runs even
  when the alert send returns an Error.
- Redaction: alert + `detail` messages are static or carry only the stable
  error-category NAME (`errors::to_string(category)`) — no broker text, no
  token-shaped content; `domain::scrub` was not needed (nothing embeds broker text).
- No throw across the boundary, no double/float, no OS API / `#ifdef`.
- Result/Error construction idioms used (verbatim from siblings):
  - success value: `return ports::BrokerAck{...};`, `return true;`, `return ports::ok();`
  - failure: `broker_exec::fail(errors::make_error(errors::ErrorCategory::X, "msg"))`
  - swallowed alert: `(void)alerts.send(ports::AlertLevel::Critical, "...")`
  - consumption: `if (!result)` / `result.value()` / `result.error()` / `.has_value()`
- Tests: 14 Catch2 cases covering every Task-6 bullet (spy AlertSink with
  configurable send-failure + lambda seams recording call order). AC-1 asserts exact
  order `{hedge, confirm, short, recheck}`; AC-2 asserts place_short count == 0 on
  placement-fail, confirm-false, confirm-Error, null-confirm-seam, and null-hedge-seam.
- Not built locally (orchestrator builds). Confirmed against the project `expected`
  surface (`has_value()`/`operator bool`/`value()`/`error()`).

### File List
- include/broker_exec/options/hedge_first.hpp (new)
- src/options/hedge_first.cpp (new)
- src/options/hedge_first_test.cpp (new)
- src/options/CMakeLists.txt (new)
- CMakeLists.txt (orchestrator-owned edit: `add_subdirectory(src/options)` after `src/cli`)
