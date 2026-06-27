#pragma once

// broker_exec::clock::SkewStallDetector — health watch over the injected clock.
//
// Fed periodic samples (taken from a ports::ClockPort, or passed explicitly),
// it watches two independent failure modes and surfaces a single ClockStatus
// that later drives degrade-to-exit-only:
//
//   * STALL — the main loop stopped ticking. Detected when the gap between two
//     consecutive *steady* (monotonic) samples exceeds `stall_threshold`. A
//     stalled loop cannot honor timeouts or react to fills, so it is unsafe.
//
//   * SKEW — the wall clock jumped relative to monotonic time. Between two
//     samples, wall elapsed and steady elapsed should agree; if they diverge by
//     more than `skew_threshold` (NTP step, manual set, VM pause), audit
//     timestamps and any wall-clock-based deadlines are untrustworthy.
//
// Healthy precedence: a single sample interval can be both stalled and skewed;
// STALL is reported first (the loop not ticking is the more fundamental fault).
// The status latches to the worst observation seen since the last reset() so a
// transient fault is not silently cleared by a subsequent healthy sample.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <chrono>
#include <string>
#include <string_view>

#include "broker_exec/ports/clock_port.hpp"

namespace broker_exec::clock {

// Overall health of the injected clock / main loop. Drives exit-only later.
enum class ClockStatus { Healthy, Stalled, Skewed };

// Stable, log-friendly status names (observability contract).
[[nodiscard]] std::string_view to_string(ClockStatus status) noexcept;

// Thresholds for the two checks. Defaults are conservative placeholders; the
// runtime injects values tuned to its loop cadence.
struct SkewStallConfig {
  // Max allowed gap between consecutive steady samples before STALL. A healthy
  // loop ticks well under this; exceeding it means the loop didn't run.
  std::chrono::nanoseconds stall_threshold{std::chrono::seconds(2)};

  // Max allowed |wall_elapsed - steady_elapsed| between consecutive samples
  // before SKEW. Absorbs normal scheduling jitter; a real wall-clock jump
  // blows past it.
  std::chrono::nanoseconds skew_threshold{std::chrono::milliseconds(500)};
};

// Stateful detector. Single-threaded use (the main loop feeds it); not
// internally synchronized.
class SkewStallDetector {
 public:
  explicit SkewStallDetector(SkewStallConfig config = {}) : config_(config) {}

  // Feed one sample read from `clock`. Returns the current (latched) status.
  ClockStatus sample(const ports::ClockPort& clock) {
    return sample(clock.now_steady(), clock.now_wall());
  }

  // Feed one sample with explicit time points (e.g. from a TestClock). The
  // first sample only establishes a baseline and is always Healthy; subsequent
  // samples are compared against the previous one. Returns the latched status.
  ClockStatus sample(std::chrono::steady_clock::time_point steady_now,
                     std::chrono::system_clock::time_point wall_now);

  // The most recent overall status (latched to the worst seen since reset()).
  [[nodiscard]] ClockStatus status() const noexcept { return status_; }

  // Human-readable explanation of the latest non-healthy observation (empty
  // while Healthy). Safe to log.
  [[nodiscard]] const std::string& reason() const noexcept { return reason_; }

  // Clear all latched state and the baseline. The next sample re-establishes a
  // baseline and reports Healthy.
  void reset() noexcept;

 private:
  SkewStallConfig config_;
  ClockStatus status_{ClockStatus::Healthy};
  std::string reason_;
  bool have_baseline_{false};
  std::chrono::steady_clock::time_point last_steady_{};
  std::chrono::system_clock::time_point last_wall_{};
};

}  // namespace broker_exec::clock
