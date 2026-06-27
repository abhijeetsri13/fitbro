#pragma once

// broker_exec::ports::RefDataPort — the abstract reference-data seam.
//
// Resolves a trading symbol to its full `domain::Instrument` (token, lot, tick,
// freeze, expiry) that the validation gate needs (Story 2.6). Reference data is
// loaded from the broker's instrument master and has an as-of time; a staleness
// gate refuses to trade on a stale master (`ErrorCategory::DataStale`). This
// story freezes the abstraction (resolve + as-of accessor); the concrete loader
// lands later.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <chrono>
#include <string_view>

#include "broker_exec/domain/types.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::ports {

// Abstract instrument reference-data source. Header-only, pure-virtual.
class RefDataPort {
 public:
  virtual ~RefDataPort() = default;

  // Resolve a trading symbol to its instrument. An unknown symbol or a stale
  // master is surfaced as an Error (Validation / DataStale respectively).
  [[nodiscard]] virtual Result<domain::Instrument> resolve(std::string_view symbol) const = 0;

  // Wall-clock instant the currently-loaded reference data was produced/loaded.
  // The freshness gate compares this against `now_wall()` to decide staleness.
  [[nodiscard]] virtual std::chrono::system_clock::time_point as_of() const = 0;
};

}  // namespace broker_exec::ports
