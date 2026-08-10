#pragma once

// broker_exec::refdata — the instrument-master lifecycle (Story 2.6, FR-3).
//
// The per-broker instrument master (Kite's `/instruments` CSV dump) is the
// authority for symbol<->token plus the lot/tick/freeze/expiry the validation
// gate needs. This module:
//   * parses the CSV (header-mapped, order-robust) into `domain::Instrument`s,
//   * refreshes + date-versioned-caches it once per trading day,
//   * resolves a symbol to its current instrument (rejecting unknown/expired),
//   * and gates safe-start on freshness (a stale/undownloaded master blocks).
//
// Hexagonal boundary: refdata depends INWARD only on `domain`, `ports`
// (ClockPort) and `errors`. The CSV is supplied through an injected
// `fetch_csv` seam (the composition root wires Kite's `instruments()`), so this
// module stays transport-free and testable — no broker SDK/adapter dependency.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`, no
// double/float in the money/price path (binding conventions).

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "broker_exec/domain/types.hpp"
#include "broker_exec/ports/clock_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::refdata {

// Parse a Kite-shaped instruments CSV (first line = header) into instruments.
//
// Columns are mapped BY HEADER NAME, so column order and extra columns (Kite's
// dump also carries exchange_token, last_price, strike, ...) do not matter. The
// columns consumed are: instrument_token, tradingsymbol, exchange, expiry,
// tick_size, lot_size. Blank lines are skipped. Minimal quoting tolerance: a
// field may be wrapped in double-quotes and contain commas; "" is an escaped
// quote. A row with too few columns or an unparseable integer/decimal yields a
// typed Validation Error NAMING the offending row number (a bad master is never
// silently dropped). No double/float: tick_size's decimal text converts to
// integer paise via domain::Price::from_paise.
[[nodiscard]] Result<std::vector<domain::Instrument>> parse_instruments_csv(std::string_view csv);

// Owns the lifecycle of one broker/segment instrument master: refresh from the
// injected source, date-versioned caching, symbol resolution, and the
// safe-start freshness gate. Non-copyable (holds a ClockPort reference).
class InstrumentMaster {
 public:
  // `fetch_csv` returns the raw CSV (or an Error to propagate); `clock` supplies
  // wall time for the trading-date stamp; `cache_dir` is where date-versioned
  // CSVs are written; `broker`/`segment` form the cache filename prefix.
  InstrumentMaster(std::function<Result<std::string>()> fetch_csv, const ports::ClockPort& clock,
                   std::filesystem::path cache_dir, std::string broker, std::string segment);

  InstrumentMaster(const InstrumentMaster&) = delete;
  InstrumentMaster& operator=(const InstrumentMaster&) = delete;
  InstrumentMaster(InstrumentMaster&&) = delete;
  InstrumentMaster& operator=(InstrumentMaster&&) = delete;
  ~InstrumentMaster() = default;

  // Fetch + parse + index + stamp today's date + persist the raw CSV
  // date-versioned to `<cache_dir>/<broker>_<segment>_<YYYY-MM-DD>.csv`. A
  // fetch or parse failure propagates as an Error (the safe-start gate then
  // blocks). On success the in-memory index is replaced atomically.
  [[nodiscard]] Result<ports::Ok> refresh();

  // Boot path: if a date-versioned cache file for `iso_date` exists, load+parse
  // it and stamp cache_date_ = iso_date (no fetch). Otherwise a DataStale Error.
  [[nodiscard]] Result<ports::Ok> load_cached_for(const std::string& iso_date);

  // Resolve (exchange, tradingsymbol) to its current instrument. An unknown
  // symbol -> Validation Error naming it. A non-empty expiry strictly before
  // today -> Validation Error (expired, rejected at the gate). Else the
  // instrument. (ISO YYYY-MM-DD compares lexicographically, == chronologically.)
  [[nodiscard]] Result<domain::Instrument> resolve(const std::string& exchange,
                                                   const std::string& tradingsymbol) const;

  // The safe-start staleness gate (AC-2): ok() iff a refresh/load has succeeded
  // AND the cached trading date is today. Otherwise a DataStale Error with
  // SuggestedAction::BlockStrategy (start halts; the operator is alerted).
  [[nodiscard]] Result<ports::Ok> require_fresh() const;

  // Convenience predicate over require_fresh().
  [[nodiscard]] bool is_fresh() const;

  // The trading date currently loaded (ISO YYYY-MM-DD), empty if never loaded.
  [[nodiscard]] const std::string& cache_date() const noexcept { return cache_date_; }

 private:
  // Derive today's date as ISO YYYY-MM-DD from clock_.now_wall() (UTC, via
  // std::chrono — no localtime/strftime, deterministic for tests).
  [[nodiscard]] std::string today_iso() const;

  // The date-versioned cache file path for a given ISO date.
  [[nodiscard]] std::filesystem::path cache_file_for(const std::string& iso_date) const;

  // Parse `csv` and, on success, replace the index and stamp cache_date_/loaded_.
  [[nodiscard]] Result<ports::Ok> ingest(std::string_view csv, std::string iso_date);

  std::function<Result<std::string>()> fetch_csv_;
  const ports::ClockPort& clock_;
  std::filesystem::path cache_dir_;
  std::string broker_;
  std::string segment_;
  std::map<std::pair<std::string, std::string>, domain::Instrument> index_;
  std::string cache_date_;
  bool loaded_ = false;
};

}  // namespace broker_exec::refdata
