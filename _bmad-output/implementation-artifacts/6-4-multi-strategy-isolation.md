# Story 6.4: Multi-strategy isolation

Status: ready-for-dev

## Story

As an operator,
I want strategies isolated within an account,
so that they don't interfere or close each other's positions. (FR-32)

## Acceptance Criteria

1. **Given** two strategies trading the same instrument **When** both run **Then** each keeps SEPARATE virtual positions,
   tags, and P&L; neither squares off the other's position UNLESS netting is enabled.
2. **And** a global account limit still BLOCKS the combined exposure (even when each strategy is individually within limit).
3. **And** one strategy can be STOPPED without stopping the other.

## Tasks / Subtasks

- [ ] Task 1: New module `src/isolation` + `include/broker_exec/isolation` (AC: all)
  - [ ] Target `broker_exec_isolation`; pure in-memory logic. Depends inward on `domain` (Money/Quantity/Side), `errors`,
        `ports` (Ok). **No new Conan dep.** Mirror `src/modes/CMakeLists.txt`. No I/O, no clock, no threads.
- [ ] Task 2: Per-strategy virtual book (AC: 1) — `include/broker_exec/isolation/strategy_book.hpp` / `.cpp`
  - [ ] A `StrategyBook` holding, keyed by `strategy_id` (std::string), each strategy's isolated state: per-symbol signed
        net quantity + average cost + realized P&L + an optional free-form `tag`, and an `active` flag (Task 4).
  - [ ] `struct Position { std::int64_t net_qty; domain::Money avg_cost; domain::Money realized_pnl; };` (net_qty signed:
        +long / -short; avg_cost is per-unit paise; integers only, NO float).
  - [ ] `void apply_fill(const std::string& strategy_id, const std::string& symbol, domain::Side side,`
        `domain::Quantity qty, domain::Money price)` — updates ONLY that strategy's (symbol) position. Average-cost model:
        - signed fill `f = (side==Buy ? +qty : -qty)`.
        - If the position is flat or the fill is in the SAME direction as `net_qty` (or flat): new avg_cost = weighted average
          of existing (|net_qty|@avg_cost) and (|f|@price); net_qty += f.
        - If the fill OPPOSES net_qty (reduces): realize P&L on the closed quantity `c = min(|f|, |net_qty|)`:
          `realized_pnl += (price - avg_cost) * c * (net_qty > 0 ? +1 : -1)` (Money paise arithmetic). net_qty += f. If the
          fill exceeds the position (crosses zero), the remainder OPENS a new position at `price` (avg_cost = price). When
          net_qty returns to 0, avg_cost resets to 0.
  - [ ] `[[nodiscard]] Position position_of(strategy_id, symbol) const;` (a flat/zero Position if unknown — isolation: a
        strategy never sees another's book). `[[nodiscard]] std::vector<std::string> strategies() const;`
        `void set_tag(strategy_id, tag); [[nodiscard]] std::string tag_of(strategy_id) const;`
  - [ ] **Isolation invariant:** `apply_fill` for strategy A NEVER mutates strategy B's positions/P&L/tag. (Tested directly.)
- [ ] Task 3: Square-off respects isolation / netting (AC: 1) — `strategy_book.hpp`
  - [ ] `struct FlattenLeg { std::string symbol; std::int64_t qty; domain::Side side; };` (the offsetting order to flatten).
  - [ ] `[[nodiscard]] std::vector<FlattenLeg> square_off(const std::string& strategy_id, bool netting_enabled);`
        - `netting_enabled == false` (default isolation): returns the offsetting legs for ONLY `strategy_id`'s non-flat
          positions and zeroes ONLY that strategy's book. Other strategies' books are UNTOUCHED (AC-1: never squares off
          another's position).
        - `netting_enabled == true`: returns the offsetting legs for the ACCOUNT-NET position per symbol (summed across ALL
          strategies) and zeroes every strategy's positions for those symbols (netting is an explicit account-level action).
  - [ ] A flat position contributes no leg. `side` is the offsetting side (net long -> Sell, net short -> Buy); `qty` is
        `|net_qty|`.
- [ ] Task 4: Global account limit + independent stop (AC: 2, 3) — `strategy_book.hpp`
  - [ ] `[[nodiscard]] domain::Money account_exposure(const std::function<domain::Money(const std::string& symbol)>&`
        `mark_price) const;` — COMBINED notional across ALL strategies: `sum over (strategy,symbol) of |net_qty| *`
        `mark_price(symbol)`. (mark_price seam keeps it I/O-free.)
  - [ ] `[[nodiscard]] Result<ports::Ok> check_new_order(const std::string& strategy_id, const std::string& symbol,`
        `domain::Quantity qty, domain::Money price, domain::Money account_max_exposure,`
        `const std::function<domain::Money(const std::string&)>& mark_price) const;` — fail-closed: if the strategy is
        STOPPED -> RiskRejected (naming the strategy). Else if `account_exposure(mark) + |qty|*price` would EXCEED
        `account_max_exposure` -> RiskRejected (AC-2: the GLOBAL limit blocks the COMBINED exposure even when this strategy is
        individually small). `account_max_exposure <= 0` means "no limit" only if explicitly 0 — a NEGATIVE cap is a misconfig
        and must reject (fail-closed). Otherwise `ok()`.
  - [ ] `void stop_strategy(strategy_id); void resume_strategy(strategy_id); [[nodiscard]] bool is_active(strategy_id) const;`
        A stopped strategy: `check_new_order` rejects its new orders; `is_active` is false. Stopping strategy A does NOT change
        B's `is_active` or B's ability to trade (AC-3). An unknown strategy is active by default (it has placed nothing).
- [ ] Task 5: CMake — `src/isolation/CMakeLists.txt` (mirror modes); wire `add_subdirectory(src/isolation)` into root
      CMakeLists after `src/options`. `broker_exec_isolation_tests` (Catch2). No new dep.
- [ ] Task 6: Tests — `src/isolation/strategy_book_test.cpp`
  - [ ] AC-1 isolation: strategies A and B both Buy 50 of "NIFTY..CE". position_of(A) and position_of(B) are EACH +50 and
        independent; a further fill on A leaves B's position/P&L EXACTLY unchanged.
  - [ ] AC-1 separate P&L: A buys 50@100 then sells 50@120 => A.realized_pnl == +1000 rupees (in paise); B (who only bought)
        has realized_pnl 0 — P&L is per-strategy.
  - [ ] AC-1 square-off isolation: square_off(A, netting=false) returns A's offsetting leg (Sell 50) and zeroes A; B's +50
        is UNTOUCHED. square_off(A, netting=true) nets across A+B for the symbol and zeroes both.
  - [ ] AC-2 global limit: two strategies each within their own size but whose COMBINED exposure exceeds the cap => a new
        order that pushes combined over `account_max_exposure` is RiskRejected, even though the placing strategy is small.
        A negative cap rejects (fail-closed); within-cap returns ok.
  - [ ] AC-3 independent stop: stop_strategy(A) => check_new_order(A,...) RiskRejected AND is_active(A)==false, BUT
        is_active(B)==true and check_new_order(B,...) still ok(); resume_strategy(A) re-enables A.
  - [ ] average-cost / cross-zero: a Buy 100@100 then Sell 150@110 leaves net_qty -50, realized P&L on the 100 closed, and the
        new short opened at 110 (avg_cost==110); returning to flat resets avg_cost to 0.

## Dev Notes

- **Strategies must not interfere** (FR-32): each strategy's positions/tags/P&L are a separate virtual book keyed by
  strategy_id; a fill or square-off touches only the owning strategy unless netting is explicitly enabled (an account-level
  action). This is the in-memory isolation the runtime maintains alongside the broker's single real position. [architecture.md#FR-32]
- **The global limit is the backstop** (AC-2): per-strategy sizing is not enough — the COMBINED account exposure is checked
  against the account cap, so two individually-small strategies can't jointly breach it. Fail-closed on a negative cap.
- **Independent lifecycle** (AC-3): stop one strategy (block its new entries) without affecting the others.
- **Integer money/qty, no float, no-throw, redaction-safe.** Reuse `domain::Money`/`Quantity`/`Side`, `errors`, `ports::Ok`.
  Mark prices come through a `std::function` seam — no market-data dependency here.

### References
- [Source: epics.md#Story 6.4, FR-32] [architecture.md#FR-32 multi-strategy isolation]
- [Source: include/broker_exec/domain/money.hpp (Money/Quantity)] [include/broker_exec/domain/enums.hpp (Side)]
- [Source: docs/conventions.md] [Source: src/options/* (sibling injected-seam style)]

## Dev Agent Record
### Agent Model Used
claude-opus-4-8[1m] (Claude Code dev agent)

### Completion Notes List
- New `broker_exec::isolation` module: `StrategyBook` is a pure in-memory, no-throw,
  integer-paise virtual book keyed by `strategy_id`. No I/O, clock, threads, OS API, or
  new Conan dep. Uses `std::map` for deterministic iteration (strategies(), square_off,
  netting, exposure).
- Error idiom mirrors `src/modes/trading_mode.cpp`: a `rejected(msg)` helper builds
  `errors::make_error(ErrorCategory::RiskRejected, msg)` then sets
  `.action = SuggestedAction::BlockStrategy`. Messages name only `strategy_id` / `symbol`
  (non-secret caller ids), redaction-safe.
- Average-cost model (all int64 paise): signed fill `f = Buy? +qty : -qty`, current net `n`.
  Opening/same-direction (n==0 or sign(f)==sign(n)):
  `avg_cost = (|n|*avg_cost + |f|*price) / (|n|+|f|)` (integer divide), `net_qty = n+f`.
  Opposing/reducing: `c = min(|f|,|n|)`,
  `realized_pnl += (price - avg_cost) * c * (n>0? +1 : -1)`, `net_qty = n+f`; cross-zero
  (`|f|>|n|`) reopens at `price`; returning to flat resets `avg_cost = 0`. A zero-qty fill
  is a no-op (also guards the divide).
- `check_new_order` order: stopped -> RiskRejected/BlockStrategy; negative cap -> reject
  (fail-closed); zero cap -> no-limit ok(); `account_exposure(mark) + |qty|*price > cap`
  (strict `>`) -> RiskRejected; else ok().
- `square_off(.., false)` flattens & zeroes only the named strategy (keeps realized_pnl);
  `square_off(.., true)` nets net_qty per symbol across all strategies, emits one leg per
  non-zero account-net, and zeroes those symbols in every strategy.
- Tests cover every Task-6 bullet (AC-1 isolation + per-strategy P&L + square-off isolation
  & netting, AC-2 combined-exposure/negative/zero/at-cap, AC-3 independent stop/resume,
  cross-zero + blend average cost, tags/strategies()). Verified P&L unit math:
  A buy50@100 sell50@120 => `(12000-10000)*50 = 100000` paise == `Money::from_rupees(1000)`.
- Assumption: account-level netting (`netting_enabled=true`) zeroes a netted symbol's
  positions in every strategy even when the account-net is flat (the operator is collapsing
  the virtual books to match the single real broker position) — a flat net still emits no
  leg.

### File List
- include/broker_exec/isolation/strategy_book.hpp (new)
- src/isolation/strategy_book.cpp (new)
- src/isolation/strategy_book_test.cpp (new)
- src/isolation/CMakeLists.txt (new)
- CMakeLists.txt (edited: `add_subdirectory(src/isolation)` after `src/options`)
