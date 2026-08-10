# Story 5.4: Margin/SPAN shock simulation (capability-gated)

Status: ready-for-dev

## Story

As an option seller,
I want pre-trade margin modeled under a volatility shock,
so that I don't open a basket that a vol spike would force-liquidate. (FR-18)

## Acceptance Criteria

1. **Given** a broker with a basket/SPAN margin source **When** a basket is submitted **Then** margin NOW and under the
   configured shock is modeled against the broker's RMS auto-square-off threshold and a CROSSING basket is flagged/blocked
   PRE-submission.
2. **And** where no basket/SPAN source exists, the shock sim reports "unavailable on this broker" AND a NET-SHORT basket's
   margin FAILS CLOSED (blocked — NEVER summed-legs, which under-counts short-spread margin).
3. **And** the result is AUDITED.

## Tasks / Subtasks

- [ ] Task 1: Extend the EXISTING `options` module (AC: all) — `include/broker_exec/options/margin_shock.hpp` /
      `src/options/margin_shock.cpp` + `margin_shock_test.cpp` into `src/options/CMakeLists.txt`.
  - [ ] Pure logic over INJECTED SEAMS, capability-gated. Depends inward on `domain` (Money), `errors`,
        `capabilities` (Support). **No new Conan dep.** Reuse the 5.1-5.3 seam/fail-closed style.
- [ ] Task 2: Vocabulary (AC: all) — `margin_shock.hpp`
  - [ ] `struct ShockMarginModel { domain::Money margin_now; domain::Money margin_under_shock; };` (what the SPAN source
        returns: blocked margin now and under the configured vol shock — integer paise, no float).
  - [ ] `struct MarginShockInputs { domain::Money available_margin; bool basket_is_net_short; };` (available_margin is the
        deployable funds; net-short flags an undefined-risk basket for the AC-2 fail-closed rule).
  - [ ] `enum class MarginShockOutcome { WithinShockLimit, BlockedMarginShock, BlockedUnavailableNetShort,`
        `AllowedUnavailableBounded };` + stable `to_string`:
        - `WithinShockLimit` — SPAN available; shocked margin fits within available (basket allowed).
        - `BlockedMarginShock` — SPAN available; shocked margin would CROSS the RMS auto-square-off threshold: BLOCK (AC-1).
        - `BlockedUnavailableNetShort` — no SPAN source AND the basket is net-short: FAIL CLOSED, block, never sum legs (AC-2).
        - `AllowedUnavailableBounded` — no SPAN source but the basket is NOT net-short (defined-risk/long): allowed, sim
          reported unavailable (AC-2 only mandates the net-short fail-closed; a bounded-risk basket is not force-liquidatable
          the same way).
  - [ ] `struct MarginShockResult { MarginShockOutcome outcome; bool span_available; domain::Money margin_now;`
        `domain::Money margin_under_shock; domain::Money available_margin; bool blocked; std::string detail; };`
        (`blocked` is a convenience = outcome is one of the two Blocked* — the gate reads this.)
- [ ] Task 3: Injected seams (AC: 1, 3)
  - [ ] `struct MarginShockSeams { std::function<Result<ShockMarginModel>()> span_margin_source;  // the broker SPAN/basket`
        `// margin source (null/absent => unavailable); std::function<void(const MarginShockResult&)> audit; };`
  - [ ] A null `span_margin_source` OR a source returning an Error => treated as UNAVAILABLE (cannot model => fail-closed for
        net-short). The `audit` seam (if present) is ALWAYS called with the final result before return (AC-3) — every path,
        including the blocked + unavailable ones.
- [ ] Task 4: The evaluator (AC: all) — `margin_shock.cpp`
  - [ ] `[[nodiscard]] MarginShockResult evaluate_margin_shock(capabilities::Support span_support,`
        `const MarginShockInputs&, const MarginShockSeams&)` — NO throw.
  - [ ] **Capability gate (fail-closed):** SPAN is "available" ONLY when `span_support == capabilities::Support::Supported`
        AND `span_margin_source` is non-null AND the source call returns a value. `Unknown`/`Unsupported` (the fail-closed
        tri-state default) or a null/erroring source => UNAVAILABLE.
  - [ ] **Available path:** call `span_margin_source()`. Record margin_now/under_shock. CROSSING test (AC-1): the basket
        CROSSES the RMS auto-square-off threshold when `margin_under_shock > inputs.available_margin` (the shocked requirement
        exceeds deployable funds, so the broker RMS would force-liquidate). `>` strictly: exactly-equal is within (documented).
        Crossing => `BlockedMarginShock`; else `WithinShockLimit`.
  - [ ] **Unavailable path (AC-2):** `inputs.basket_is_net_short` => `BlockedUnavailableNetShort` (detail: "SPAN/basket-margin
        unavailable on this broker; net-short basket blocked — no summed-legs fallback"). Not net-short =>
        `AllowedUnavailableBounded` (detail: "SPAN unavailable; basket is not net-short (bounded risk) — allowed").
  - [ ] Set `span_available` + `blocked` accordingly; populate the Money fields (margin_now/under_shock are zero/`from_paise(0)`
        on the unavailable path — documented as not-modeled). **Call `seams.audit(result)` if present, then return** (AC-3).
        `detail` is redaction-safe (Money + capability/outcome names only; no secrets).
- [ ] Task 5: CMake — add `margin_shock.cpp` + `margin_shock_test.cpp` to `src/options/CMakeLists.txt`; link
      `broker_exec_capabilities` (PUBLIC — `capabilities::Support` is in the function signature). No new dep.
- [ ] Task 6: Tests — `src/options/margin_shock_test.cpp` (lambda seams + an audit spy capturing the result)
  - [ ] AC-1 within: Supported + source{now=100rs, shock=400rs}, available=500rs => `WithinShockLimit`, blocked==false.
  - [ ] AC-1 crossing blocked: Supported + source{shock=600rs}, available=500rs => `BlockedMarginShock`, blocked==true.
  - [ ] AC-1 boundary: shock == available (exactly) => `WithinShockLimit` (documented strict `>` crossing).
  - [ ] AC-2 unavailable + net-short: Support::Unsupported (and/or null source) + net_short=true => `BlockedUnavailableNetShort`,
        blocked==true; ALSO Support::Unknown => same (fail-closed default); ALSO Supported-but-source-returns-Error + net_short
        => fail-closed block (cannot model).
  - [ ] AC-2 unavailable + not net-short: Unsupported + net_short=false => `AllowedUnavailableBounded`, blocked==false.
  - [ ] AC-2 NEVER summed-legs: assert the unavailable net-short block does NOT consult any per-leg margin sum — there is no
        such fallback path (the test documents intent: the only available-margin input is SPAN; absence => block, not sum).
  - [ ] AC-3 audited: the audit spy is invoked exactly once with the SAME outcome on EVERY path (within, blocked-shock,
        blocked-unavailable-net-short, allowed-unavailable-bounded); a null audit seam does not crash.

## Dev Notes

- **Don't open what a vol spike force-liquidates** (FR-18): the shock margin is modeled BEFORE submission and a basket whose
  shocked requirement exceeds deployable funds is blocked pre-trade (the broker's RMS auto-square-off is the line). [architecture.md#FR-18]
- **Capability-gated, fail-closed** — `capabilities::Support` defaults to `Unknown` (= not Supported), so a broker that never
  advertised SPAN/basket margin is treated as unavailable; a net-short basket is then BLOCKED, never approximated by summing
  per-leg margins (which badly under-counts a short spread and is the exact trap FR-18 guards against). [#2.5 capability model]
- **Audited** (AC-3): every evaluation (allowed or blocked) is handed to the audit seam — the decision + the modeled Money is
  the evidence trail. Reuse `domain::Money` (integer paise), `capabilities::Support`, `errors`. No throw, no float.

### References
- [Source: epics.md#Story 5.4, FR-18] [architecture.md#FR-18 SPAN shock, #2.5 capability model]
- [Source: include/broker_exec/capabilities/*.hpp (Support, Capability::MarginShockSim/BasketMargin)]
- [Source: include/broker_exec/domain/money.hpp] [include/broker_exec/options/sliced_leg.hpp (5.3 sibling)] [docs/conventions.md]

## Dev Agent Record
### Agent Model Used
claude-opus-4-8[1m] (Claude Opus 4.8, 1M context)

### Completion Notes List
- Implemented `evaluate_margin_shock` in the EXISTING `options` module, mirroring the 5.3 sibling
  (`sliced_leg.hpp/.cpp`) seam style, no-throw discipline, and header-doc density. 5.4 uses an
  `audit` seam instead of an AlertSink.
- Capability gate is fail-closed: SPAN is "available" ONLY when
  `span_support == capabilities::Support::Supported` AND `span_margin_source` is non-null AND the
  source Result holds a value. `Unknown` / `Unsupported`, a null source, or a source Error all fall
  through to the UNAVAILABLE path. `.value()` is never called on an errored Result.
- Crossing test is a STRICT integer `>` on `domain::Money` (paise): `margin_under_shock > available_margin`
  => `BlockedMarginShock`; exactly-equal is `WithinShockLimit` (boundary documented + tested).
- UNAVAILABLE path zeroes the Money fields via `domain::Money::from_paise(0)` (documented not-modeled),
  fails closed for net-short (`BlockedUnavailableNetShort`, never summed-legs), allows bounded/non-net-short
  (`AllowedUnavailableBounded`).
- `available_margin` is always set from the input; the `audit` seam (when present) is called on EVERY
  path before return (AC-3); a null audit seam is skipped and does not crash. `detail` is redaction-safe
  (only Money.to_string() + outcome/capability wording).
- No throw, no float/double, no `#ifdef`/OS API, no new Conan dep. Did not build (orchestrator builds).

### File List
- include/broker_exec/options/margin_shock.hpp (new)
- src/options/margin_shock.cpp (new)
- src/options/margin_shock_test.cpp (new)
- src/options/CMakeLists.txt (edited: +margin_shock.cpp, +margin_shock_test.cpp, +broker_exec_capabilities PUBLIC link)
