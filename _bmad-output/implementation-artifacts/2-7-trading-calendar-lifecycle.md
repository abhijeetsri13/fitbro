# Story 2.7: Trading-calendar lifecycle

Status: ready-for-dev

## Story

As an operator,
I want the holiday/special-session calendar sourced and staleness-gated,
so that the bot never trades on a holiday or misses a special session. (FR-4)

## Acceptance Criteria

1. **Given** a configured calendar source **When** the bot starts **Then** the calendar is refreshed/cached;
   a stale/missing calendar blocks trading at safe-start (a typed DataStale Error).
2. **And** entry cut-off and square-off windows are enforced.
3. **And** new holidays take effect only after a refresh.

## Tasks / Subtasks

- [ ] Task 1: Extend the `refdata` module (AC: all)
  - [ ] Add `include/broker_exec/refdata/trading_calendar.hpp` + `src/refdata/trading_calendar.cpp` to the EXISTING
        `broker_exec_refdata` target. Link `nlohmann_json::nlohmann_json` PRIVATE to that target (calendar source is JSON;
        nlohmann is an existing Conan dep — NO new dep). Mirror InstrumentMaster's lifecycle shape (Story 2.6).
- [ ] Task 2: Calendar source + parse (AC: 1, 3)
  - [ ] JSON calendar document: `{ "holidays": ["YYYY-MM-DD", ...], "special_sessions": ["YYYY-MM-DD", ...],
        "windows": { "open":"HH:MM", "entry_cutoff":"HH:MM", "square_off":"HH:MM", "close":"HH:MM" } }`.
        Parse with `nlohmann::json` (allow_exceptions=false; a malformed doc/time -> typed Error). Store holidays as a set,
        special_sessions as a set, and the four windows as minutes-of-day (HH*60+MM) integers — NO float.
- [ ] Task 3: `TradingCalendar` lifecycle (AC: all)
  - [ ] Ctor: `(std::function<Result<std::string>()> fetch_json, const ports::ClockPort& clock, std::filesystem::path cache_dir,
        std::string broker, int tz_offset_minutes = 330 /* IST = UTC+5:30 */)`. The offset converts now_wall() (UTC) to the
        exchange-local date + minute-of-day deterministically (no localtime/#ifdef).
  - [ ] `Result<Ok> refresh()`: fetch_json() (propagate Error -> safe-start blocks); parse; commit fresh state ONLY AFTER the
        date-versioned cache file `<cache_dir>/<broker>_calendar_<YYYY-MM-DD>.csv|json` is written (mirror Story 2.6's
        no-fresh-on-persist-fail ordering); stamp cache_date_ = today (exchange-local).
  - [ ] `Result<Ok> load_cached_for(iso_date)`; `Result<Ok> require_fresh() const` (DataStale + BlockStrategy if never
        loaded or cache_date_ != today-local); `bool is_fresh() const`.
  - [ ] `bool is_trading_day(iso_date) const`: false on Saturday/Sunday UNLESS in special_sessions; false if in holidays
        (a holiday in special_sessions = a working-day special session -> trading day). Weekday via std::chrono.
  - [ ] Window predicates evaluated at the current local time (from clock):
        `bool in_entry_window() const` (today is a trading day AND open <= minute_of_day < entry_cutoff),
        `bool past_square_off() const` (minute_of_day >= square_off),
        `bool in_session() const` (open <= minute_of_day < close on a trading day).
        Provide a `Result<Ok> require_entry_allowed() const` returning a typed Error (MarketClosed) when entry is not
        permitted (not a trading day, before open, or at/after entry_cutoff) — naming the reason.
- [ ] Task 4: CMake — extend `src/refdata/CMakeLists.txt`
  - [ ] Add `trading_calendar.cpp` to `broker_exec_refdata`; link `nlohmann_json::nlohmann_json` PRIVATE. Add
        `trading_calendar_test.cpp` to the existing `broker_exec_refdata_tests` exe (it already links clock).
- [ ] Task 5: Tests (AC: 1, 2, 3) — `src/refdata/trading_calendar_test.cpp`
  - [ ] Fake fetch_json returning a known calendar (a holiday, a Sunday special session, the four windows) + TestClock pinned
        to chosen UTC instants; assert the IST conversion lands on the intended local date/time.
  - [ ] refresh() then: is_trading_day(a normal weekday)==true; is_trading_day(a configured holiday)==false;
        is_trading_day(a Sunday in special_sessions)==true; is_trading_day(a plain Saturday)==false.
  - [ ] entry window: clock at 09:20 IST (open 09:15, entry_cutoff 15:00) -> in_entry_window()==true, require_entry_allowed ok;
        clock at 15:10 IST -> in_entry_window()==false AND past_square_off()==true (square_off 15:20? pick consistent values);
        clock before open -> require_entry_allowed -> MarketClosed Error.
  - [ ] require_fresh(): DataStale before refresh, ok same-day, fails after advancing the clock to the next local day.
  - [ ] new holiday: a date NOT a holiday in calendar#1 -> is_trading_day true; swap fetcher to calendar#2 adding that
        holiday, refresh() -> is_trading_day now false (AC-3).
  - [ ] date-versioned cache written; load_cached_for(today) re-loads without fetch; fetch failure -> refresh Error + require_fresh stays failing.

## Dev Notes

- **Date/time via ClockPort + tz offset** (UTC -> exchange-local), std::chrono only (no localtime/strftime/#ifdef). This also
  resolves the UTC-vs-IST day-boundary note from Story 2.6 for the calendar's purposes. [docs/conventions.md, FR-23]
- **Staleness = cache-date vs trading-date**; stale/missing -> DataStale + BlockStrategy (safe-start blocks). [architecture.md#DA-4]
- **NO float**: window times are integer minutes-of-day. [docs/conventions.md#Money — and same no-float discipline]
- **Errors:** MarketClosed for outside-window entry; DataStale for staleness; Validation for a malformed calendar.
- **Reuse:** `ports::ClockPort`, `clock::TestClock` (tests), `errors` taxonomy, the Story-2.6 date/cache idioms.

### References
- [Source: epics.md#Story 2.7] [architecture.md#DA-4 reference-data cache, #Source tree refdata/calendar]
- [Source: docs/conventions.md] [Source: src/refdata/instrument_master.cpp (lifecycle pattern to mirror)]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
