# Story 2.6: Instrument-master lifecycle

Status: ready-for-dev

## Story

As an operator,
I want the instrument master refreshed and staleness-gated daily,
so that orders never use stale lot/tick/freeze/expiry or a wrong token. (FR-3)

## Acceptance Criteria

1. **Given** the per-broker instrument master **When** a new trading day begins **Then** it is downloaded,
   cached (date-versioned), and a symbol resolves to current token + lot/tick/freeze/expiry.
2. **And** a stale or failed-to-download master blocks trading at safe-start with an alert (a typed DataStale Error).
3. **And** an expired/unknown instrument is rejected at the gate; a new strike is tradable only after refresh.

## Tasks / Subtasks

- [ ] Task 1: `refdata` module (AC: all)
  - [ ] `include/broker_exec/refdata/` + `src/refdata/`; target `broker_exec_refdata` (+ alias). Depends inward only on
        `domain`, `ports` (ClockPort), `errors`. NO transport dep — the CSV is supplied through an injected seam so the
        module is decoupled and testable (composition root wires Kite's `instruments()`).
- [ ] Task 2: Kite instruments CSV parser (AC: 1, 3)
  - [ ] Parse the Kite instruments dump (header row then rows). Map columns BY HEADER NAME (order-robust):
        `instrument_token, tradingsymbol, name, expiry, tick_size, lot_size, instrument_type, segment, exchange`
        (Kite's real columns include also exchange_token,last_price,strike). Produce `domain::Instrument`
        (symbol, token, exchange, lot_size, tick_size, freeze_qty, expiry). freeze_qty is NOT in the CSV — default 0
        here (the freeze slicer, Story 2.9, supplies exchange freeze ceilings); a comment must say so.
  - [ ] Robust parsing: tolerate quoted fields/commas-in-quotes minimally, skip blank lines, a malformed row -> a typed
        Error naming the row (don't silently drop a bad master). Money/price fields parse to the integer Price/Quantity
        types (NO double/float) — convert the decimal tick/strike text to paise/integer via the domain helpers.
- [ ] Task 3: `InstrumentMaster` lifecycle (AC: 1, 2, 3)
  - [ ] Ctor: `(std::function<Result<std::string>()> fetch_csv, const ports::ClockPort& clock, std::filesystem::path cache_dir,
        std::string broker, std::string segment)`.
  - [ ] `Result<Ok> refresh()`: call fetch_csv(); on failure propagate (the safe-start gate blocks). Parse; build an index
        keyed by (exchange, tradingsymbol) -> Instrument; stamp `cache_date_ = today (clock.now_wall() -> date)`; persist
        the raw CSV date-versioned to `<cache_dir>/<broker>_<segment>_<YYYY-MM-DD>.csv` (date-versioned per architecture DA-4).
  - [ ] `Result<Ok> load_cached_for(date)`: if a date-versioned cache file for `date` exists, load+parse it and set
        cache_date_ = date (boot path: use today's cache if present, else refresh()).
  - [ ] `Result<domain::Instrument> resolve(exchange, tradingsymbol) const`: unknown symbol -> Validation Error naming it;
        an instrument whose `expiry` is non-empty and < today -> a Validation/DataStale Error (expired, rejected at gate);
        else the Instrument.
  - [ ] `Result<Ok> require_fresh() const`: DataStale Error (SuggestedAction::BlockStrategy) if `cache_date_ != today`
        OR no successful refresh yet — the safe-start staleness gate (AC-2). `bool is_fresh() const` convenience.
- [ ] Task 4: CMake (orchestrator pre-wires root add_subdirectory(src/refdata); NO new Conan dep)
  - [ ] `src/refdata/CMakeLists.txt`: links PUBLIC `broker_exec::domain` `broker_exec::ports` `broker_exec::errors`;
        PRIVATE warnings+sanitizers; test exe `broker_exec_refdata_tests` (also link `broker_exec::clock` for TestClock).
- [ ] Task 5: Tests (AC: 1, 2, 3) — `src/refdata/instrument_master_test.cpp`
  - [ ] Inject a fake fetch_csv returning a known small Kite-shaped CSV + a `clock::TestClock` pinned to a fixed wall date.
  - [ ] refresh() then resolve(NFO, known symbol) -> correct token + lot_size + tick_size + expiry.
  - [ ] resolve(unknown) -> Error; resolve(an expired-expiry row, clock date after expiry) -> Error (rejected).
  - [ ] require_fresh(): fails (DataStale) before any refresh AND when cache_date is a prior day (advance the TestClock a
        day after refresh -> require_fresh now fails); passes right after a same-day refresh.
  - [ ] new strike: a symbol absent from the first CSV -> resolve Error; after a refresh() whose CSV includes it -> resolves (AC-3).
  - [ ] a date-versioned cache file is written; load_cached_for(today) re-loads it without a fetch.
  - [ ] a fetch failure -> refresh() returns Error and require_fresh() stays failing (blocks safe-start).

## Dev Notes

- **Source:** Kite `/instruments` CSV via `KiteRestClient::instruments()` (Story 2.3) — injected as `fetch_csv` so refdata
  stays transport-free + testable. [architecture.md#DA-4 reference-data cache, #Source tree refdata/]
- **Date-versioned cache + staleness = cache-date vs trading-date** at safe-start. [architecture.md#DA-4, #COH-2]
- **No double/float:** tick_size/strike decimals convert to the integer Price/Quantity domain types. [docs/conventions.md#Money]
- **Time via ClockPort** (now_wall -> date); never call system_clock directly. [docs/conventions.md#Cross-platform, FR-23]
- **Errors:** unknown/expired -> Validation; stale/undownloaded -> DataStale + BlockStrategy (safe-start blocks). Reuse errors taxonomy.
- **Reuse:** `domain::Instrument`/`Price`/`Quantity` (Story 1.2), `ports::ClockPort`, `clock::TestClock` (tests).

### References
- [Source: epics.md#Story 2.6] [architecture.md#DA-4, #COH-2, #Source tree]
- [Source: docs/conventions.md] [Source: include/broker_exec/domain/types.hpp, ports/clock_port.hpp]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
