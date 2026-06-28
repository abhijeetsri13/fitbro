#pragma once

// broker_exec::alerting::HeartbeatMonitor — the dead-man's-switch liveness
// primitive (Story 4.3, AC-2/AC-3, FR-28, CC-6).
//
// The alerter cannot fail silently: even when every delivery channel is down,
// the ABSENCE of heartbeats is itself the alarm. This monitor records the last
// `beat()` against the injected monotonic (steady) ClockPort and answers
// `is_alive(max_gap)`: true iff a beat was seen and the elapsed steady time
// since it is within `max_gap`.
//
// EXTERNAL-WATCHER CONTRACT: the alerter calls beat() on every heartbeat tick
// (and on each successful delivery — a delivered alert is itself a sign of
// life). A SEPARATE external process polls is_alive(max_gap) on its own clock.
// When the alerter is killed it stops beating; once the steady gap exceeds
// max_gap, is_alive flips false — that false IS the absence alarm the watcher
// fires on (AC-3). Steady (monotonic) time is used deliberately: a wall-clock
// jump can neither fake liveness nor spuriously trip the alarm.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`, no float.

#include <chrono>

#include "broker_exec/ports/clock_port.hpp"

namespace broker_exec::alerting {

class HeartbeatMonitor {
 public:
  explicit HeartbeatMonitor(const ports::ClockPort& clock) : clock_(clock) {}

  // Stamp a sign of life at the current monotonic instant. noexcept: reading the
  // injected steady clock and storing a time_point cannot fail.
  void beat() noexcept {
    last_beat_ = clock_.now_steady();
    seen_ = true;
  }

  // Liveness verdict. Before any beat -> false (no sign of life yet). Otherwise
  // true iff the elapsed monotonic time since the last beat is within max_gap.
  // Integer chrono throughout (no float): max_gap is cast to the steady clock's
  // duration and the comparison is inclusive.
  [[nodiscard]] bool is_alive(std::chrono::milliseconds max_gap) const {
    if (!seen_) {
      return false;
    }
    const std::chrono::steady_clock::duration gap = clock_.now_steady() - last_beat_;
    return gap <= std::chrono::duration_cast<std::chrono::steady_clock::duration>(max_gap);
  }

 private:
  const ports::ClockPort& clock_;
  std::chrono::steady_clock::time_point last_beat_{};
  bool seen_ = false;
};

}  // namespace broker_exec::alerting
