#include "broker_exec/clock/system_clock.hpp"

#include <chrono>

// This translation unit is the SINGLE sanctioned caller of steady_clock::now()
// and system_clock::now() (binding convention; lint-banned everywhere else).
// Every other component receives time through ports::ClockPort.

namespace broker_exec::clock {

std::chrono::steady_clock::time_point SystemClock::now_steady() const {
  return std::chrono::steady_clock::now();
}

std::chrono::system_clock::time_point SystemClock::now_wall() const {
  return std::chrono::system_clock::now();
}

}  // namespace broker_exec::clock
