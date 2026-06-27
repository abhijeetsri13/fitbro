# Story 2.8: Pre-submission validation gate

Status: ready-for-dev

## Story

As an operator,
I want a non-bypassable validation gate before any broker call,
so that no invalid or unsafe order reaches the broker. (FR-6)

## Acceptance Criteria

1. **Given** the gate **When** an order is submitted **Then** it checks lot/freeze (over-freeze ⇒ slice), tick,
   product, exchange, funds (fail-closed-on-stale), risk, time-window, duplicate, hedge, kill-switch, and
   UNKNOWN-pause — **naming the failed check**.
2. **And** strategy code has no path to bypass it.
3. **And** an entry under an active UNKNOWN-pause is blocked while a risk-reducing exit passes.

## Tasks / Subtasks

- [ ] Task 1: `risk` module + gate framework (AC: all)
  - [ ] `include/broker_exec/risk/` + `src/risk/`; target `broker_exec_risk` (+ alias). Depends inward on `domain`,
        `refdata`, `capabilities`, `ports`, `errors`. The gate composes already-built modules and INJECTED predicates for
        modules not yet built (funds=2.11, risk-engine=2.10, slicer=2.9, kill-switch=3.8, UNKNOWN-pause runtime) so the gate
        is complete and testable now and later stories wire real impls.
  - [ ] `GateDecision`: an enum/struct result — `Allow`, `AllowWithSlicing` (over-freeze, slice-mode), or a typed `Error`
        from the FIRST failing check (the Error message NAMES the check, e.g. "gate: tick check failed: ...").
  - [ ] `GateContext`: the order under test + flags: `bool is_risk_reducing` (exit/square-off/hedge-completion — exempt from
        entry-only blocks), resolved `domain::Instrument` (from the instrument master), and the inputs below.
- [ ] Task 2: The checks (ordered, fail-closed) (AC: 1, 3)
  - [ ] Implement an ordered pipeline of checks; the FIRST failure returns its named Error; an exit (is_risk_reducing) is
        EXEMPT from kill-switch(entry), UNKNOWN-pause, time-window(entry-cutoff), and duplicate-entry blocks (a protective exit
        is never frozen). Suggested order (cheapest/safety first):
        1. **kill-switch** — injected `KillState` (soft/strategy/broker/account block ENTRIES, allow exits; panic blocks all
           new but the runtime drives panic exits separately). Active entry-block + entry -> Error.
        2. **UNKNOWN-pause** — injected `bool unknown_pause_active`. Active + entry -> Error; exit passes (AC-3).
        3. **duplicate** — injected `std::function<bool(client_ref, signal_hash)>` is_duplicate. Duplicate -> DuplicateOrder Error.
        4. **exchange** — instrument's exchange must be in the configured allowed set / non-empty + match.
        5. **product** — intent.product allowed for this instrument/segment (use a small allowed-products rule; reject otherwise).
        6. **lot** — quantity > 0, >= lot_size, and quantity % lot_size == 0 (lot-aligned) else Validation Error.
        7. **tick** — for limit/SL orders, price > 0 and price aligned to tick_size (price_paise % tick_paise == 0).
        8. **freeze** — quantity > instrument.freeze_qty (and freeze_qty>0): in slice-mode (default) -> GateDecision
           AllowWithSlicing (NOT an error); in reject-mode (config) -> Validation Error. (The actual fan-out is Story 2.9.)
        9. **time-window** — for entries, `calendar.require_entry_allowed()`; exits exempt. (TradingCalendar from 2.7.)
        10. **funds** — injected `FundsCheck` returning Result; fail-closed when the funds view is stale (DataStale) — margin-
            sensitive entries only; exits exempt. (Real impl = Story 2.11.)
        11. **risk** — injected `RiskCheck` returning Result (account/strategy/instrument/order). (Real impl = Story 2.10.)
        12. **hedge** — injected `HedgeCheck` returning Result (naked-sell prevention; real impl = Story 5.1). Default pass.
  - [ ] Every failure Error NAMES the check (AC-1) and carries the right taxonomy category/action.
- [ ] Task 3: Non-bypassable surface (AC: 2)
  - [ ] The gate exposes a single `Result<GateDecision> validate(const GateContext&) const`; document that the runtime's
        ONLY path to dispatch() runs through it (structural enforcement lands with the runtime; here, the gate is the single
        validate function and there is no partial/"skip checks" variant).
- [ ] Task 4: CMake (orchestrator pre-wires root add_subdirectory(src/risk); NO new Conan dep)
  - [ ] `src/risk/CMakeLists.txt`: target `broker_exec_risk` links PUBLIC `broker_exec::domain` `broker_exec::errors`;
        PRIVATE `broker_exec::refdata` `broker_exec::capabilities` `broker_exec::ports` warnings+sanitizers; test exe
        `broker_exec_risk_tests` (also link refdata + clock for building instruments/calendar in-test).
- [ ] Task 5: Tests (AC: 1, 2, 3) — `src/risk/validation_gate_test.cpp`
  - [ ] Build a known instrument (lot 50, tick 0.05=5 paise, freeze 1800) + a fresh TradingCalendar in-window; inject
        passing predicates -> a valid entry -> Allow.
  - [ ] Each check fails in isolation with all others passing, and the Error NAMES that check: bad lot (qty 75 not multiple
        of 50), bad tick (price 100.03), wrong exchange, disallowed product, duplicate, kill-switch active, UNKNOWN-pause
        active, out-of-window (calendar past cutoff), stale funds (fail-closed), risk reject, hedge reject.
  - [ ] over-freeze (qty 2000 > 1800) in slice-mode -> AllowWithSlicing (not an error); in reject-mode -> Error.
  - [ ] AC-3: with UNKNOWN-pause active AND kill-switch(entry) active, an ENTRY is blocked but a risk-reducing EXIT
        (is_risk_reducing=true) passes those checks.
  - [ ] ordering: when multiple checks would fail, the FIRST in the pipeline is the one named.

## Dev Notes

- **Composition:** real `refdata::InstrumentMaster` (resolve symbol->Instrument) + `refdata::TradingCalendar`
  (require_entry_allowed); injected std::function predicates for funds/risk/hedge/duplicate/kill/UNKNOWN-pause (real impls in
  2.9/2.10/2.11/3.8 + runtime). [architecture.md#risk/gate, #FR-6, #CC-3 UNKNOWN-pause scope, #B freeze slicing]
- **Exit exemption:** risk-reducing ops (square-off/cancel/hedge-completion/emergency exit) bypass entry-only blocks
  (kill-switch entry, UNKNOWN-pause, entry-cutoff, duplicate-entry) — a protective leg is never frozen. [architecture.md#CC-3, #B]
- **Over-freeze ⇒ slice (default), reject (config)** — NOT "invalid quantity". [architecture.md#IBR-2 freeze slicing]
- **Errors:** name the failed check; categories — Validation (lot/tick/product/exchange), DataStale (stale funds),
  RiskRejected (risk), DuplicateOrder (dup), MarketClosed (window), NotSupported/BrokerRejected as apt; SuggestedAction fail-closed.
- **No double/float:** lot/tick/freeze use integer Quantity/Price. [docs/conventions.md]
- **Reuse:** `domain::OrderIntent`/`Instrument`/`Money`/`Quantity`/`Price`, `refdata::*`, `capabilities::*`, `errors`.

### References
- [Source: epics.md#Story 2.8] [architecture.md#risk/gate, #FR-6, #CC-3, #IBR-2]
- [Source: docs/conventions.md] [Source: domain/types.hpp, refdata/*, capabilities/*]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
