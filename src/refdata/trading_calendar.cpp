#include "broker_exec/refdata/trading_calendar.hpp"

#include <chrono>
#include <cstddef>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "broker_exec/errors/error.hpp"

namespace broker_exec::refdata {

namespace {

using errors::ErrorCategory;
using errors::make_error;
using json = nlohmann::json;

// Left-pad a non-negative integer to `width` with '0' (ISO date formatting).
[[nodiscard]] std::string pad(long value, std::size_t width) {
  std::string digits = std::to_string(value);
  while (digits.size() < width) {
    digits.insert(digits.begin(), '0');
  }
  return digits;
}

// Format a calendar y/m/d as ISO YYYY-MM-DD.
[[nodiscard]] std::string to_iso(const std::chrono::year_month_day& ymd) {
  const long year = static_cast<long>(static_cast<int>(ymd.year()));
  const unsigned month = static_cast<unsigned>(ymd.month());
  const unsigned dom = static_cast<unsigned>(ymd.day());
  return pad(year, 4) + "-" + pad(static_cast<long>(month), 2) + "-" +
         pad(static_cast<long>(dom), 2);
}

// Parse a strict ISO YYYY-MM-DD date into a sys_days. Returns nullopt on any
// shape/range violation (so an unparseable date is treated as a non-trading day
// by the caller — the safe default). No localtime, all std::chrono.
[[nodiscard]] std::optional<std::chrono::sys_days> parse_iso_date(std::string_view s) noexcept {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') {
    return std::nullopt;
  }
  const auto is_digit = [](char c) noexcept { return c >= '0' && c <= '9'; };
  for (const std::size_t i : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U}) {
    if (!is_digit(s[i])) {
      return std::nullopt;
    }
  }
  const int year = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
  const unsigned month = static_cast<unsigned>((s[5] - '0') * 10 + (s[6] - '0'));
  const unsigned day = static_cast<unsigned>((s[8] - '0') * 10 + (s[9] - '0'));

  const std::chrono::year_month_day ymd{std::chrono::year{year}, std::chrono::month{month},
                                        std::chrono::day{day}};
  if (!ymd.ok()) {
    return std::nullopt;
  }
  return std::chrono::sys_days{ymd};
}

// Parse a strict "HH:MM" (00-23:00-59) into minutes-of-day (HH*60+MM). NO float.
// Returns nullopt on any shape/range violation.
[[nodiscard]] std::optional<int> parse_hhmm(std::string_view s) noexcept {
  if (s.size() != 5 || s[2] != ':') {
    return std::nullopt;
  }
  const auto is_digit = [](char c) noexcept { return c >= '0' && c <= '9'; };
  if (!is_digit(s[0]) || !is_digit(s[1]) || !is_digit(s[3]) || !is_digit(s[4])) {
    return std::nullopt;
  }
  const int hh = (s[0] - '0') * 10 + (s[1] - '0');
  const int mm = (s[3] - '0') * 10 + (s[4] - '0');
  if (hh > 23 || mm > 59) {
    return std::nullopt;
  }
  return hh * 60 + mm;
}

}  // namespace

Result<CalendarData> parse_calendar_json(std::string_view doc) {
  // allow_exceptions=false: a parse failure yields a discarded value, never a
  // throw across the strategy-facing boundary. The iterator-pair overload accepts
  // any char range (here a string_view) cleanly across nlohmann versions.
  const json parsed =
      json::parse(doc.begin(), doc.end(), /*cb=*/nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return fail(make_error(ErrorCategory::Validation, "calendar: malformed JSON document"));
  }

  CalendarData data;

  // holidays / special_sessions are optional arrays of ISO date strings.
  const auto load_dates = [&parsed](const char* key,
                                    std::set<std::string>& out) -> std::optional<errors::Error> {
    const auto it = parsed.find(key);
    if (it == parsed.end()) {
      return std::nullopt;  // Absent -> empty set.
    }
    if (!it->is_array()) {
      return make_error(ErrorCategory::Validation,
                        std::string("calendar: '") + key + "' must be an array");
    }
    for (const auto& v : *it) {
      if (!v.is_string()) {
        return make_error(ErrorCategory::Validation,
                          std::string("calendar: '") + key + "' entries must be ISO date strings");
      }
      std::string entry = v.get<std::string>();
      // Reject a malformed date verbatim: an unparseable entry would never match a
      // real local date, silently letting the bot TRADE on a typo'd holiday.
      if (!parse_iso_date(entry).has_value()) {
        return make_error(ErrorCategory::Validation, std::string("calendar: '") + key +
                                                         "' has invalid ISO date \"" + entry + "\"");
      }
      out.insert(std::move(entry));
    }
    return std::nullopt;
  };

  if (auto err = load_dates("holidays", data.holidays)) {
    return fail(std::move(*err));
  }
  if (auto err = load_dates("special_sessions", data.special_sessions)) {
    return fail(std::move(*err));
  }

  // windows is a required object carrying the four "HH:MM" session boundaries.
  const auto windows = parsed.find("windows");
  if (windows == parsed.end() || !windows->is_object()) {
    return fail(make_error(ErrorCategory::Validation, "calendar: missing or invalid 'windows'"));
  }

  const auto get_window = [&windows](const char* key, int& out) -> bool {
    const auto f = windows->find(key);
    if (f == windows->end() || !f->is_string()) {
      return false;
    }
    const std::optional<int> minutes = parse_hhmm(f->get<std::string>());
    if (!minutes.has_value()) {
      return false;
    }
    out = *minutes;
    return true;
  };

  if (!get_window("open", data.open_min) || !get_window("entry_cutoff", data.entry_cutoff_min) ||
      !get_window("square_off", data.square_off_min) || !get_window("close", data.close_min)) {
    return fail(make_error(ErrorCategory::Validation,
                           "calendar: invalid window time (expected \"HH:MM\", 00-23:00-59)"));
  }

  return data;
}

TradingCalendar::TradingCalendar(std::function<Result<std::string>()> fetch_json,
                                 const ports::ClockPort& clock, std::filesystem::path cache_dir,
                                 std::string broker, int tz_offset_minutes)
    : fetch_json_(std::move(fetch_json)),
      clock_(clock),
      cache_dir_(std::move(cache_dir)),
      broker_(std::move(broker)),
      tz_offset_minutes_(tz_offset_minutes) {}

TradingCalendar::LocalNow TradingCalendar::local_now() const {
  // UTC wall instant -> exchange-local instant by adding the fixed tz offset,
  // then floor to days for the local calendar date and take the remainder for
  // the minute-of-day. floor() rounds toward -inf, so the remainder is always in
  // [0, 1 day) even for a negative tz offset — no negative minute-of-day.
  const std::chrono::system_clock::time_point now = clock_.now_wall();
  const std::chrono::system_clock::time_point local =
      now + std::chrono::minutes(tz_offset_minutes_);
  const std::chrono::sys_days day = std::chrono::floor<std::chrono::days>(local);
  const std::chrono::year_month_day ymd{day};

  const std::chrono::system_clock::duration since_midnight = local - day;
  const int minute_of_day =
      static_cast<int>(std::chrono::duration_cast<std::chrono::minutes>(since_midnight).count());

  return LocalNow{to_iso(ymd), minute_of_day};
}

std::string TradingCalendar::today_local() const { return local_now().iso_date; }

std::filesystem::path TradingCalendar::cache_file_for(const std::string& iso_date) const {
  return cache_dir_ / (broker_ + "_calendar_" + iso_date + ".json");
}

Result<ports::Ok> TradingCalendar::ingest(std::string_view doc, std::string iso_date) {
  Result<CalendarData> parsed = parse_calendar_json(doc);
  if (!parsed) {
    return fail(parsed.error());
  }

  // Commit atomically only after a fully successful parse.
  calendar_ = std::move(parsed.value());
  cache_date_ = std::move(iso_date);
  loaded_ = true;
  return ports::ok();
}

Result<ports::Ok> TradingCalendar::refresh() {
  Result<std::string> doc = fetch_json_();
  if (!doc) {
    return fail(doc.error());  // Propagate; the safe-start gate stays failing.
  }

  const std::string today = today_local();

  // Parse into a LOCAL result first; only persist a calendar we could actually
  // read. Nothing about the object's "fresh" state changes yet.
  Result<CalendarData> parsed = parse_calendar_json(doc.value());
  if (!parsed) {
    return fail(parsed.error());
  }

  // Persist the raw JSON date-versioned (architecture DA-4) BEFORE marking the
  // calendar fresh. std::filesystem error_code overloads keep us no-throw. On
  // any write failure we return the Error and leave the prior state untouched,
  // so require_fresh() correctly keeps failing.
  std::error_code ec;
  std::filesystem::create_directories(cache_dir_, ec);
  if (ec) {
    return fail(make_error(ErrorCategory::Internal, "calendar: cannot create cache dir"));
  }

  const std::filesystem::path file = cache_file_for(today);
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) {
    return fail(make_error(ErrorCategory::Internal, "calendar: cannot open cache file for write"));
  }
  out.write(doc.value().data(), static_cast<std::streamsize>(doc.value().size()));
  out.flush();
  if (!out) {
    return fail(make_error(ErrorCategory::Internal, "calendar: cache file write failed"));
  }

  // Commit the "fresh" state only after a successful parse AND a persisted cache.
  calendar_ = std::move(parsed.value());
  cache_date_ = today;
  loaded_ = true;
  return ports::ok();
}

Result<ports::Ok> TradingCalendar::load_cached_for(const std::string& iso_date) {
  const std::filesystem::path file = cache_file_for(iso_date);

  std::error_code ec;
  if (!std::filesystem::exists(file, ec) || ec) {
    return fail(make_error(ErrorCategory::DataStale, "calendar: no cache for date " + iso_date));
  }

  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return fail(
        make_error(ErrorCategory::DataStale, "calendar: cannot open cache for date " + iso_date));
  }
  const std::string doc((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  return ingest(doc, iso_date);
}

Result<ports::Ok> TradingCalendar::require_fresh() const {
  if (!loaded_ || cache_date_ != today_local()) {
    // DataStale defaults to SuggestedAction::BlockStrategy: safe-start halts.
    return fail(make_error(ErrorCategory::DataStale,
                           "calendar is stale or undownloaded; trading blocked"));
  }
  return ports::ok();
}

bool TradingCalendar::is_fresh() const { return require_fresh().has_value(); }

bool TradingCalendar::is_trading_day(const std::string& iso_date) const {
  // A working-day special session always wins (a session declared on a weekend
  // or on a holiday IS a trading day).
  if (calendar_.special_sessions.contains(iso_date)) {
    return true;
  }
  // An exchange holiday is not a trading day.
  if (calendar_.holidays.contains(iso_date)) {
    return false;
  }
  // Otherwise a weekend (Sat/Sun) is not a trading day; any other weekday is.
  const std::optional<std::chrono::sys_days> day = parse_iso_date(iso_date);
  if (!day.has_value()) {
    return false;  // Unparseable date -> not tradable (safe default).
  }
  const std::chrono::weekday wd{*day};
  if (wd == std::chrono::Saturday || wd == std::chrono::Sunday) {
    return false;
  }
  return true;
}

bool TradingCalendar::in_entry_window() const {
  const LocalNow now = local_now();
  return is_trading_day(now.iso_date) && now.minute_of_day >= calendar_.open_min &&
         now.minute_of_day < calendar_.entry_cutoff_min;
}

bool TradingCalendar::past_square_off() const {
  return local_now().minute_of_day >= calendar_.square_off_min;
}

bool TradingCalendar::in_session() const {
  const LocalNow now = local_now();
  return is_trading_day(now.iso_date) && now.minute_of_day >= calendar_.open_min &&
         now.minute_of_day < calendar_.close_min;
}

Result<ports::Ok> TradingCalendar::require_entry_allowed() const {
  const LocalNow now = local_now();
  // MarketClosed defaults to SuggestedAction::BlockStrategy. The message names
  // the reason and carries no secret material.
  if (!is_trading_day(now.iso_date)) {
    return fail(make_error(ErrorCategory::MarketClosed, "calendar: not a trading day"));
  }
  if (now.minute_of_day < calendar_.open_min) {
    return fail(make_error(ErrorCategory::MarketClosed, "calendar: before open"));
  }
  if (now.minute_of_day >= calendar_.entry_cutoff_min) {
    return fail(make_error(ErrorCategory::MarketClosed, "calendar: at or after entry cutoff"));
  }
  return ports::ok();
}

}  // namespace broker_exec::refdata
