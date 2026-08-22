#include "broker_exec/marketdata/market_data.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <string>

#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

using broker_exec::domain::Price;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::SuggestedAction;
using broker_exec::marketdata::MarketDataState;
using broker_exec::marketdata::MarketDataView;
using broker_exec::marketdata::ReconnectResult;
using broker_exec::marketdata::Tick;
using broker_exec::ports::AlertLevel;

namespace ports = broker_exec::ports;
namespace clock_ns = broker_exec::clock;

namespace {

// Thresholds used throughout: a symbol goes Stale after 2000ms with no fresh
// tick (steady age), and Delayed when the feed lags the exchange by > 1000ms
// (wall lag).
constexpr std::chrono::milliseconds kStaleness{2000};
constexpr std::chrono::milliseconds kDelay{1000};

// A wall-clock base far enough from the epoch that exchange_ts = now_wall - 2s
// never underflows.
[[nodiscard]] std::chrono::system_clock::time_point wall_base() {
  return std::chrono::system_clock::time_point{} + std::chrono::hours{1};
}

// Recording AlertSink: counts alerts and tracks the last level/message so the
// auth-failure path can be asserted. Mirrors the sink in the reconcile tests.
class CountingAlertSink final : public ports::AlertSink {
 public:
  broker_exec::Result<ports::Ok> send(AlertLevel level, const std::string& message) override {
    ++count_;
    last_level_ = level;
    last_message_ = message;
    return ports::ok();
  }
  broker_exec::Result<ports::Ok> send_test_alert() override { return ports::ok(); }

  [[nodiscard]] std::size_t count() const noexcept { return count_; }
  [[nodiscard]] AlertLevel last_level() const noexcept { return last_level_; }
  [[nodiscard]] const std::string& last_message() const noexcept { return last_message_; }

 private:
  std::size_t count_ = 0;
  AlertLevel last_level_ = AlertLevel::Info;
  std::string last_message_;
};

Tick make_tick(std::string symbol, std::int64_t paise,
               std::chrono::system_clock::time_point exchange_ts) {
  return Tick{std::move(symbol), Price::from_paise(paise), exchange_ts};
}

}  // namespace

TEST_CASE("a never-seen symbol is Unknown and not tradable") {
  clock_ns::TestClock clock(std::chrono::steady_clock::time_point{}, wall_base());
  MarketDataView view(clock, kStaleness, kDelay);

  // Connect first so this isolates the no-tick Unknown state, not Disconnected.
  view.on_connect();

  CHECK(view.state_for("RELIANCE") == MarketDataState::Unknown);
  CHECK_FALSE(view.is_tradable("RELIANCE"));
  CHECK_FALSE(view.ltp("RELIANCE").has_value());

  const auto gated = view.require_tradable("RELIANCE");
  REQUIRE_FALSE(gated.has_value());
  CHECK(gated.error().category == ErrorCategory::DataStale);
  CHECK(gated.error().action == SuggestedAction::BlockStrategy);
}

TEST_CASE("a fresh tick (exchange_ts == now_wall) is Live and tradable") {
  clock_ns::TestClock clock(std::chrono::steady_clock::time_point{}, wall_base());
  MarketDataView view(clock, kStaleness, kDelay);

  view.on_connect();
  view.on_tick(make_tick("RELIANCE", 250000, clock.now_wall()));

  CHECK(view.state_for("RELIANCE") == MarketDataState::Live);
  CHECK(view.is_tradable("RELIANCE"));
  CHECK(view.require_tradable("RELIANCE").has_value());

  const auto price = view.ltp("RELIANCE");
  REQUIRE(price.has_value());
  CHECK(*price == Price::from_paise(250000));
}

TEST_CASE("advancing the steady clock past staleness makes a symbol Stale") {
  clock_ns::TestClock clock(std::chrono::steady_clock::time_point{}, wall_base());
  MarketDataView view(clock, kStaleness, kDelay);

  view.on_connect();
  view.on_tick(make_tick("RELIANCE", 250000, clock.now_wall()));
  REQUIRE(view.state_for("RELIANCE") == MarketDataState::Live);

  // No new tick; only the monotonic clock moves forward past the threshold.
  clock.advance(std::chrono::milliseconds{2001});

  CHECK(view.state_for("RELIANCE") == MarketDataState::Stale);
  CHECK_FALSE(view.is_tradable("RELIANCE"));
}

TEST_CASE("a freshly-received tick lagging the exchange is Delayed") {
  clock_ns::TestClock clock(std::chrono::steady_clock::time_point{}, wall_base());
  MarketDataView view(clock, kStaleness, kDelay);

  // Received just now (steady fresh) but stamped 2000ms behind the exchange — a
  // wall lag of 2000ms > the 1000ms delay threshold.
  view.on_connect();
  view.on_tick(make_tick("RELIANCE", 250000, clock.now_wall() - std::chrono::milliseconds{2000}));

  CHECK(view.state_for("RELIANCE") == MarketDataState::Delayed);
  CHECK_FALSE(view.is_tradable("RELIANCE"));
}

TEST_CASE("on_disconnect overrides a fresh tick; on_connect restores Live") {
  clock_ns::TestClock clock(std::chrono::steady_clock::time_point{}, wall_base());
  MarketDataView view(clock, kStaleness, kDelay);

  view.on_connect();
  view.on_tick(make_tick("RELIANCE", 250000, clock.now_wall()));
  REQUIRE(view.state_for("RELIANCE") == MarketDataState::Live);

  view.on_disconnect();
  CHECK(view.state_for("RELIANCE") == MarketDataState::Disconnected);
  CHECK_FALSE(view.is_tradable("RELIANCE"));

  view.on_connect();
  view.on_tick(make_tick("RELIANCE", 250100, clock.now_wall()));
  CHECK(view.state_for("RELIANCE") == MarketDataState::Live);
  CHECK(view.is_tradable("RELIANCE"));
}

TEST_CASE("de-dup / out-of-order ticks never regress the view") {
  clock_ns::TestClock clock(std::chrono::steady_clock::time_point{}, wall_base());
  MarketDataView view(clock, kStaleness, kDelay);

  view.on_connect();
  const auto t = clock.now_wall();
  view.on_tick(make_tick("RELIANCE", 250000, t));
  REQUIRE(view.ltp("RELIANCE") == Price::from_paise(250000));

  // Equal exchange_ts with a DIFFERENT ltp -> ignored.
  view.on_tick(make_tick("RELIANCE", 999999, t));
  CHECK(view.ltp("RELIANCE") == Price::from_paise(250000));

  // Older exchange_ts -> ignored.
  view.on_tick(make_tick("RELIANCE", 888888, t - std::chrono::milliseconds{1}));
  CHECK(view.ltp("RELIANCE") == Price::from_paise(250000));

  // Strictly-newer exchange_ts -> updates.
  view.on_tick(make_tick("RELIANCE", 260000, t + std::chrono::milliseconds{1}));
  CHECK(view.ltp("RELIANCE") == Price::from_paise(260000));
}

TEST_CASE("AC-3: an auth failure routes to session re-establishment, not a retry loop") {
  clock_ns::TestClock clock(std::chrono::steady_clock::time_point{}, wall_base());
  MarketDataView view(clock, kStaleness, kDelay);
  CountingAlertSink alerts;

  const auto result = view.handle_reconnect(ReconnectResult::AuthFailure, alerts);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::SessionExpired);
  CHECK(result.error().action == SuggestedAction::ReEstablishSession);
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Error);
  // It is NOT left "connected"/retrying — the socket is down, routed to session.
  CHECK(view.state_for("RELIANCE") == MarketDataState::Disconnected);
}

TEST_CASE("AC-3: a transport failure stays disconnected but returns ok (transport retries)") {
  clock_ns::TestClock clock(std::chrono::steady_clock::time_point{}, wall_base());
  MarketDataView view(clock, kStaleness, kDelay);
  CountingAlertSink alerts;

  view.on_disconnect();
  const auto result = view.handle_reconnect(ReconnectResult::TransportFailure, alerts);
  CHECK(result.has_value());
  CHECK(view.state_for("RELIANCE") == MarketDataState::Disconnected);
  CHECK(alerts.count() == 0);
}

TEST_CASE("a successful reconnect restores the connection") {
  clock_ns::TestClock clock(std::chrono::steady_clock::time_point{}, wall_base());
  MarketDataView view(clock, kStaleness, kDelay);
  CountingAlertSink alerts;

  view.on_disconnect();
  const auto result = view.handle_reconnect(ReconnectResult::Reconnected, alerts);
  CHECK(result.has_value());
  // Connected again: a fresh tick is now Live.
  view.on_tick(make_tick("RELIANCE", 250000, clock.now_wall()));
  CHECK(view.state_for("RELIANCE") == MarketDataState::Live);
}

TEST_CASE("boundary: steady age exactly equal to the staleness threshold is still Live") {
  clock_ns::TestClock clock(std::chrono::steady_clock::time_point{}, wall_base());
  MarketDataView view(clock, kStaleness, kDelay);

  view.on_connect();
  view.on_tick(make_tick("RELIANCE", 250000, clock.now_wall()));

  // Age == threshold exactly: strictly-greater means NOT yet Stale.
  clock.advance(kStaleness);
  CHECK(view.state_for("RELIANCE") == MarketDataState::Live);
  CHECK(view.is_tradable("RELIANCE"));

  // One tick past the threshold flips it to Stale.
  clock.advance(std::chrono::milliseconds{1});
  CHECK(view.state_for("RELIANCE") == MarketDataState::Stale);
}

TEST_CASE("boundary: wall lag exactly equal to the delay threshold is still Live") {
  clock_ns::TestClock clock(std::chrono::steady_clock::time_point{}, wall_base());
  MarketDataView view(clock, kStaleness, kDelay);

  view.on_connect();
  const auto exchange_ts = clock.now_wall();
  view.on_tick(make_tick("RELIANCE", 250000, exchange_ts));

  // Wall lag == threshold exactly: strictly-greater means NOT yet Delayed. Only
  // the wall clock moves (steady frozen), so staleness never enters the picture.
  clock.set_wall(exchange_ts + kDelay);
  CHECK(view.state_for("RELIANCE") == MarketDataState::Live);
  CHECK(view.is_tradable("RELIANCE"));

  // One ms past the threshold flips it to Delayed.
  clock.set_wall(exchange_ts + kDelay + std::chrono::milliseconds{1});
  CHECK(view.state_for("RELIANCE") == MarketDataState::Delayed);
  CHECK_FALSE(view.is_tradable("RELIANCE"));
}

TEST_CASE("de-dup does not restamp freshness: a duplicate tick cannot hide staleness") {
  clock_ns::TestClock clock(std::chrono::steady_clock::time_point{}, wall_base());
  MarketDataView view(clock, kStaleness, kDelay);

  view.on_connect();
  const auto t = clock.now_wall();
  view.on_tick(make_tick("RELIANCE", 250000, t));
  REQUIRE(view.state_for("RELIANCE") == MarketDataState::Live);

  // Advance the steady clock to JUST under the staleness threshold, then replay an
  // EQUAL tick (exchange_ts == stored). The de-dup guard must IGNORE it without
  // restamping received_at.
  clock.advance(kStaleness - std::chrono::milliseconds{1});
  view.on_tick(make_tick("RELIANCE", 250000, t));

  // Past the ORIGINAL threshold (measured from the first tick's received_at): had
  // the duplicate reset received_at, age would still be sub-threshold and read
  // Live. It did not, so the symbol is Stale.
  clock.advance(std::chrono::milliseconds{2});
  CHECK(view.state_for("RELIANCE") == MarketDataState::Stale);
  CHECK_FALSE(view.is_tradable("RELIANCE"));
}

TEST_CASE("a tick with a future exchange_ts is Live (no negative-lag underflow to Delayed)") {
  clock_ns::TestClock clock(std::chrono::steady_clock::time_point{}, wall_base());
  MarketDataView view(clock, kStaleness, kDelay);

  view.on_connect();
  // exchange_ts is 500ms AHEAD of local wall: the wall lag (now_wall - exchange_ts)
  // is negative, not a huge positive value — must read Live, never Delayed.
  view.on_tick(make_tick("RELIANCE", 250000, clock.now_wall() + std::chrono::milliseconds{500}));

  CHECK(view.state_for("RELIANCE") == MarketDataState::Live);
  CHECK(view.is_tradable("RELIANCE"));
}

TEST_CASE("a wall-clock jump does not fake staleness (staleness is steady-based)") {
  clock_ns::TestClock clock(std::chrono::steady_clock::time_point{}, wall_base());
  MarketDataView view(clock, kStaleness, kDelay);

  view.on_connect();
  view.on_tick(make_tick("RELIANCE", 250000, clock.now_wall()));
  REQUIRE(view.state_for("RELIANCE") == MarketDataState::Live);

  // Skew ONLY the wall clock (steady frozen). Staleness is measured on the steady
  // clock, so a wall jump must not expire the tick. A backward skew is used so the
  // wall lag (now_wall - exchange_ts) stays <= the delay threshold; a large FORWARD
  // wall jump would legitimately trip the (wall-based) delay check, so it could not
  // assert Live. Either way, staleness stays steady-based and is not faked.
  clock.set_wall(wall_base() - std::chrono::minutes{30});

  CHECK(view.state_for("RELIANCE") == MarketDataState::Live);
  CHECK(view.is_tradable("RELIANCE"));
}
