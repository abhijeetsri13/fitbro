# Story 2.10: Four-level risk engine

Status: ready-for-dev

## Story

As an operator,
I want account/strategy/instrument/order risk enforced on every order,
so that configured limits demonstrably block offending orders. (FR-15)

## Acceptance Criteria

1. **Given** configured limits **When** an order would violate one (daily loss, max lots, max margin, max open
   positions, market-order block, slippage, illiquid/stale-data, time windows, etc.) **Then** it is blocked with
   the violated rule named.
2. **And** each level (account/strategy/instrument/order) is independently testable.
3. **And** a strategy can be stopped without stopping others.

## Tasks / Subtasks

- [ ] Task 1: Add the risk engine to the EXISTING `risk` module (AC: all)
  - [ ] `include/broker_exec/risk/risk_engine.hpp` + `src/risk/risk_engine.cpp`; add to `broker_exec_risk`. Pure logic;
        no new deps (domain/errors only).
- [ ] Task 2: Limits + state value types (AC: 1, 2, 3)
  - [ ] `struct RiskLimits` — integer (paise / counts), all optional-with-sane-default (0 / disabled = "no limit"):
        account: `daily_loss_limit_paise` (max tolerated loss, a positive magnitude; 0 = off), `max_open_positions`
        (0 = off), `max_account_margin_paise` (0 = off);
        order: `max_order_value_paise` (0 = off), `bool block_market_orders`, `max_slippage_bps` (0 = off);
        strategy/instrument caps: `max_lots_per_strategy`, `max_lots_per_instrument` (0 = off).
  - [ ] `struct RiskState` — the current world the order is evaluated against (injected; real wiring later):
        `account_pnl_paise` (signed; negative = loss), `open_positions` (count), `used_margin_paise`,
        `order_value_paise` (this order's notional; for a Market order the caller supplies an estimate, 0 = unknown ->
        skip the value/margin/slippage checks that need it, but DO log/skip-safely — never silently pass a margin check
        it cannot evaluate; treat unknown-margin as fail-closed only where the limit is set), `order_lots`,
        `strategy_lots` (this strategy's current lots), `instrument_lots` (this instrument's current lots),
        `bool strategy_enabled` (false = strategy stopped — AC-3), `strategy_pnl_paise`, `strategy_daily_loss_limit_paise`,
        `bool data_tradable` (false = illiquid/stale price -> block), `slippage_bps` (this order's est. slippage).
- [ ] Task 3: The four levels (each independently callable) (AC: 1, 2)
  - [ ] `Result<Ok> check_account(intent, inst, limits, state) const` — daily-loss breached (pnl <= -limit), max-open-positions
        (entries only; >= limit), max-account-margin (used + order_value > limit; if order_value unknown AND limit set ->
        fail-closed). Name the rule, e.g. "risk[account]: daily loss limit breached".
  - [ ] `Result<Ok> check_strategy(...)` — strategy stopped (!strategy_enabled -> reject; AC-3), strategy daily loss
        (strategy_pnl <= -strategy_daily_loss_limit), strategy max lots (strategy_lots + order_lots > max_lots_per_strategy).
  - [ ] `Result<Ok> check_instrument(...)` — instrument max lots (instrument_lots + order_lots > max_lots_per_instrument),
        illiquid/stale (!data_tradable -> reject).
  - [ ] `Result<Ok> check_order(...)` — market-order block (block_market_orders && order_type==Market), max order value
        (order_value > max_order_value), slippage (slippage_bps > max_slippage_bps).
  - [ ] All return `RiskRejected` (SuggestedAction default) Errors NAMING the level + rule. A limit of 0 / flag false means
        "no limit" and never blocks. Exits (risk-reducing) are NOT special-cased here — the GATE owns exit-exemption (2.8);
        the engine evaluates the order it is given (the gate decides whether to even call risk for an exit).
- [ ] Task 4: Composite + gate adapter (AC: 1)
  - [ ] `Result<Ok> check_all(...) const` — runs account -> strategy -> instrument -> order in that order; FIRST violation
        wins and is named. (This is what the runtime binds as the gate's injected `risk_check` from Story 2.8.)
  - [ ] A convenience that produces a `std::function<Result<Ok>()>` bound to (intent, inst, limits, state) so it drops into
        `GateContext::risk_check` directly (document the wiring; no actual dep on the gate type needed).
- [ ] Task 5: CMake — extend `src/risk/CMakeLists.txt`
  - [ ] Add `risk_engine.cpp` to `broker_exec_risk`. Add `risk_engine_test.cpp` to `broker_exec_risk_tests` (or a new
        `broker_exec_risk_engine_tests` exe). No new deps.
- [ ] Task 6: Tests (AC: 1, 2, 3) — `src/risk/risk_engine_test.cpp`
  - [ ] Each LEVEL independently: a clean state passes; flipping ONE limit/state to a violation blocks with the named rule —
        for every rule listed above (daily loss, max open positions, max margin, strategy stopped, strategy daily loss,
        strategy max lots, instrument max lots, illiquid, market-order block, max order value, slippage).
  - [ ] AC-3: strategy A disabled (strategy_enabled=false) -> its order blocked; strategy B enabled, same everything else ->
        passes (one stopped, others fine).
  - [ ] "no limit": a 0/false limit never blocks even at extreme state values.
  - [ ] check_all ordering: set an account AND an order violation -> the account rule is the one named.
  - [ ] fail-closed: a set margin limit with unknown order_value (0) -> blocked (never silently passed).

## Dev Notes

- **Four levels, independently testable** (AC-2): expose check_account/strategy/instrument/order + check_all. [architecture.md#IA risk, FR-15]
- **Gate integration:** check_all is the gate's injected `risk_check` (Story 2.8 GateContext.risk_check). [2-8 loop-state]
- **Per-strategy stop** (AC-3) = `strategy_enabled=false` blocks that strategy only; the engine is stateless over (intent,state). [FR-15, FR-32 seed]
- **No float**: paise/bps/counts are integers. Slippage in basis points (int). [docs/conventions.md#Money]
- **Errors:** `RiskRejected`; message names "risk[<level>]: <rule>". 0/false limit = off (never blocks). Fail-closed where a
  set limit cannot be evaluated (unknown margin). [docs/conventions.md#Errors]
- **Reuse:** `domain::OrderIntent`/`Instrument`/`Money`/`Price`/`Quantity`, `errors::Error`/`RiskRejected`, `ports::Ok`.

### References
- [Source: epics.md#Story 2.10] [architecture.md#risk, FR-15] [Source: src/risk/validation_gate.* (sibling in the same module)]
- [Source: docs/conventions.md]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
