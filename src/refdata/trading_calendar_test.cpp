#include "broker_exec/refdata/trading_calendar.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <system_error>

#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/result.hpp"

using broker_exec::Result;
using broker_exec::clock::TestClock;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::SuggestedAction;
using broker_exec::ports::Ok;
using broker_exec::refdata::CalendarData;
using broker_exec::refdata::parse_calendar_json;
using broker_exec::refdata::TradingCalendar;

namespace {

// A unique temp directory, cleaned up on destruction (RAII).
struct TempDir {
  std::filesystem::path path;
  TempDir() {
    std::random_device rd;
    path = std::filesystem::temp_directory_path() /
           ("brexec_calendar_" + std::to_string(rd()) + "_" + std::to_string(rd()));
    std::filesystem::create_directories(path);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

// Build a UTC wall instant for a UTC calendar date + time-of-day (deterministic).
// IST (the exchange-local zone the calendar uses) = this UTC + 5:30.
[[nodiscard]] std::chrono::system_clock::time_point utc_at(int year, unsigned month, unsigned day,
                                                           int hour, int minute) {
  const std::chrono::sys_days d{std::chrono::year{year} / std::chrono::month{month} /
                                std::chrono::day{day}};
  return std::chrono::system_clock::time_point(d) + std::chrono::hours(hour) +
         std::chrono::minutes(minute);
}

[[nodiscard]] TestClock clock_utc(int year, unsigned month, unsigned day, int hour, int minute) {
  return TestClock(std::chrono::steady_clock::time_point{}, utc_at(year, month, day, hour, minute));
}

// Calendar #1: holiday 2026-06-30 (a Tuesday), special session 2026-06-28 (a
// Sunday), windows open 09:15 / entry_cutoff 15:00 / square_off 15:20 / close
// 15:30. Reference weekdays (2026-01-01 is a Thursday):
//   2026-06-27 Saturday, 2026-06-28 Sunday, 2026-06-29 Monday, 2026-06-30 Tuesday.
constexpr const char* kCal1 =
    R"({"holidays":["2026-06-30"],"special_sessions":["2026-06-28"],)"
    R"("windows":{"open":"09:15","entry_cutoff":"15:00","square_off":"15:20","close":"15:30"}})";

[[nodiscard]] std::function<Result<std::string>()> static_fetcher(std::string doc) {
  return [doc = std::move(doc)]() -> Result<std::string> { return doc; };
}

}  // namespace

TEST_CASE("parse_calendar_json stores windows as integer minutes-of-day", "[refdata][calendar]") {
  const Result<CalendarData> r = parse_calendar_json(kCal1);
  REQUIRE(r.has_value());
  CHECK(r.value().open_min == 9 * 60 + 15);
  CHECK(r.value().entry_cutoff_min == 15 * 60);
  CHECK(r.value().square_off_min == 15 * 60 + 20);
  CHECK(r.value().close_min == 15 * 60 + 30);
  CHECK(r.value().holidays.contains("2026-06-30"));
  CHECK(r.value().special_sessions.contains("2026-06-28"));
}

TEST_CASE("a malformed calendar / missing windows / bad HH:MM is a Validation Error",
          "[refdata][calendar]") {
  CHECK_FALSE(parse_calendar_json("{ not json").has_value());
  CHECK(parse_calendar_json("{ not json").error().category == ErrorCategory::Validation);

  // Missing windows.
  const Result<CalendarData> no_win = parse_calendar_json(R"({"holidays":[]})");
  REQUIRE_FALSE(no_win.has_value());
  CHECK(no_win.error().category == ErrorCategory::Validation);

  // Out-of-range minute (25:00).
  const Result<CalendarData> bad_time = parse_calendar_json(
      R"({"windows":{"open":"25:00","entry_cutoff":"15:00","square_off":"15:20","close":"15:30"}})");
  REQUIRE_FALSE(bad_time.has_value());
  CHECK(bad_time.error().category == ErrorCategory::Validation);
}

TEST_CASE("is_trading_day: weekday true, holiday false, Sunday-special true, Saturday false",
          "[refdata][calendar][AC1]") {
  TempDir dir;
  TestClock clock = clock_utc(2026, 6, 29, 3, 50);  // IST 2026-06-29 09:20 (Monday)
  TradingCalendar cal(static_fetcher(kCal1), clock, dir.path, "kite");
  REQUIRE(cal.refresh().has_value());

  CHECK(cal.is_trading_day("2026-06-29"));        // Monday, not a holiday
  CHECK_FALSE(cal.is_trading_day("2026-06-30"));  // Tuesday but a holiday
  CHECK(cal.is_trading_day("2026-06-28"));        // Sunday, but a special session
  CHECK_FALSE(cal.is_trading_day("2026-06-27"));  // plain Saturday
}

TEST_CASE("IST conversion + entry window land on the intended local time",
          "[refdata][calendar][AC2]") {
  TempDir dir;

  SECTION("09:20 IST on a trading day -> in entry window, entry allowed") {
    TestClock clock = clock_utc(2026, 6, 29, 3, 50);  // IST 09:20 Monday
    TradingCalendar cal(static_fetcher(kCal1), clock, dir.path, "kite");
    REQUIRE(cal.refresh().has_value());
    CHECK(cal.in_entry_window());
    CHECK_FALSE(cal.past_square_off());
    CHECK(cal.in_session());
    CHECK(cal.require_entry_allowed().has_value());
  }

  SECTION("15:25 IST -> past square-off, entry no longer allowed but still in session") {
    TestClock clock = clock_utc(2026, 6, 29, 9, 55);  // IST 15:25 Monday
    TradingCalendar cal(static_fetcher(kCal1), clock, dir.path, "kite");
    REQUIRE(cal.refresh().has_value());
    CHECK_FALSE(cal.in_entry_window());  // 15:25 >= entry_cutoff 15:00
    CHECK(cal.past_square_off());        // 15:25 >= square_off 15:20
    CHECK(cal.in_session());             // 15:25 < close 15:30
    const Result<Ok> entry = cal.require_entry_allowed();
    REQUIRE_FALSE(entry.has_value());
    CHECK(entry.error().category == ErrorCategory::MarketClosed);
  }

  SECTION("08:00 IST (before open) -> require_entry_allowed is MarketClosed") {
    TestClock clock = clock_utc(2026, 6, 29, 2, 30);  // IST 08:00 Monday
    TradingCalendar cal(static_fetcher(kCal1), clock, dir.path, "kite");
    REQUIRE(cal.refresh().has_value());
    CHECK_FALSE(cal.in_entry_window());
    const Result<Ok> entry = cal.require_entry_allowed();
    REQUIRE_FALSE(entry.has_value());
    CHECK(entry.error().category == ErrorCategory::MarketClosed);
    CHECK(entry.error().action == SuggestedAction::BlockStrategy);
  }
}

TEST_CASE("half-open window edges are exact at open / entry_cutoff / square_off / close",
          "[refdata][calendar][AC2]") {
  TempDir dir;
  // 2026-06-29 is a Monday (a trading day). The fake calendar windows are
  // open 09:15 / entry_cutoff 15:00 / square_off 15:20 / close 15:30 IST.
  // IST = UTC + 5:30, so each UTC instant below is the IST minute minus 5:30.

  SECTION("at exactly OPEN (09:15 IST) -> in entry window, entry allowed, in session") {
    TestClock clock = clock_utc(2026, 6, 29, 3, 45);  // IST 09:15 (open is inclusive)
    TradingCalendar cal(static_fetcher(kCal1), clock, dir.path, "kite");
    REQUIRE(cal.refresh().has_value());
    CHECK(cal.in_entry_window());
    CHECK(cal.require_entry_allowed().has_value());
    CHECK(cal.in_session());
  }

  SECTION("at exactly ENTRY_CUTOFF (15:00 IST) -> entry window closed, MarketClosed (exclusive)") {
    TestClock clock = clock_utc(2026, 6, 29, 9, 30);  // IST 15:00 (cutoff is exclusive)
    TradingCalendar cal(static_fetcher(kCal1), clock, dir.path, "kite");
    REQUIRE(cal.refresh().has_value());
    CHECK_FALSE(cal.in_entry_window());
    const Result<Ok> entry = cal.require_entry_allowed();
    REQUIRE_FALSE(entry.has_value());
    CHECK(entry.error().category == ErrorCategory::MarketClosed);
  }

  SECTION("at exactly SQUARE_OFF (15:20 IST) -> past square-off") {
    TestClock clock = clock_utc(2026, 6, 29, 9, 50);  // IST 15:20 (square_off is inclusive)
    TradingCalendar cal(static_fetcher(kCal1), clock, dir.path, "kite");
    REQUIRE(cal.refresh().has_value());
    CHECK(cal.past_square_off());
  }

  SECTION("at exactly CLOSE (15:30 IST) -> not in session (close is exclusive)") {
    TestClock clock = clock_utc(2026, 6, 29, 10, 0);  // IST 15:30 (close is exclusive)
    TradingCalendar cal(static_fetcher(kCal1), clock, dir.path, "kite");
    REQUIRE(cal.refresh().has_value());
    CHECK_FALSE(cal.in_session());
  }
}

TEST_CASE("require_entry_allowed on a holiday is MarketClosed (not a trading day)",
          "[refdata][calendar][AC2]") {
  TempDir dir;
  TestClock clock = clock_utc(2026, 6, 30, 3, 50);  // IST 09:20 on the 2026-06-30 holiday
  TradingCalendar cal(static_fetcher(kCal1), clock, dir.path, "kite");
  REQUIRE(cal.refresh().has_value());

  CHECK_FALSE(cal.in_entry_window());
  const Result<Ok> entry = cal.require_entry_allowed();
  REQUIRE_FALSE(entry.has_value());
  CHECK(entry.error().category == ErrorCategory::MarketClosed);
}

TEST_CASE("require_fresh: stale before, ok same-day, fails on the next local day",
          "[refdata][calendar][AC1]") {
  TempDir dir;
  TestClock clock = clock_utc(2026, 6, 29, 3, 50);  // IST 2026-06-29
  TradingCalendar cal(static_fetcher(kCal1), clock, dir.path, "kite");

  // Before any refresh: stale (DataStale + BlockStrategy).
  const Result<Ok> before = cal.require_fresh();
  REQUIRE_FALSE(before.has_value());
  CHECK(before.error().category == ErrorCategory::DataStale);
  CHECK(before.error().action == SuggestedAction::BlockStrategy);
  CHECK_FALSE(cal.is_fresh());

  // Same-day refresh -> fresh.
  REQUIRE(cal.refresh().has_value());
  CHECK(cal.require_fresh().has_value());
  CHECK(cal.is_fresh());
  CHECK(cal.cache_date() == "2026-06-29");

  // Advance the wall clock to the next IST day -> stale again.
  clock.set_wall(utc_at(2026, 6, 30, 3, 50));  // IST 2026-06-30
  const Result<Ok> next_day = cal.require_fresh();
  REQUIRE_FALSE(next_day.has_value());
  CHECK(next_day.error().category == ErrorCategory::DataStale);
  CHECK(next_day.error().action == SuggestedAction::BlockStrategy);
}

TEST_CASE("IST conversion crosses the UTC day boundary (+5:30 rollover)",
          "[refdata][calendar][AC1]") {
  TempDir dir;
  // UTC 2026-06-28 20:00 is IST 2026-06-29 01:30 — the local date rolls forward.
  TestClock clock = clock_utc(2026, 6, 28, 20, 0);
  TradingCalendar cal(static_fetcher(kCal1), clock, dir.path, "kite");
  REQUIRE(cal.refresh().has_value());

  CHECK(cal.cache_date() == "2026-06-29");
  CHECK(std::filesystem::exists(dir.path / "kite_calendar_2026-06-29.json"));
}

TEST_CASE("a new holiday takes effect only after a refresh (AC-3)", "[refdata][calendar][AC3]") {
  TempDir dir;
  TestClock clock = clock_utc(2026, 6, 29, 3, 50);

  // Calendar #1 has NO holiday on 2026-07-01 (a Wednesday) -> it is a trading day.
  const std::string cal_v1 =
      R"({"holidays":[],"special_sessions":[],)"
      R"("windows":{"open":"09:15","entry_cutoff":"15:00","square_off":"15:20","close":"15:30"}})";
  // Calendar #2 adds 2026-07-01 as a holiday.
  const std::string cal_v2 =
      R"({"holidays":["2026-07-01"],"special_sessions":[],)"
      R"("windows":{"open":"09:15","entry_cutoff":"15:00","square_off":"15:20","close":"15:30"}})";

  std::string current = cal_v1;
  auto fetcher = [&current]() -> Result<std::string> { return current; };
  TradingCalendar cal(fetcher, clock, dir.path, "kite");

  REQUIRE(cal.refresh().has_value());
  CHECK(cal.is_trading_day("2026-07-01"));  // not yet a holiday

  current = cal_v2;
  REQUIRE(cal.refresh().has_value());
  CHECK_FALSE(cal.is_trading_day("2026-07-01"));  // holiday takes effect after refresh
}

TEST_CASE("date-versioned cache is written and load_cached_for re-loads without a fetch",
          "[refdata][calendar][AC1]") {
  TempDir dir;
  TestClock clock = clock_utc(2026, 6, 29, 3, 50);  // IST 2026-06-29

  // First calendar writes the date-versioned cache.
  {
    TradingCalendar writer(static_fetcher(kCal1), clock, dir.path, "kite");
    REQUIRE(writer.refresh().has_value());
  }
  CHECK(std::filesystem::exists(dir.path / "kite_calendar_2026-06-29.json"));

  // A fresh calendar loads today's cache WITHOUT calling the fetcher.
  int fetch_calls = 0;
  auto counting_fetcher = [&fetch_calls]() -> Result<std::string> {
    ++fetch_calls;
    return std::string{};
  };
  TradingCalendar reader(counting_fetcher, clock, dir.path, "kite");

  REQUIRE(reader.load_cached_for("2026-06-29").has_value());
  CHECK(fetch_calls == 0);
  CHECK(reader.is_fresh());
  CHECK(reader.is_trading_day("2026-06-29"));
  CHECK_FALSE(reader.is_trading_day("2026-06-30"));  // holiday survives the round-trip

  // No cache for a different date -> DataStale.
  const Result<Ok> missing = reader.load_cached_for("2026-06-20");
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().category == ErrorCategory::DataStale);
}

TEST_CASE("a fetch failure leaves refresh failing and safe-start blocked",
          "[refdata][calendar][AC1]") {
  TempDir dir;
  TestClock clock = clock_utc(2026, 6, 29, 3, 50);
  auto failing_fetcher = []() -> Result<std::string> {
    return broker_exec::fail(
        broker_exec::errors::make_error(ErrorCategory::Network, "download failed"));
  };
  TradingCalendar cal(failing_fetcher, clock, dir.path, "kite");

  const Result<Ok> refreshed = cal.refresh();
  REQUIRE_FALSE(refreshed.has_value());
  CHECK(refreshed.error().category == ErrorCategory::Network);  // propagated verbatim

  // The staleness gate still blocks (no successful load happened).
  const Result<Ok> gate = cal.require_fresh();
  REQUIRE_FALSE(gate.has_value());
  CHECK(gate.error().category == ErrorCategory::DataStale);
  CHECK(gate.error().action == SuggestedAction::BlockStrategy);
}

TEST_CASE("a malformed calendar JSON makes refresh fail and leaves state untouched",
          "[refdata][calendar][AC1]") {
  TempDir dir;
  TestClock clock = clock_utc(2026, 6, 29, 3, 50);
  TradingCalendar cal(static_fetcher("{ this is not valid json"), clock, dir.path, "kite");

  const Result<Ok> refreshed = cal.refresh();
  REQUIRE_FALSE(refreshed.has_value());
  CHECK(refreshed.error().category == ErrorCategory::Validation);
  CHECK_FALSE(cal.is_fresh());
}
