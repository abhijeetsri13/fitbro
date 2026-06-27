#pragma once

// broker_exec::clock::TestClock — a deterministic ClockPort for tests.
//
// Both clocks are manually controlled: no real time is ever read, so tests are
// reproducible and fast (no sleeps). `advance()` moves the monotonic clock
// forward; `set_wall()` sets the wall clock to an explicit instant — together
// they let a test induce a main-loop stall (large steady gap) or a clock skew
// (wall jump inconsistent with steady elapsed) precisely at a threshold.
//
// Inline/header-only so tests can construct and drive it without linking extra
// translation units. NEVER used in production (that is SystemClock).
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <chrono>

#include "broker_exec/ports/clock_port.hpp"

namespace broker_exec::clock {

class TestClock final : public ports::ClockPort {
 public:
  // Start both clocks at their epoch (0). Tests set explicit values as needed.
  TestClock() = default;

  // Start at explicit instants.
  TestClock(std::chrono::steady_clock::time_point steady,
            std::chrono::system_clock::time_point wall)
      : steady_(steady), wall_(wall) {}

  [[nodiscard]] std::chrono::steady_clock::time_point now_steady() const override {
    return steady_;
  }
  [[nodiscard]] std::chrono::system_clock::time_point now_wall() const override { return wall_; }

  // Move the monotonic clock forward by `delta`. Monotonic clocks never go
  // backward; callers pass a non-negative delta (a stall is a *large* delta).
  void advance(std::chrono::nanoseconds delta) { steady_ += delta; }

  // Advance BOTH clocks by the same delta — the normal, skew-free progression
  // (a healthy loop tick). Use advance()/set_wall() separately to induce skew.
  void advance_both(std::chrono::nanoseconds delta) {
    steady_ += delta;
    wall_ += std::chrono::duration_cast<std::chrono::system_clock::duration>(delta);
  }

  // Set the wall clock to an explicit instant (can jump backward/forward — that
  // is the skew under test).
  void set_wall(std::chrono::system_clock::time_point wall) { wall_ = wall; }

  // Set the monotonic clock to an explicit instant.
  void set_steady(std::chrono::steady_clock::time_point steady) { steady_ = steady; }

 private:
  std::chrono::steady_clock::time_point steady_{};
  std::chrono::system_clock::time_point wall_{};
};

}  // namespace broker_exec::clock
