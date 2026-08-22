#include <catch2/catch_test_macros.hpp>
#include <chrono>

#include "broker_exec/clock/skew_stall_detector.hpp"
#include "broker_exec/clock/system_clock.hpp"
#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/ports/clock_port.hpp"

using namespace std::chrono_literals;
using broker_exec::clock::ClockStatus;
using broker_exec::clock::SkewStallConfig;
using broker_exec::clock::SkewStallDetector;
using broker_exec::clock::SystemClock;
using broker_exec::clock::TestClock;

TEST_CASE("TestClock is deterministic and independently controllable", "[clock]") {
  const std::chrono::steady_clock::time_point s0{};
  const std::chrono::system_clock::time_point w0{};
  TestClock clk(s0, w0);

  REQUIRE(clk.now_steady() == s0);
  REQUIRE(clk.now_wall() == w0);

  clk.advance(10ms);
  REQUIRE(clk.now_steady() == s0 + 10ms);
  REQUIRE(clk.now_wall() == w0);  // wall untouched by advance()

  clk.advance_both(5ms);
  REQUIRE(clk.now_steady() == s0 + 15ms);
  REQUIRE(clk.now_wall() == w0 + 5ms);

  clk.set_wall(w0 + 1h);  // induce a forward wall jump
  REQUIRE(clk.now_wall() == w0 + 1h);
}

TEST_CASE("SystemClock is monotonic on steady and usable through the port", "[clock]") {
  SystemClock clk;
  const broker_exec::ports::ClockPort& port = clk;

  const auto a = port.now_steady();
  const auto b = port.now_steady();
  REQUIRE(b >= a);  // steady clock never goes backward

  // Wall clock returns a sane, post-epoch instant.
  REQUIRE(port.now_wall().time_since_epoch().count() > 0);
}

TEST_CASE("SkewStallDetector first sample only establishes a baseline", "[clock]") {
  SkewStallDetector det;
  TestClock clk;
  REQUIRE(det.sample(clk) == ClockStatus::Healthy);
  REQUIRE(det.status() == ClockStatus::Healthy);
  REQUIRE(det.reason().empty());
}

TEST_CASE("SkewStallDetector stays Healthy within thresholds", "[clock]") {
  SkewStallConfig cfg;
  cfg.stall_threshold = 2s;
  cfg.skew_threshold = 500ms;
  SkewStallDetector det(cfg);
  TestClock clk;

  det.sample(clk);  // baseline
  for (int i = 0; i < 10; ++i) {
    clk.advance_both(100ms);  // healthy tick: steady and wall move together
    REQUIRE(det.sample(clk) == ClockStatus::Healthy);
  }
  REQUIRE(det.status() == ClockStatus::Healthy);
  REQUIRE(det.reason().empty());
}

TEST_CASE("SkewStallDetector flags a main-loop stall past threshold", "[clock]") {
  SkewStallConfig cfg;
  cfg.stall_threshold = 2s;
  cfg.skew_threshold = 500ms;
  SkewStallDetector det(cfg);
  TestClock clk;

  det.sample(clk);  // baseline

  // Just under threshold -> still Healthy.
  clk.advance_both(1999ms);
  REQUIRE(det.sample(clk) == ClockStatus::Healthy);

  // Steady gap beyond the stall threshold (the loop didn't tick).
  clk.advance_both(2500ms);
  REQUIRE(det.sample(clk) == ClockStatus::Stalled);
  REQUIRE(det.status() == ClockStatus::Stalled);
  REQUIRE_FALSE(det.reason().empty());
}

TEST_CASE("SkewStallDetector flags clock skew past threshold", "[clock]") {
  SkewStallConfig cfg;
  cfg.stall_threshold = 2s;
  cfg.skew_threshold = 500ms;
  SkewStallDetector det(cfg);
  TestClock clk;

  det.sample(clk);  // baseline

  // Advance steady a little, but jump wall far ahead -> divergence > threshold.
  clk.advance(100ms);                 // steady +100ms
  clk.set_wall(clk.now_wall() + 5s);  // wall +5s (NTP step / manual set)
  REQUIRE(det.sample(clk) == ClockStatus::Skewed);
  REQUIRE(det.status() == ClockStatus::Skewed);
  REQUIRE_FALSE(det.reason().empty());
}

TEST_CASE("SkewStallDetector tolerates small wall jitter as Healthy", "[clock]") {
  SkewStallConfig cfg;
  cfg.stall_threshold = 2s;
  cfg.skew_threshold = 500ms;
  SkewStallDetector det(cfg);
  TestClock clk;

  det.sample(clk);  // baseline

  clk.advance(1000ms);                    // steady +1000ms
  clk.set_wall(clk.now_wall() + 1200ms);  // wall +1200ms -> 200ms divergence < 500ms
  REQUIRE(det.sample(clk) == ClockStatus::Healthy);
}

TEST_CASE("SkewStallDetector stall takes precedence over skew", "[clock]") {
  SkewStallConfig cfg;
  cfg.stall_threshold = 2s;
  cfg.skew_threshold = 500ms;
  SkewStallDetector det(cfg);
  TestClock clk;

  det.sample(clk);  // baseline

  // Both faults present in one interval: huge steady gap AND a wall jump.
  clk.advance(3s);
  clk.set_wall(clk.now_wall() + 10s);
  REQUIRE(det.sample(clk) == ClockStatus::Stalled);
}

TEST_CASE("SkewStallDetector latches until reset", "[clock]") {
  SkewStallConfig cfg;
  cfg.stall_threshold = 2s;
  cfg.skew_threshold = 500ms;
  SkewStallDetector det(cfg);
  TestClock clk;

  det.sample(clk);  // baseline
  clk.advance_both(3s);
  REQUIRE(det.sample(clk) == ClockStatus::Stalled);

  // A subsequent healthy interval does NOT silently clear the fault.
  clk.advance_both(100ms);
  REQUIRE(det.sample(clk) == ClockStatus::Stalled);

  det.reset();
  REQUIRE(det.status() == ClockStatus::Healthy);
  REQUIRE(det.reason().empty());

  det.sample(clk);  // new baseline after reset
  clk.advance_both(100ms);
  REQUIRE(det.sample(clk) == ClockStatus::Healthy);
}
