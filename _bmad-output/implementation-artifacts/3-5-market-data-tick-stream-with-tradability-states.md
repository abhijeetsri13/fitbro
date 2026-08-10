# Story 3.5: Market-data tick stream with tradability states

Status: ready-for-dev

## Story

As a strategy author,
I want market data exposed with explicit tradability,
so that I can ask "is this price tradable?" not just "what is the LTP?". (FR-19)

## Acceptance Criteria

1. **Given** the tick stream **When** ticks flow or stop **Then** data is exposed as Live/Stale/Disconnected/Delayed/
   Unknown with auto-reconnect (auth-aware), de-dup, and configured REST fallback.
2. **And** price-sensitive entries are blocked while data is not Live.
3. **And** a reconnect that fails on auth routes to session validation, not an endless retry loop.

## Tasks / Subtasks

- [ ] Task 1: `marketdata` module (AC: all)
  - [ ] `include/broker_exec/marketdata/` + `src/marketdata/`; target `broker_exec_marketdata` (+ alias). Depends inward on
        `domain` (Price), `ports` (ClockPort, AlertSink), `errors`. NO IXWebSocket / no new Conan dep — the tradability LOGIC
        is fed by an INJECTED tick/connection seam; the real WS transport is a thin adapter behind it (follow-up note).
- [ ] Task 2: Tradability model (AC: 1, 2)
  - [ ] `enum class MarketDataState { Unknown, Live, Stale, Delayed, Disconnected };` (+ to_string).
  - [ ] `struct Tick { std::string symbol; domain::Price ltp; std::chrono::system_clock::time_point exchange_ts; };`
  - [ ] `class MarketDataView` (ctor `(const ports::ClockPort& clock, std::chrono::milliseconds staleness_threshold,
        std::chrono::milliseconds delay_threshold)`):
    - `void on_tick(const Tick&)`: DE-DUP / out-of-order guard — ignore a tick whose `exchange_ts` is <= the last stored
        exchange_ts for that symbol (a duplicate or older tick never regresses). Otherwise store last ltp + exchange_ts +
        `received_at = clock.now_steady()`.
    - `void on_disconnect() noexcept` / `void on_connect() noexcept`: set the connection flag.
    - `[[nodiscard]] MarketDataState state_for(std::string_view symbol) const`: priority —
        Disconnected (socket down) -> Unknown (no tick ever seen for the symbol) -> Stale (now_steady - received_at >
        staleness_threshold: we stopped receiving) -> Delayed (now_wall - exchange_ts > delay_threshold: feed lagging the
        exchange) -> else Live.
    - `[[nodiscard]] bool is_tradable(std::string_view symbol) const`: true ONLY when state_for == Live (AC-2).
    - `[[nodiscard]] Result<ports::Ok> require_tradable(std::string_view symbol) const`: ok() iff Live; else a typed
        `DataStale` Error (action BlockStrategy) naming the state — the gate's price-sensitive-entry block.
    - `[[nodiscard]] std::optional<domain::Price> ltp(std::string_view symbol) const`: last price for display (regardless of tradability).
- [ ] Task 3: Auth-aware reconnect + REST fallback (AC: 1, 3)
  - [ ] `enum class ReconnectResult { Reconnected, TransportFailure, AuthFailure };`
  - [ ] `[[nodiscard]] Result<ports::Ok> handle_reconnect(ReconnectResult, ports::AlertSink&)`:
        Reconnected -> on_connect(), ok(). TransportFailure -> stay disconnected, ok() (the transport may retry with bounded
        backoff — admission/retry is the transport's, not an endless loop here). AuthFailure -> route to session validation:
        return a `SessionExpired` Error (action ReEstablishSession) + an alert; do NOT signal "retry" (AC-3: an auth-failed
        reconnect must NOT spin an endless retry loop — it goes to session re-establishment).
  - [ ] REST fallback: feeding a REST-fetched price is just another `on_tick(...)` (the configured fallback source is injected
        and calls on_tick when the WS is stale/disconnected). Document the seam; no transport here.
- [ ] Task 4: CMake (orchestrator pre-wires root add_subdirectory(src/marketdata); NO new Conan dep)
  - [ ] `src/marketdata/CMakeLists.txt`: links PUBLIC `broker_exec::domain` `broker_exec::ports` `broker_exec::errors`;
        PRIVATE warnings+sanitizers. Test exe `broker_exec_marketdata_tests` ALSO links `broker_exec::clock` (TestClock).
- [ ] Task 5: Tests (AC: 1, 2, 3) — `src/marketdata/market_data_test.cpp` (TestClock; CountingAlertSink)
  - [ ] never seen -> Unknown, is_tradable false, require_tradable Error (AC-2).
  - [ ] a fresh tick -> Live, is_tradable true, require_tradable ok; ltp returns the price.
  - [ ] advance the TestClock past staleness_threshold (no new tick) -> Stale -> not tradable.
  - [ ] a tick with an exchange_ts older than now_wall by > delay_threshold (but freshly received) -> Delayed -> not tradable.
  - [ ] on_disconnect() -> Disconnected -> not tradable even with a recent tick; on_connect() + a fresh tick -> Live again.
  - [ ] de-dup / out-of-order: a tick with exchange_ts <= the stored one is IGNORED (state + ltp unchanged); a newer one updates.
  - [ ] AC-3 auth-aware: handle_reconnect(AuthFailure) -> SessionExpired/ReEstablishSession Error + alert, and it does NOT
        leave the view "connected"/retrying; handle_reconnect(TransportFailure) -> ok() but still Disconnected (retry is the
        transport's); handle_reconnect(Reconnected) -> connected.
  - [ ] boundary: age == staleness_threshold is still Live (strictly greater is Stale); same for delay.

## Dev Notes

- **Tradability over LTP** — the strategy asks `is_tradable()`/`require_tradable()`, not just `ltp()`; only Live is tradable.
  [architecture.md#FR-19, #IBR-3]
- **Injected seam, no WS dep** — the tradability logic is fed by on_tick/on_connect/on_disconnect + ClockPort; the IXWebSocket
  transport is a thin adapter behind the seam (follow-up, like the cpr transport behind the kite HttpClient seam). [architecture.md#FR-19]
- **Auth-aware reconnect (AC-3)** — distinguish transport vs auth failure; an auth failure routes to session re-establishment
  (SessionExpired/ReEstablishSession), never an endless socket-retry loop. [architecture.md#IBR-3 WS re-auth]
- **De-dup / out-of-order** — a tick older-or-equal by exchange_ts never regresses the view. **REST fallback** = an injected
  source that calls on_tick when the WS is not Live. [architecture.md#FR-19]
- **No float** — Price is integer paise. Staleness via ClockPort.now_steady (monotonic); delay via now_wall vs exchange_ts.
- **Reuse:** `ports::ClockPort`/`AlertSink`, `domain::Price`, `errors` (DataStale/SessionExpired), `clock::TestClock` (tests).

### References
- [Source: epics.md#Story 3.5] [architecture.md#FR-19 market-data states, #IBR-3 WS re-auth] [Source: ports/clock_port.hpp, domain/money.hpp]
- [Source: docs/conventions.md]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
