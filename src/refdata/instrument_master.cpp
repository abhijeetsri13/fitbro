#include "broker_exec/refdata/instrument_master.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"

namespace broker_exec::refdata {

namespace {

using errors::ErrorCategory;
using errors::make_error;

// Trim ASCII spaces, tabs, and CR from both ends of a view.
[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
  const auto is_ws = [](char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  std::size_t begin = 0;
  std::size_t end = s.size();
  while (begin < end && is_ws(s[begin])) {
    ++begin;
  }
  while (end > begin && is_ws(s[end - 1])) {
    --end;
  }
  return s.substr(begin, end - begin);
}

// Parse a non-empty all-digits (optionally signed) integer. Returns nullopt on
// any non-digit or overflow. No float.
[[nodiscard]] std::optional<std::int64_t> parse_int64(std::string_view in) noexcept {
  const std::string_view s = trim(in);
  if (s.empty()) {
    return std::nullopt;
  }
  std::int64_t value = 0;
  const char* const first = s.data();
  const char* const last = s.data() + s.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec != std::errc{} || ptr != last) {
    return std::nullopt;
  }
  return value;
}

// Parse a decimal string (e.g. "0.05", "24000", "1.5") into integer paise with
// NO floating point. Up to two fractional digits are significant (paise
// precision); any further digits must still be digits but are truncated (tick
// sizes never carry sub-paise precision). Returns nullopt on malformed input.
[[nodiscard]] std::optional<std::int64_t> parse_decimal_paise(std::string_view in) noexcept {
  const std::string_view s = trim(in);
  if (s.empty()) {
    return std::nullopt;
  }

  std::size_t i = 0;
  bool negative = false;
  if (s[i] == '+' || s[i] == '-') {
    negative = (s[i] == '-');
    ++i;
  }

  std::int64_t rupees = 0;
  bool any_digit = false;
  for (; i < s.size() && s[i] != '.'; ++i) {
    if (s[i] < '0' || s[i] > '9') {
      return std::nullopt;
    }
    rupees = rupees * 10 + (s[i] - '0');
    any_digit = true;
  }

  std::int64_t frac = 0;
  int frac_digits = 0;
  if (i < s.size() && s[i] == '.') {
    ++i;
    for (; i < s.size(); ++i) {
      if (s[i] < '0' || s[i] > '9') {
        return std::nullopt;
      }
      if (frac_digits < 2) {
        frac = frac * 10 + (s[i] - '0');
        ++frac_digits;
      }
      any_digit = true;
    }
  }

  if (!any_digit) {
    return std::nullopt;
  }
  while (frac_digits < 2) {
    frac *= 10;
    ++frac_digits;
  }

  const std::int64_t paise = rupees * 100 + frac;
  return negative ? -paise : paise;
}

// Split a single CSV line into fields. Minimal quoting tolerance: a field may be
// wrapped in double-quotes and contain commas; a doubled "" inside quotes is an
// escaped literal quote. Unquoted fields split on commas.
[[nodiscard]] std::vector<std::string> split_csv_line(std::string_view line) {
  std::vector<std::string> fields;
  std::string current;
  bool in_quotes = false;

  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (in_quotes) {
      if (c == '"') {
        if (i + 1 < line.size() && line[i + 1] == '"') {
          current.push_back('"');
          ++i;
        } else {
          in_quotes = false;
        }
      } else {
        current.push_back(c);
      }
    } else if (c == '"') {
      in_quotes = true;
    } else if (c == ',') {
      fields.push_back(std::move(current));
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  fields.push_back(std::move(current));
  return fields;
}

// Split a raw CSV blob into lines, dropping a trailing CR (CRLF tolerance).
[[nodiscard]] std::vector<std::string> split_lines(std::string_view csv) {
  std::vector<std::string> lines;
  std::string current;
  for (const char c : csv) {
    if (c == '\n') {
      if (!current.empty() && current.back() == '\r') {
        current.pop_back();
      }
      lines.push_back(std::move(current));
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty() && current.back() == '\r') {
    current.pop_back();
  }
  lines.push_back(std::move(current));
  return lines;
}

[[nodiscard]] errors::Error row_error(std::size_t line_no, std::string_view reason) {
  return make_error(ErrorCategory::Validation,
                    "instrument master: row " + std::to_string(line_no) + ": " +
                        std::string(reason));
}

}  // namespace

Result<std::vector<domain::Instrument>> parse_instruments_csv(std::string_view csv) {
  const std::vector<std::string> lines = split_lines(csv);

  // Locate the header (first non-blank line); remember its 1-based line number.
  std::size_t header_idx = lines.size();
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (!trim(lines[i]).empty()) {
      header_idx = i;
      break;
    }
  }
  if (header_idx == lines.size()) {
    return fail(make_error(ErrorCategory::Validation, "instrument master: empty CSV (no header)"));
  }

  // Map header name -> column index (header order does not matter).
  std::map<std::string, std::size_t> columns;
  {
    const std::vector<std::string> header_fields = split_csv_line(lines[header_idx]);
    for (std::size_t i = 0; i < header_fields.size(); ++i) {
      columns.emplace(std::string(trim(header_fields[i])), i);
    }
  }

  const auto column_of = [&columns](std::string_view name) -> std::optional<std::size_t> {
    const auto it = columns.find(std::string(name));
    if (it == columns.end()) {
      return std::nullopt;
    }
    return it->second;
  };

  // The columns we actually consume to build a domain::Instrument.
  const auto col_token = column_of("instrument_token");
  const auto col_symbol = column_of("tradingsymbol");
  const auto col_exchange = column_of("exchange");
  const auto col_expiry = column_of("expiry");
  const auto col_tick = column_of("tick_size");
  const auto col_lot = column_of("lot_size");

  for (const auto& [name, col] :
       {std::pair{std::string_view{"instrument_token"}, col_token},
        std::pair{std::string_view{"tradingsymbol"}, col_symbol},
        std::pair{std::string_view{"exchange"}, col_exchange},
        std::pair{std::string_view{"expiry"}, col_expiry},
        std::pair{std::string_view{"tick_size"}, col_tick},
        std::pair{std::string_view{"lot_size"}, col_lot}}) {
    if (!col.has_value()) {
      return fail(make_error(ErrorCategory::Validation,
                             "instrument master: missing required column '" + std::string(name) +
                                 "'"));
    }
  }

  std::vector<domain::Instrument> instruments;
  for (std::size_t i = header_idx + 1; i < lines.size(); ++i) {
    const std::size_t line_no = i + 1;  // 1-based, for human-facing errors.
    if (trim(lines[i]).empty()) {
      continue;  // Skip blank lines.
    }

    const std::vector<std::string> fields = split_csv_line(lines[i]);

    // Every consumed column must be present in this row.
    const std::size_t max_col =
        std::max({*col_token, *col_symbol, *col_exchange, *col_expiry, *col_tick, *col_lot});
    if (fields.size() <= max_col) {
      return fail(row_error(line_no, "too few columns"));
    }

    const std::optional<std::int64_t> token = parse_int64(fields[*col_token]);
    if (!token.has_value()) {
      return fail(row_error(line_no, "unparseable instrument_token"));
    }
    const std::optional<std::int64_t> lot = parse_int64(fields[*col_lot]);
    if (!lot.has_value()) {
      return fail(row_error(line_no, "unparseable lot_size"));
    }
    const std::optional<std::int64_t> tick_paise = parse_decimal_paise(fields[*col_tick]);
    if (!tick_paise.has_value()) {
      return fail(row_error(line_no, "unparseable tick_size"));
    }

    // A non-positive lot/tick is a corrupt master row, not a valid instrument:
    // it risks a divide-by-zero in the freeze slicer (Story 2.9). Reject it.
    if (*lot <= 0) {
      return fail(row_error(line_no, "non-positive lot_size"));
    }
    if (*tick_paise <= 0) {
      return fail(row_error(line_no, "non-positive tick_size"));
    }

    domain::Instrument inst;
    inst.symbol = std::string(trim(fields[*col_symbol]));
    inst.token = *token;
    inst.exchange = std::string(trim(fields[*col_exchange]));
    inst.lot_size = domain::Quantity::of(*lot);
    inst.tick_size = domain::Price::from_paise(*tick_paise);
    // freeze_qty is NOT in the Kite CSV: default 0 here. The freeze slicer
    // (Story 2.9) supplies the per-exchange freeze ceiling downstream.
    inst.freeze_qty = domain::Quantity::of(0);
    inst.expiry = std::string(trim(fields[*col_expiry]));

    instruments.push_back(std::move(inst));
  }

  return instruments;
}

InstrumentMaster::InstrumentMaster(std::function<Result<std::string>()> fetch_csv,
                                   const ports::ClockPort& clock, std::filesystem::path cache_dir,
                                   std::string broker, std::string segment)
    : fetch_csv_(std::move(fetch_csv)),
      clock_(clock),
      cache_dir_(std::move(cache_dir)),
      broker_(std::move(broker)),
      segment_(std::move(segment)) {}

std::string InstrumentMaster::today_iso() const {
  // Floor the wall instant to days -> a sys_days -> calendar y/m/d, all via
  // std::chrono (UTC, deterministic). No localtime/strftime, no #ifdef.
  const std::chrono::system_clock::time_point now = clock_.now_wall();
  const std::chrono::sys_days day = std::chrono::floor<std::chrono::days>(now);
  const std::chrono::year_month_day ymd{day};

  const long year = static_cast<long>(static_cast<int>(ymd.year()));
  const unsigned month = static_cast<unsigned>(ymd.month());
  const unsigned dom = static_cast<unsigned>(ymd.day());

  const auto pad = [](long value, std::size_t width) {
    std::string digits = std::to_string(value);
    while (digits.size() < width) {
      digits.insert(digits.begin(), '0');
    }
    return digits;
  };

  return pad(year, 4) + "-" + pad(static_cast<long>(month), 2) + "-" +
         pad(static_cast<long>(dom), 2);
}

std::filesystem::path InstrumentMaster::cache_file_for(const std::string& iso_date) const {
  return cache_dir_ / (broker_ + "_" + segment_ + "_" + iso_date + ".csv");
}

Result<ports::Ok> InstrumentMaster::ingest(std::string_view csv, std::string iso_date) {
  Result<std::vector<domain::Instrument>> parsed = parse_instruments_csv(csv);
  if (!parsed) {
    return fail(parsed.error());
  }

  std::map<std::pair<std::string, std::string>, domain::Instrument> index;
  for (auto& inst : parsed.value()) {
    auto key = std::pair{inst.exchange, inst.symbol};
    index.insert_or_assign(std::move(key), std::move(inst));
  }

  // Commit atomically only after a fully successful parse.
  index_ = std::move(index);
  cache_date_ = std::move(iso_date);
  loaded_ = true;
  return ports::ok();
}

Result<ports::Ok> InstrumentMaster::refresh() {
  Result<std::string> csv = fetch_csv_();
  if (!csv) {
    return fail(csv.error());  // Propagate; the safe-start gate stays failing.
  }

  const std::string today = today_iso();

  // Parse + index into a LOCAL index first; only persist a master we could
  // actually read. Nothing about the object's "fresh" state changes yet.
  Result<std::vector<domain::Instrument>> parsed = parse_instruments_csv(csv.value());
  if (!parsed) {
    return fail(parsed.error());
  }

  std::map<std::pair<std::string, std::string>, domain::Instrument> index;
  for (auto& inst : parsed.value()) {
    auto key = std::pair{inst.exchange, inst.symbol};
    index.insert_or_assign(std::move(key), std::move(inst));
  }

  // Persist the raw CSV date-versioned (architecture DA-4) BEFORE marking the
  // master fresh. std::filesystem error_code overloads keep us no-throw across
  // the boundary. On any write failure we return the Error and leave the prior
  // state untouched, so require_fresh() correctly keeps failing.
  std::error_code ec;
  std::filesystem::create_directories(cache_dir_, ec);
  if (ec) {
    return fail(make_error(ErrorCategory::Internal,
                           "instrument master: cannot create cache dir"));
  }

  const std::filesystem::path file = cache_file_for(today);
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) {
    return fail(make_error(ErrorCategory::Internal,
                           "instrument master: cannot open cache file for write"));
  }
  out.write(csv.value().data(), static_cast<std::streamsize>(csv.value().size()));
  out.flush();
  if (!out) {
    return fail(make_error(ErrorCategory::Internal,
                           "instrument master: cache file write failed"));
  }

  // Commit the "fresh" state atomically only after a fully successful parse AND
  // a successfully persisted cache file.
  index_ = std::move(index);
  cache_date_ = today;
  loaded_ = true;
  return ports::ok();
}

Result<ports::Ok> InstrumentMaster::load_cached_for(const std::string& iso_date) {
  const std::filesystem::path file = cache_file_for(iso_date);

  std::error_code ec;
  if (!std::filesystem::exists(file, ec) || ec) {
    return fail(make_error(ErrorCategory::DataStale,
                           "instrument master: no cache for date " + iso_date));
  }

  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return fail(make_error(ErrorCategory::DataStale,
                           "instrument master: cannot open cache for date " + iso_date));
  }
  const std::string csv((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  return ingest(csv, iso_date);
}

Result<domain::Instrument> InstrumentMaster::resolve(const std::string& exchange,
                                                     const std::string& tradingsymbol) const {
  const auto it = index_.find(std::pair{exchange, tradingsymbol});
  if (it == index_.end()) {
    return fail(make_error(ErrorCategory::Validation,
                           "instrument master: unknown symbol " + exchange + ":" + tradingsymbol));
  }

  const domain::Instrument& inst = it->second;
  // ISO YYYY-MM-DD compares lexicographically == chronologically. A non-empty
  // expiry strictly before today is an expired contract: reject at the gate.
  if (!inst.expiry.empty() && inst.expiry < today_iso()) {
    return fail(make_error(ErrorCategory::Validation, "instrument master: expired symbol " +
                                                          exchange + ":" + tradingsymbol +
                                                          " (expiry " + inst.expiry + ")"));
  }

  return inst;
}

Result<ports::Ok> InstrumentMaster::require_fresh() const {
  if (!loaded_ || cache_date_ != today_iso()) {
    // DataStale defaults to SuggestedAction::BlockStrategy: safe-start halts.
    return fail(make_error(ErrorCategory::DataStale,
                           "instrument master is stale or undownloaded; trading blocked"));
  }
  return ports::ok();
}

bool InstrumentMaster::is_fresh() const { return require_fresh().has_value(); }

}  // namespace broker_exec::refdata
