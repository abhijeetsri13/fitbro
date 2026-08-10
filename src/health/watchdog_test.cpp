#include "broker_exec/health/watchdog.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/marketdata/market_data.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

using broker_exec::domain::Price;
using broker_exec::health::feed_connected_but_mute;
using broker_exec::health::HealthSignal;
using broker_exec::health::ResourceLimits;
using broker_exec::health::ResourceUsage;
using broker_exec::health::Watchdog;
using broker_exec::health::WatchdogInputs;
using broker_exec::marketdata::MarketDataState;
using broker_exec::marketdata::MarketDataView;
using broker_exec::marketdata::Tick;
using broker_exec::ports::AlertLevel;

namespace ports = broker_exec::ports;
namespace clock_ns = broker_exec::clock;

namespace {

// Recording AlertSink: counts alerts and tracks the last level/message. Mirrors
// the sink in the marketdata/reconcile tests.
class CountingAlertSink final : public ports::AlertSink {
 public:
  broker_exec::Result<ports::Ok> send(AlertLevel level, const std::string& message) override {
    (void)message;  // redaction-safe content is asserted elsewhere; here we only count + level.
    ++count_;
    last_level_ = level;
    return ports::ok();
  }
  broker_exec::Result<ports::Ok> send_test_alert() override { return ports::ok(); }

  [[nodiscard]] std::size_t count() const noexcept { return count_; }
  [[nodiscard]] AlertLevel last_level() const noexcept { return last_level_; }

 private:
  std::size_t count_ = 0;
  AlertLevel last_level_ = AlertLevel::Info;
};

// True iff `signal` appears in the verdict's breach list.
[[nodiscard]] bool has(const std::vector<HealthSignal>& breaches, HealthSignal signal) {
  return std::find(breaches.begin(), breaches.end(), signal) != breaches.end();
}

// Generous headroom limits used by the "all healthy" / single-dimension tests.
constexpr ResourceLimits kLimits{/*disk*/ 1000, /*mem*/ 2000, /*handles*/ 100};

}  // namespace

TEST_CASE("all healthy: usage under limits, no mute, no clock issue -> healthy verdict") {
  CountingAlertSink alerts;
  Watchdog watchdog(kLimits, alerts);

  WatchdogInputs in;
  in.usage = ResourceUsage{/*disk*/ 500, /*mem*/ 1000, /*handles*/ 50};

  const auto verdict = watchdog.check(in);
  CHECK(verdict.healthy());
  CHECK(verdict.breaches.empty());
  CHECK_FALSE(verdict.degrade_to_exit_only());
  CHECK(alerts.count() == 0);
}

TEST_CASE("AC-1: a connected-but-mute feed flags MuteFeed and degrades") {
  CountingAlertSink alerts;
  Watchdog watchdog(kLimits, alerts);

  WatchdogInputs in;
  in.usage = ResourceUsage{500, 1000, 50};
  in.feed_connected_but_mute = true;

  const auto verdict = watchdog.check(in);
  CHECK(has(verdict.breaches, HealthSignal::MuteFeed));
  CHECK(verdict.degrade_to_exit_only());
  CHECK_FALSE(verdict.healthy());
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Critical);
}

TEST_CASE("AC-2: disk over the headroom limit flags DiskPressure + alert") {
  CountingAlertSink alerts;
  Watchdog watchdog(kLimits, alerts);

  WatchdogInputs in;
  in.usage = ResourceUsage{/*disk*/ 1001, /*mem*/ 1000, /*handles*/ 50};

  const auto verdict = watchdog.check(in);
  CHECK(has(verdict.breaches, HealthSignal::DiskPressure));
  CHECK(verdict.breaches.size() == 1);
  CHECK(verdict.degrade_to_exit_only());
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Error);
}

TEST_CASE("AC-2: memory over the headroom limit flags MemoryPressure + alert") {
  CountingAlertSink alerts;
  Watchdog watchdog(kLimits, alerts);

  WatchdogInputs in;
  in.usage = ResourceUsage{/*disk*/ 500, /*mem*/ 2001, /*handles*/ 50};

  const auto verdict = watchdog.check(in);
  CHECK(has(verdict.breaches, HealthSignal::MemoryPressure));
  CHECK(verdict.breaches.size() == 1);
  CHECK(verdict.degrade_to_exit_only());
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Error);
}

TEST_CASE("AC-2: handles over the headroom limit flags HandlePressure + alert") {
  CountingAlertSink alerts;
  Watchdog watchdog(kLimits, alerts);

  WatchdogInputs in;
  in.usage = ResourceUsage{/*disk*/ 500, /*mem*/ 1000, /*handles*/ 101};

  const auto verdict = watchdog.check(in);
  CHECK(has(verdict.breaches, HealthSignal::HandlePressure));
  CHECK(verdict.breaches.size() == 1);
  CHECK(verdict.degrade_to_exit_only());
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Error);
}

TEST_CASE("AC-3: clock skew flags ClockSkew and degrades") {
  CountingAlertSink alerts;
  Watchdog watchdog(kLimits, alerts);

  WatchdogInputs in;
  in.usage = ResourceUsage{500, 1000, 50};
  in.clock_skew = true;

  const auto verdict = watchdog.check(in);
  CHECK(has(verdict.breaches, HealthSignal::ClockSkew));
  CHECK(verdict.breaches.size() == 1);
  CHECK(verdict.degrade_to_exit_only());
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Critical);
}

TEST_CASE("AC-3: clock stall flags ClockStall and degrades") {
  CountingAlertSink alerts;
  Watchdog watchdog(kLimits, alerts);

  WatchdogInputs in;
  in.usage = ResourceUsage{500, 1000, 50};
  in.clock_stall = true;

  const auto verdict = watchdog.check(in);
  CHECK(has(verdict.breaches, HealthSignal::ClockStall));
  CHECK(verdict.breaches.size() == 1);
  CHECK(verdict.degrade_to_exit_only());
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Critical);
}

TEST_CASE("multiple breaches at once: mute + disk + clock_stall -> all three, 3 alerts") {
  CountingAlertSink alerts;
  Watchdog watchdog(kLimits, alerts);

  WatchdogInputs in;
  in.usage = ResourceUsage{/*disk*/ 1001, /*mem*/ 1000, /*handles*/ 50};
  in.feed_connected_but_mute = true;
  in.clock_stall = true;

  const auto verdict = watchdog.check(in);
  CHECK(has(verdict.breaches, HealthSignal::MuteFeed));
  CHECK(has(verdict.breaches, HealthSignal::DiskPressure));
  CHECK(has(verdict.breaches, HealthSignal::ClockStall));
  CHECK(verdict.breaches.size() == 3);
  CHECK(verdict.degrade_to_exit_only());
  CHECK(alerts.count() == 3);
}

TEST_CASE("0 = no limit: huge usage with max_*=0 is NOT a resource breach") {
  CountingAlertSink alerts;
  Watchdog watchdog(ResourceLimits{/*disk*/ 0, /*mem*/ 0, /*handles*/ 0}, alerts);

  WatchdogInputs in;
  in.usage = ResourceUsage{/*disk*/ 1'000'000'000'000LL,
                           /*mem*/ 1'000'000'000'000LL,
                           /*handles*/ 1'000'000};

  const auto verdict = watchdog.check(in);
  CHECK(verdict.healthy());
  CHECK(verdict.breaches.empty());
  CHECK_FALSE(verdict.degrade_to_exit_only());
  CHECK(alerts.count() == 0);
}

TEST_CASE("boundary: usage exactly == limit is NOT a breach (strictly greater)") {
  CountingAlertSink alerts;
  Watchdog watchdog(kLimits, alerts);

  WatchdogInputs in;
  in.usage = ResourceUsage{/*disk*/ 1000, /*mem*/ 2000, /*handles*/ 100};  // exactly at each limit

  const auto verdict = watchdog.check(in);
  CHECK(verdict.healthy());
  CHECK(verdict.breaches.empty());
  CHECK(alerts.count() == 0);
}

TEST_CASE("mute helper: Stale->true, Live->false, Disconnected->false") {
  constexpr std::chrono::milliseconds staleness{2000};
  constexpr std::chrono::milliseconds delay{1000};
  const auto wall_base = std::chrono::system_clock::time_point{} + std::chrono::hours{1};

  clock_ns::TestClock clock(std::chrono::steady_clock::time_point{}, wall_base);
  MarketDataView view(clock, staleness, delay);

  view.on_connect();
  view.on_tick(Tick{"RELIANCE", Price::from_paise(250000), clock.now_wall()});
  REQUIRE(view.state_for("RELIANCE") == MarketDataState::Live);

  // Live: fresh ticks -> not mute.
  CHECK_FALSE(feed_connected_but_mute(view, "RELIANCE"));

  // Advance the steady clock past the staleness threshold -> Stale -> mute.
  clock.advance(staleness + std::chrono::milliseconds{1});
  REQUIRE(view.state_for("RELIANCE") == MarketDataState::Stale);
  CHECK(feed_connected_but_mute(view, "RELIANCE"));

  // A fresh tick restores Live -> not mute. NOTE: TestClock::advance() moves only
  // the steady clock, so now_wall() is unchanged from the first tick; the new tick
  // must carry a strictly-newer exchange_ts or the 3.5 de-dup guard (correctly)
  // drops it as a duplicate and freshness never refreshes.
  view.on_tick(Tick{"RELIANCE", Price::from_paise(250100),
                    clock.now_wall() + std::chrono::milliseconds{1}});
  REQUIRE(view.state_for("RELIANCE") == MarketDataState::Live);
  CHECK_FALSE(feed_connected_but_mute(view, "RELIANCE"));

  // Disconnected is a DIFFERENT signal (transport reconnect), not a mute feed.
  view.on_disconnect();
  REQUIRE(view.state_for("RELIANCE") == MarketDataState::Disconnected);
  CHECK_FALSE(feed_connected_but_mute(view, "RELIANCE"));
}
