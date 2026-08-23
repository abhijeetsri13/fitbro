#include "broker_exec/clock/skew_stall_detector.hpp"

#include <chrono>
#include <string>

namespace broker_exec::clock {

namespace {

// Absolute value of a nanosecond duration without pulling in <cstdlib>; keeps
// the sign-handling explicit so it is obvious under -Wsign-conversion.
[[nodiscard]] std::chrono::nanoseconds abs_ns(std::chrono::nanoseconds value) noexcept {
  return value < std::chrono::nanoseconds::zero() ? -value : value;
}

[[nodiscard]] std::string ms_text(std::chrono::nanoseconds value) {
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(value).count();
  return std::to_string(ms) + "ms";
}

}  // namespace

std::string_view to_string(ClockStatus status) noexcept {
  switch (status) {
    case ClockStatus::Healthy:
      return "Healthy";
    case ClockStatus::Stalled:
      return "Stalled";
    case ClockStatus::Skewed:
      return "Skewed";
  }
  return "Healthy";
}

ClockStatus SkewStallDetector::sample(std::chrono::steady_clock::time_point steady_now,
                                      std::chrono::system_clock::time_point wall_now) {
  if (!have_baseline_) {
    have_baseline_ = true;
    last_steady_ = steady_now;
    last_wall_ = wall_now;
    return status_;  // first sample only establishes the baseline
  }

  // Elapsed since the previous sample, both normalized to nanoseconds. The two
  // clocks have different native periods, so cast explicitly (no narrowing).
  const auto steady_elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(steady_now - last_steady_);
  const auto wall_elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(wall_now - last_wall_);

  last_steady_ = steady_now;
  last_wall_ = wall_now;

  // STALL takes precedence: the loop not ticking is the more fundamental fault.
  if (steady_elapsed > config_.stall_threshold) {
    status_ = ClockStatus::Stalled;
    reason_ = "main-loop stall: steady gap " + ms_text(steady_elapsed) + " exceeds threshold " +
              ms_text(config_.stall_threshold);
    return status_;
  }

  // SKEW: wall progression inconsistent with monotonic progression.
  const auto divergence = abs_ns(wall_elapsed - steady_elapsed);
  if (divergence > config_.skew_threshold) {
    status_ = ClockStatus::Skewed;
    reason_ = "clock skew: wall/steady divergence " + ms_text(divergence) + " exceeds threshold " +
              ms_text(config_.skew_threshold);
    return status_;
  }

  // Healthy this interval. Note status_ latches: it is only cleared by reset(),
  // so a prior fault remains visible until explicitly acknowledged.
  return status_;
}

void SkewStallDetector::reset() noexcept {
  status_ = ClockStatus::Healthy;
  reason_.clear();
  have_baseline_ = false;
  last_steady_ = {};
  last_wall_ = {};
}

}  // namespace broker_exec::clock
