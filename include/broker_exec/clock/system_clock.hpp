#pragma once

// broker_exec::clock::SystemClock — the production ClockPort.
//
// ┌──────────────────────────────────────────────────────────────────────────┐
// │ THIS IS THE ONLY PLACE IN THE LIBRARY where std::chrono::steady_clock::    │
// │ now() and std::chrono::system_clock::now() may be called. Everywhere else  │
// │ those calls are lint-banned; all time flows through ports::ClockPort.      │
// │ The two calls live in system_clock.cpp.                                    │
// └──────────────────────────────────────────────────────────────────────────┘
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <chrono>

#include "broker_exec/ports/clock_port.hpp"

namespace broker_exec::clock {

// The real, wall-and-monotonic system clock. Stateless and thread-safe (each
// call just reads the OS clock via the standard library).
class SystemClock final : public ports::ClockPort {
 public:
  SystemClock() = default;

  [[nodiscard]] std::chrono::steady_clock::time_point now_steady() const override;
  [[nodiscard]] std::chrono::system_clock::time_point now_wall() const override;
};

}  // namespace broker_exec::clock
