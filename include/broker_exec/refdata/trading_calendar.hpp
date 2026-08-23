#pragma once

// broker_exec::refdata — the trading-calendar lifecycle (Story 2.7, FR-4).
//
// The per-broker trading calendar (holidays, working-day special sessions, and
// the open/entry-cutoff/square-off/close session windows) is the authority for
// "may the bot trade right now?". This module:
//   * parses a JSON calendar document (holidays, special_sessions, windows),
//   * refreshes + date-versioned-caches it once per trading day,
//   * answers is_trading_day / in_session / window predicates at the current
//     exchange-local time, and
//   * gates safe-start on freshness (a stale/undownloaded calendar blocks).
//
// Hexagonal boundary: refdata depends INWARD only on `ports` (ClockPort),
// `errors` and `result` (plus nlohmann_json PRIVATELY in the .cpp). The JSON is
// supplied through an injected `fetch_json` seam (the composition root wires the
// concrete calendar source), so this module stays transport-free and testable —
// no broker SDK/adapter dependency.
//
// Date/time: the injected ClockPort yields UTC wall time; a fixed
// `tz_offset_minutes` (IST = UTC+5:30 = 330) converts it to the exchange-local
// date + minute-of-day deterministically via std::chrono — NO localtime/strftime,
// no `#ifdef`. NO double/float: window times are integer minutes-of-day.

#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <string_view>

#include "broker_exec/ports/clock_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::refdata {

// The parsed, in-memory calendar. Holidays and special sessions are ISO
// YYYY-MM-DD date strings (lexicographic == chronological); the four windows are
// integer minutes-of-day (HH*60+MM, range 0..1439). NO float.
struct CalendarData {
  std::set<std::string> holidays;
  std::set<std::string> special_sessions;
  int open_min = 0;          // session open
  int entry_cutoff_min = 0;  // last entry allowed strictly before this
  int square_off_min = 0;    // forced square-off begins at/after this
  int close_min = 0;         // session close
};

// Parse a JSON calendar document of the shape:
//   { "holidays": ["YYYY-MM-DD", ...],
//     "special_sessions": ["YYYY-MM-DD", ...],
//     "windows": { "open":"HH:MM", "entry_cutoff":"HH:MM",
//                  "square_off":"HH:MM", "close":"HH:MM" } }
// Parsing uses nlohmann::json with allow_exceptions=false (no-throw). A malformed
// document, a missing "windows" key (or a missing window field), or a bad "HH:MM"
// (not 00-23:00-59) yields a typed Validation Error. holidays/special_sessions
// may be omitted (treated as empty). No double/float — windows become minutes.
[[nodiscard]] Result<CalendarData> parse_calendar_json(std::string_view doc);

// Owns the lifecycle of one broker's trading calendar: refresh from the injected
// source, date-versioned caching, the trading-day/session/window predicates, and
// the safe-start freshness gate. Non-copyable (holds a ClockPort reference).
class TradingCalendar {
 public:
  // `fetch_json` returns the raw calendar JSON (or an Error to propagate);
  // `clock` supplies UTC wall time; `cache_dir` is where date-versioned JSON is
  // written; `broker` forms the cache filename prefix; `tz_offset_minutes` turns
  // UTC into the exchange-local date + minute-of-day (IST = UTC+5:30 = 330).
  TradingCalendar(std::function<Result<std::string>()> fetch_json, const ports::ClockPort& clock,
                  std::filesystem::path cache_dir, std::string broker, int tz_offset_minutes = 330);

  TradingCalendar(const TradingCalendar&) = delete;
  TradingCalendar& operator=(const TradingCalendar&) = delete;
  TradingCalendar(TradingCalendar&&) = delete;
  TradingCalendar& operator=(TradingCalendar&&) = delete;
  ~TradingCalendar() = default;

  // Fetch + parse + stamp today's exchange-local date + persist the raw JSON
  // date-versioned to `<cache_dir>/<broker>_calendar_<YYYY-MM-DD>.json`. A fetch
  // or parse failure propagates as an Error (the safe-start gate then blocks). On
  // any write failure the prior state is left untouched. On success the in-memory
  // calendar is replaced atomically (only AFTER the cache file is written).
  [[nodiscard]] Result<ports::Ok> refresh();

  // Boot path: if a date-versioned cache file for `iso_date` exists, read+parse
  // it and stamp cache_date_ = iso_date (no fetch). Otherwise a DataStale Error.
  [[nodiscard]] Result<ports::Ok> load_cached_for(const std::string& iso_date);

  // The safe-start staleness gate (AC-1): ok() iff a refresh/load has succeeded
  // AND the cached date is today (exchange-local). Otherwise a DataStale Error
  // with SuggestedAction::BlockStrategy (start halts; the operator is alerted).
  [[nodiscard]] Result<ports::Ok> require_fresh() const;

  // Convenience predicate over require_fresh().
  [[nodiscard]] bool is_fresh() const;

  // Is `iso_date` a trading day? A weekend (Sat/Sun) is NOT a trading day unless
  // the date is a working-day special session; a date in holidays is NOT a
  // trading day. A special session always wins (a holiday that is also a special
  // session, or a weekend special session, IS a trading day). A normal weekday
  // not in holidays is a trading day. Weekday is derived via std::chrono.
  [[nodiscard]] bool is_trading_day(const std::string& iso_date) const;

  // Current-time predicates evaluated at the exchange-local clock (AC-2):
  //   in_entry_window(): today is a trading day AND open <= now_min < entry_cutoff
  //   past_square_off(): now_min >= square_off
  //   in_session():      today is a trading day AND open <= now_min < close
  [[nodiscard]] bool in_entry_window() const;
  [[nodiscard]] bool past_square_off() const;
  [[nodiscard]] bool in_session() const;

  // ok() iff in_entry_window(); else a typed MarketClosed Error naming the reason
  // (not-a-trading-day / before-open / at-or-after-entry-cutoff). No secrets.
  [[nodiscard]] Result<ports::Ok> require_entry_allowed() const;

  // The date currently loaded (ISO YYYY-MM-DD), empty if never loaded.
  [[nodiscard]] const std::string& cache_date() const noexcept { return cache_date_; }

 private:
  // The exchange-local "now": ISO YYYY-MM-DD date + minute-of-day (0..1439),
  // derived from clock_.now_wall() (UTC) + tz_offset_minutes via std::chrono.
  struct LocalNow {
    std::string iso_date;
    int minute_of_day = 0;
  };
  [[nodiscard]] LocalNow local_now() const;

  // Today's exchange-local ISO date (the freshness/cache stamp).
  [[nodiscard]] std::string today_local() const;

  // The date-versioned cache file path for a given ISO date.
  [[nodiscard]] std::filesystem::path cache_file_for(const std::string& iso_date) const;

  // Parse `doc` and, on success, replace the calendar and stamp cache_date_.
  [[nodiscard]] Result<ports::Ok> ingest(std::string_view doc, std::string iso_date);

  std::function<Result<std::string>()> fetch_json_;
  const ports::ClockPort& clock_;
  std::filesystem::path cache_dir_;
  std::string broker_;
  int tz_offset_minutes_;
  CalendarData calendar_;
  std::string cache_date_;
  bool loaded_ = false;
};

}  // namespace broker_exec::refdata
