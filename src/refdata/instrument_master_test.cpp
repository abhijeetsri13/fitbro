#include "broker_exec/refdata/instrument_master.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/result.hpp"

using broker_exec::Result;
using broker_exec::clock::TestClock;
using broker_exec::domain::Instrument;
using broker_exec::domain::Price;
using broker_exec::domain::Quantity;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::SuggestedAction;
using broker_exec::refdata::InstrumentMaster;

namespace {

// A unique temp directory, cleaned up on destruction (RAII).
struct TempDir {
  std::filesystem::path path;
  TempDir() {
    std::random_device rd;
    path = std::filesystem::temp_directory_path() /
           ("brexec_refdata_" + std::to_string(rd()) + "_" + std::to_string(rd()));
    std::filesystem::create_directories(path);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

// Build a wall-clock instant for a UTC calendar date (deterministic).
[[nodiscard]] std::chrono::system_clock::time_point wall_on(int year, unsigned month,
                                                            unsigned day) {
  return std::chrono::system_clock::time_point(std::chrono::sys_days{
      std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day}});
}

[[nodiscard]] TestClock clock_at(int year, unsigned month, unsigned day) {
  return TestClock(std::chrono::steady_clock::time_point{}, wall_on(year, month, day));
}

// A Kite-shaped instruments CSV. Header order is non-trivial and carries extra
// columns (exchange_token, last_price, strike) to exercise header-name mapping.
// Includes a blank line and a quoted name field containing a comma.
//   known, fresh: NIFTY26JUL24000CE  expiry 2026-07-30  tick 0.05  lot 75  token 256265
//   expired:      NIFTY26JUN24000CE  expiry 2026-06-26  tick 0.05  lot 75
//   quoted name:  TESTCOMMA          expiry 2026-12-31  tick 0.10  lot 50
constexpr const char* kCsvV1 =
    "instrument_token,exchange_token,tradingsymbol,name,last_price,expiry,strike,tick_size,"
    "lot_size,instrument_type,segment,exchange\n"
    "256265,1001,NIFTY26JUL24000CE,\"NIFTY\",0,2026-07-30,24000,0.05,75,CE,NFO-OPT,NFO\n"
    "111111,1002,NIFTY26JUN24000CE,NIFTY,0,2026-06-26,24000,0.05,75,CE,NFO-OPT,NFO\n"
    "\n"
    "222222,1003,TESTCOMMA,\"NIFTY, fifty\",0,2026-12-31,0,0.10,50,CE,NFO-OPT,NFO\n";

// V2 adds a brand-new strike absent from V1 (the AC-3 "new strike" case).
constexpr const char* kCsvV2 =
    "instrument_token,exchange_token,tradingsymbol,name,last_price,expiry,strike,tick_size,"
    "lot_size,instrument_type,segment,exchange\n"
    "256265,1001,NIFTY26JUL24000CE,\"NIFTY\",0,2026-07-30,24000,0.05,75,CE,NFO-OPT,NFO\n"
    "256266,1004,NIFTY26JUL24500CE,NIFTY,0,2026-07-30,24500,0.05,75,CE,NFO-OPT,NFO\n";

[[nodiscard]] std::function<Result<std::string>()> static_fetcher(std::string csv) {
  return [csv = std::move(csv)]() -> Result<std::string> { return csv; };
}

}  // namespace

TEST_CASE("refresh then resolve returns current token/lot/tick/expiry", "[refdata][AC1]") {
  TempDir dir;
  TestClock clock = clock_at(2026, 6, 27);
  InstrumentMaster master(static_fetcher(kCsvV1), clock, dir.path, "kite", "NFO");

  REQUIRE(master.refresh().has_value());

  const Result<Instrument> r = master.resolve("NFO", "NIFTY26JUL24000CE");
  REQUIRE(r.has_value());
  CHECK(r.value().token == 256265);
  CHECK(r.value().lot_size == Quantity::of(75));
  CHECK(r.value().tick_size == Price::from_paise(5));  // "0.05" -> 5 paise, no float
  CHECK(r.value().tick_size.paise() == 5);
  CHECK(r.value().exchange == "NFO");
  CHECK(r.value().expiry == "2026-07-30");
  // freeze_qty is not in the CSV; defaults to 0 (slicer Story 2.9 supplies it).
  CHECK(r.value().freeze_qty == Quantity::of(0));
}

TEST_CASE("quoted field with embedded comma parses correctly", "[refdata]") {
  TempDir dir;
  TestClock clock = clock_at(2026, 6, 27);
  InstrumentMaster master(static_fetcher(kCsvV1), clock, dir.path, "kite", "NFO");
  REQUIRE(master.refresh().has_value());

  const Result<Instrument> r = master.resolve("NFO", "TESTCOMMA");
  REQUIRE(r.has_value());
  CHECK(r.value().token == 222222);
  CHECK(r.value().tick_size.paise() == 10);  // "0.10" -> 10 paise
  CHECK(r.value().lot_size == Quantity::of(50));
}

TEST_CASE("resolve of an unknown symbol is a Validation Error", "[refdata][AC3]") {
  TempDir dir;
  TestClock clock = clock_at(2026, 6, 27);
  InstrumentMaster master(static_fetcher(kCsvV1), clock, dir.path, "kite", "NFO");
  REQUIRE(master.refresh().has_value());

  const Result<Instrument> r = master.resolve("NFO", "NONEXISTENT");
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().category == ErrorCategory::Validation);
}

TEST_CASE("an expired instrument is rejected at the gate", "[refdata][AC3]") {
  TempDir dir;
  // Clock is AFTER the 2026-06-26 expiry of NIFTY26JUN24000CE.
  TestClock clock = clock_at(2026, 6, 27);
  InstrumentMaster master(static_fetcher(kCsvV1), clock, dir.path, "kite", "NFO");
  REQUIRE(master.refresh().has_value());

  const Result<Instrument> r = master.resolve("NFO", "NIFTY26JUN24000CE");
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().category == ErrorCategory::Validation);
}

TEST_CASE("an instrument is tradable on its expiry day (boundary)", "[refdata][AC3]") {
  TempDir dir;
  // Clock date EQUALS the 2026-07-30 expiry of NIFTY26JUL24000CE. An instrument
  // is tradable ON its expiry day, so resolve() must still succeed: this guards
  // the '<' (expired strictly before today) vs '<=' boundary at the gate.
  TestClock clock = clock_at(2026, 7, 30);
  InstrumentMaster master(static_fetcher(kCsvV1), clock, dir.path, "kite", "NFO");
  REQUIRE(master.refresh().has_value());

  const Result<Instrument> r = master.resolve("NFO", "NIFTY26JUL24000CE");
  REQUIRE(r.has_value());
  CHECK(r.value().expiry == "2026-07-30");
}

TEST_CASE("require_fresh gates safe-start on the trading date", "[refdata][AC2]") {
  TempDir dir;
  TestClock clock = clock_at(2026, 6, 27);
  InstrumentMaster master(static_fetcher(kCsvV1), clock, dir.path, "kite", "NFO");

  // Before any refresh: stale (DataStale + BlockStrategy), and is_fresh() false.
  const Result<broker_exec::ports::Ok> before = master.require_fresh();
  REQUIRE_FALSE(before.has_value());
  CHECK(before.error().category == ErrorCategory::DataStale);
  CHECK(before.error().action == SuggestedAction::BlockStrategy);
  CHECK_FALSE(master.is_fresh());

  // Right after a same-day refresh: fresh.
  REQUIRE(master.refresh().has_value());
  CHECK(master.require_fresh().has_value());
  CHECK(master.is_fresh());

  // Advance the wall clock one day: the cache is now a prior day -> stale again.
  clock.set_wall(wall_on(2026, 6, 28));
  const Result<broker_exec::ports::Ok> next_day = master.require_fresh();
  REQUIRE_FALSE(next_day.has_value());
  CHECK(next_day.error().category == ErrorCategory::DataStale);
  CHECK(next_day.error().action == SuggestedAction::BlockStrategy);
}

TEST_CASE("a new strike becomes tradable only after a refresh", "[refdata][AC3]") {
  TempDir dir;
  TestClock clock = clock_at(2026, 6, 27);

  // Swap the fetcher between refreshes (the new strike is absent from V1).
  std::string current_csv = kCsvV1;
  auto fetcher = [&current_csv]() -> Result<std::string> { return current_csv; };
  InstrumentMaster master(fetcher, clock, dir.path, "kite", "NFO");

  REQUIRE(master.refresh().has_value());
  CHECK_FALSE(master.resolve("NFO", "NIFTY26JUL24500CE").has_value());  // not yet present

  current_csv = kCsvV2;
  REQUIRE(master.refresh().has_value());
  const Result<Instrument> r = master.resolve("NFO", "NIFTY26JUL24500CE");
  REQUIRE(r.has_value());
  CHECK(r.value().token == 256266);
}

TEST_CASE("load_cached_for re-resolves from the date-versioned file without a fetch",
          "[refdata][AC1]") {
  TempDir dir;
  TestClock clock = clock_at(2026, 6, 27);

  // First master writes the date-versioned cache.
  {
    InstrumentMaster writer(static_fetcher(kCsvV1), clock, dir.path, "kite", "NFO");
    REQUIRE(writer.refresh().has_value());
  }
  CHECK(std::filesystem::exists(dir.path / "kite_NFO_2026-06-27.csv"));

  // A fresh master loads the cache for today WITHOUT calling the fetcher.
  int fetch_calls = 0;
  auto counting_fetcher = [&fetch_calls]() -> Result<std::string> {
    ++fetch_calls;
    return std::string{};
  };
  InstrumentMaster reader(counting_fetcher, clock, dir.path, "kite", "NFO");

  REQUIRE(reader.load_cached_for("2026-06-27").has_value());
  CHECK(fetch_calls == 0);
  CHECK(reader.is_fresh());

  const Result<Instrument> r = reader.resolve("NFO", "NIFTY26JUL24000CE");
  REQUIRE(r.has_value());
  CHECK(r.value().token == 256265);

  // No cache for a different date -> DataStale.
  const Result<broker_exec::ports::Ok> missing = reader.load_cached_for("2026-06-20");
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().category == ErrorCategory::DataStale);
}

TEST_CASE("a fetch failure leaves refresh failing and safe-start blocked", "[refdata][AC2]") {
  TempDir dir;
  TestClock clock = clock_at(2026, 6, 27);
  auto failing_fetcher = []() -> Result<std::string> {
    return broker_exec::fail(
        broker_exec::errors::make_error(ErrorCategory::Network, "download failed"));
  };
  InstrumentMaster master(failing_fetcher, clock, dir.path, "kite", "NFO");

  const Result<broker_exec::ports::Ok> refreshed = master.refresh();
  REQUIRE_FALSE(refreshed.has_value());
  CHECK(refreshed.error().category == ErrorCategory::Network);  // propagated verbatim

  // The staleness gate still blocks (no successful load happened).
  const Result<broker_exec::ports::Ok> gate = master.require_fresh();
  REQUIRE_FALSE(gate.has_value());
  CHECK(gate.error().category == ErrorCategory::DataStale);
  CHECK(gate.error().action == SuggestedAction::BlockStrategy);
}

// ── IMP-14: tick_size now goes through the SHARED decimal->paise parser ──────
//
// This file used to carry its own copy of that parse. The copy was already
// fail-closed, so the migration onto domain::parse_decimal_paise is a pure
// de-duplication — with ONE difference, and it is in the safe direction: the
// shared parser is OVERFLOW-GUARDED where the copy computed `rupees * 10 + digit`
// unchecked. A 20-digit tick_size was therefore signed-integer overflow —
// undefined behaviour, which the CI sanitizer job traps, and a wrapped
// (possibly negative) tick otherwise. The rule for the consolidation was that the
// stricter behaviour wins, so such a row is now REJECTED.
//
// The rest of this case is the pin that keeps the de-duplication honest: every
// shape the private copy accepted must still be accepted, with the same value.
TEST_CASE("tick_size is parsed by the shared fail-closed decimal parser", "[refdata][IMP-14]") {
  const auto csv_with_tick = [](const char* tick) {
    return std::string(
               "instrument_token,tradingsymbol,expiry,tick_size,lot_size,exchange\n"
               "256265,NIFTY26JUL24000CE,2026-07-30,") +
           tick + ",75,NFO\n";
  };
  const auto tick_paise_of = [&csv_with_tick](const char* tick) {
    const Result<std::vector<Instrument>> parsed =
        broker_exec::refdata::parse_instruments_csv(csv_with_tick(tick));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed.value().size() == 1);
    return parsed.value().front().tick_size.paise();
  };
  const auto refuses = [&csv_with_tick](const char* tick) {
    const Result<std::vector<Instrument>> parsed =
        broker_exec::refdata::parse_instruments_csv(csv_with_tick(tick));
    return !parsed.has_value();
  };

  SECTION("THE ONE CHANGE: a tick_size that overflows int64 paise is refused, not wrapped") {
    const Result<std::vector<Instrument>> parsed =
        broker_exec::refdata::parse_instruments_csv(csv_with_tick("99999999999999999999"));
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().category == ErrorCategory::Validation);
    CHECK(parsed.error().message.find("unparseable tick_size") != std::string::npos);

    // And the subtler overflow: a value that fits in int64 RUPEES but not once it
    // is scaled to paise. The private copy multiplied by 100 unchecked.
    CHECK(refuses("92233720368547758.08"));
  }

  SECTION("every shape the private copy accepted is still accepted, with the same value") {
    CHECK(tick_paise_of("0.05") == 5);
    CHECK(tick_paise_of("+0.05") == 5);   // a leading '+' is accepted by both
    CHECK(tick_paise_of("1") == 100);     // no fractional part at all
    CHECK(tick_paise_of("0.5") == 50);    // one fractional digit is ZERO-PADDED, not 5
    CHECK(tick_paise_of("0.059") == 5);   // sub-paise digits truncated, not refused
    CHECK(tick_paise_of(" 0.05 ") == 5);  // surrounding whitespace trimmed by both
  }

  SECTION("every shape it refused is still refused") {
    CHECK(refuses(""));       // an empty tick column
    CHECK(refuses("abc"));    // not a number at all
    CHECK(refuses("0.0x5"));  // a non-digit among the fractional digits
    CHECK(refuses("1.2.3"));  // a second decimal point
    CHECK(refuses("."));      // no digits either side
    CHECK(refuses("-"));      // a sign and nothing else
    // A well-formed NEGATIVE parses fine and is then rejected one line later by
    // the non-positive guard — a different error, and deliberately so.
    //
    // THE MESSAGE IS ASSERTED, NOT JUST THE REFUSAL. `refuses()` is satisfied by
    // EITHER error, so on its own it says nothing about WHICH guard fired: it
    // would keep passing if the shared parser started rejecting a leading '-'
    // outright, and the case would silently stop testing the thing it names. The
    // distinction also matters to whoever reads the row error — "the master states
    // a tick of -0.05" is a corrupt feed, "we could not read this tick" is a
    // format problem, and they have different fixes.
    const Result<std::vector<Instrument>> negative =
        broker_exec::refdata::parse_instruments_csv(csv_with_tick("-0.05"));
    REQUIRE_FALSE(negative.has_value());
    CHECK(negative.error().message.find("non-positive tick_size") != std::string::npos);
    CHECK(negative.error().message.find("unparseable") == std::string::npos);
  }
}

TEST_CASE("a malformed row yields a typed Error naming the row", "[refdata]") {
  // lot_size "abc" on the second data row (physical line 3) is unparseable.
  const std::string bad =
      "instrument_token,tradingsymbol,expiry,tick_size,lot_size,exchange\n"
      "256265,NIFTY26JUL24000CE,2026-07-30,0.05,75,NFO\n"
      "111111,BADROW,2026-07-30,0.05,abc,NFO\n";
  const Result<std::vector<Instrument>> parsed = broker_exec::refdata::parse_instruments_csv(bad);
  REQUIRE_FALSE(parsed.has_value());
  CHECK(parsed.error().category == ErrorCategory::Validation);
  CHECK(parsed.error().message.find("row 3") != std::string::npos);
}
