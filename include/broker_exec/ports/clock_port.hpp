#pragma once

// broker_exec::ports::ClockPort — the injected time source (FR-23).
//
// Time is a dependency, not an ambient global. ALL time in the library is read
// through this port so behavior is deterministic and testable: production wires
// a `clock::SystemClock`, tests wire a `clock::TestClock`. Per the binding
// conventions, `std::chrono::steady_clock::now()` / `system_clock::now()` are
// called in EXACTLY ONE place — the system Clock impl — and are lint-banned
// everywhere else; everything goes through this interface.
//
// Two distinct clocks, never interchanged:
//   * steady (monotonic): for timeouts, backoff, durations, loop-tick gaps.
//     Never jumps; unaffected by wall-clock adjustments.
//   * wall (system): for human/audit timestamps. May jump (NTP step, manual
//     set) — which is exactly the skew the SkewStallDetector watches for.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <chrono>

namespace broker_exec::ports {

// Abstract time source. Header-only, pure-virtual; concrete impls in
// `broker_exec::clock`.
class ClockPort {
 public:
  virtual ~ClockPort() = default;

  // Monotonic time for measuring elapsed durations (timeouts, backoff, stall
  // detection). MUST be non-decreasing across calls.
  [[nodiscard]] virtual std::chrono::steady_clock::time_point now_steady() const = 0;

  // Wall-clock time for audit/record timestamps. MAY jump (NTP/manual set).
  [[nodiscard]] virtual std::chrono::system_clock::time_point now_wall() const = 0;
};

}  // namespace broker_exec::ports
