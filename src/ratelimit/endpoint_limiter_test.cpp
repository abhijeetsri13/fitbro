#include "broker_exec/ratelimit/endpoint_limiter.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>

#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

using broker_exec::clock::TestClock;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::SuggestedAction;
using broker_exec::ratelimit::BreakerConfig;
using broker_exec::ratelimit::EndpointClass;
using broker_exec::ratelimit::EndpointLimits;
using broker_exec::ratelimit::EndpointRateLimiter;
using broker_exec::ratelimit::to_string;
using namespace std::chrono_literals;

namespace {

// Lane readability: entries vs exits (mirrors rate_limiter_test.cpp).
constexpr bool kEntry = false;
constexpr bool kExit = true;

}  // namespace

TEST_CASE("per-endpoint isolation: draining Quote does NOT consume Order tokens",
          "[endpoint][isolation]") {
  TestClock clock;
  EndpointRateLimiter rl(clock, EndpointLimits{}, BreakerConfig{});

  // Quote is 1/s: the single entry is granted, the next is denied (drained).
  REQUIRE(rl.acquire(EndpointClass::Quote, kEntry).has_value());
  CHECK_FALSE(rl.acquire(EndpointClass::Quote, kEntry).has_value());

  // The Order bucket is untouched: an Order entry still succeeds. A shared global
  // bucket would have let the quote flood eat the order token — this proves it
  // cannot.
  CHECK(rl.acquire(EndpointClass::Order, kEntry).has_value());
}

TEST_CASE("each class honors its own per-second rate, then refills after one window",
          "[endpoint][rate]") {
  TestClock clock;
  EndpointRateLimiter rl(clock, EndpointLimits{}, BreakerConfig{});

  SECTION("Order ~10/s (8 entries above the reserved-2 floor)") {
    for (int i = 0; i < 8; ++i) {  // capacity 10, reserved 2 -> 8 entry tokens.
      INFO("order entry #" << i);
      REQUIRE(rl.acquire(EndpointClass::Order, kEntry).has_value());
    }
    const auto denied = rl.acquire(EndpointClass::Order, kEntry);
    REQUIRE_FALSE(denied.has_value());
    CHECK(denied.error().category == ErrorCategory::RateLimited);  // empty bucket, not breaker
    CHECK(denied.error().action == SuggestedAction::RetrySafe);

    clock.advance(1000ms);  // one full window refills the whole bucket.
    CHECK(rl.acquire(EndpointClass::Order, kEntry).has_value());
  }

  SECTION("Quote 1/s") {
    REQUIRE(rl.acquire(EndpointClass::Quote, kEntry).has_value());
    CHECK_FALSE(rl.acquire(EndpointClass::Quote, kEntry).has_value());
    clock.advance(1000ms);
    CHECK(rl.acquire(EndpointClass::Quote, kEntry).has_value());
  }

  SECTION("Historical 3/s (refill 333ms/token => 3 tokens per 1s window)") {
    for (int i = 0; i < 3; ++i) {
      INFO("historical entry #" << i);
      REQUIRE(rl.acquire(EndpointClass::Historical, kEntry).has_value());
    }
    CHECK_FALSE(rl.acquire(EndpointClass::Historical, kEntry).has_value());
    clock.advance(1000ms);
    CHECK(rl.acquire(EndpointClass::Historical, kEntry).has_value());
  }
}

TEST_CASE("reserved exit lane on Order: entry denied at the floor, exit still acquires",
          "[endpoint][reserved]") {
  TestClock clock;
  EndpointRateLimiter rl(clock, EndpointLimits{}, BreakerConfig{});

  // Drain the 8 entry tokens (10 capacity - reserved 2).
  for (int i = 0; i < 8; ++i) {
    REQUIRE(rl.acquire(EndpointClass::Order, kEntry).has_value());
  }

  // At the reserved floor: an ENTRY is denied (it would dip into the exit pool)...
  CHECK_FALSE(rl.acquire(EndpointClass::Order, kEntry).has_value());

  // ...but an EXIT still acquires, drawing from the reserved pool (2 tokens).
  CHECK(rl.acquire(EndpointClass::Order, kExit).has_value());
  CHECK(rl.acquire(EndpointClass::Order, kExit).has_value());
  // Reserved pool now empty -> a third exit is denied by the genuinely empty bucket.
  CHECK_FALSE(rl.acquire(EndpointClass::Order, kExit).has_value());
}

TEST_CASE("circuit-breaker: opens after N consecutive 429, blocks ENTRY but never EXIT",
          "[endpoint][breaker]") {
  TestClock clock;
  const BreakerConfig bc{};  // trip_after 5, base 1s, max 300s
  EndpointRateLimiter rl(clock, EndpointLimits{}, bc);

  REQUIRE_FALSE(rl.breaker_open(EndpointClass::Order));

  // Feed exactly the trip threshold of consecutive 429s -> breaker OPENS.
  for (int i = 0; i < bc.trip_after_consecutive_429; ++i) {
    rl.record_429(EndpointClass::Order);
  }
  REQUIRE(rl.breaker_open(EndpointClass::Order));

  // An ENTRY is now denied up front: RiskRejected / DoNotRetry, message NAMES the
  // endpoint class (redaction-safe).
  const auto entry = rl.acquire(EndpointClass::Order, kEntry);
  REQUIRE_FALSE(entry.has_value());
  CHECK(entry.error().category == ErrorCategory::RiskRejected);
  CHECK(entry.error().action == SuggestedAction::DoNotRetry);
  CHECK(entry.error().message.find("order") != std::string::npos);

  // THE INVARIANT: an EXIT still attempts the bucket EVEN with the breaker open —
  // a protective exit must never be trapped. The Order bucket is full, so it is
  // granted.
  CHECK(rl.acquire(EndpointClass::Order, kExit).has_value());

  // A success closes the breaker; a subsequent ENTRY is allowed again.
  rl.record_success(EndpointClass::Order);
  CHECK_FALSE(rl.breaker_open(EndpointClass::Order));
  CHECK(rl.acquire(EndpointClass::Order, kEntry).has_value());
}

TEST_CASE("circuit-breaker: a storm on entries does not trap a position's exit",
          "[endpoint][breaker][invariant]") {
  TestClock clock;
  EndpointRateLimiter rl(clock, EndpointLimits{}, BreakerConfig{});

  // A relentless 429 storm well past the trip threshold.
  for (int i = 0; i < 50; ++i) {
    rl.record_429(EndpointClass::Order);
  }
  REQUIRE(rl.breaker_open(EndpointClass::Order));

  // Entries are walled off (cannot escalate into an account ban)...
  CHECK_FALSE(rl.acquire(EndpointClass::Order, kEntry).has_value());
  // ...but every protective exit still gets a shot at the bucket. Drain the whole
  // Order bucket (capacity 10) via exits, all granted despite the open breaker.
  for (int i = 0; i < 10; ++i) {
    INFO("exit #" << i << " under open breaker");
    REQUIRE(rl.acquire(EndpointClass::Order, kExit).has_value());
  }
  // Only now (genuinely empty bucket) is an exit denied — and that is RateLimited,
  // NOT the breaker's RiskRejected.
  const auto empty = rl.acquire(EndpointClass::Order, kExit);
  REQUIRE_FALSE(empty.has_value());
  CHECK(empty.error().category == ErrorCategory::RateLimited);
}

TEST_CASE("circuit-breaker backoff grows with consecutive trips and reopens/closes correctly",
          "[endpoint][breaker][backoff]") {
  TestClock clock;
  // trip on EVERY 429 so each call advances the trip index: cooldown 1s, 2s, 4s...
  EndpointRateLimiter rl(clock, EndpointLimits{},
                         BreakerConfig{/*trip_after=*/1,
                                       /*base=*/1, /*max=*/300});

  // Trip #1 -> 1s cooldown (open_until = 0 + 1s).
  rl.record_429(EndpointClass::Quote);
  CHECK(rl.breaker_open(EndpointClass::Quote));
  clock.advance(999ms);
  CHECK(rl.breaker_open(EndpointClass::Quote));  // still open at 999ms
  clock.advance(1ms);
  CHECK_FALSE(rl.breaker_open(EndpointClass::Quote));  // closed at exactly 1000ms

  // Trip #2 -> 2s cooldown (open_until = 1000ms + 2000ms = 3000ms): strictly longer.
  rl.record_429(EndpointClass::Quote);
  CHECK(rl.breaker_open(EndpointClass::Quote));
  clock.advance(1999ms);
  CHECK(rl.breaker_open(EndpointClass::Quote));  // still open at 2999ms (1s window would be closed)
  clock.advance(1ms);
  CHECK_FALSE(rl.breaker_open(EndpointClass::Quote));  // closed at 3000ms
}

TEST_CASE("circuit-breaker cooldown is capped and overflow-safe under a huge trip count",
          "[endpoint][breaker][backoff][overflow]") {
  TestClock clock;
  // Small cap so we can step exactly to it; trip on every 429.
  EndpointRateLimiter rl(clock, EndpointLimits{},
                         BreakerConfig{/*trip_after=*/1,
                                       /*base=*/1, /*max=*/4});

  // A pathologically long storm: the trip index climbs into the tens of thousands.
  // The capped-doubling cooldown must NOT shift past the cap (no signed-overflow
  // UB) and must clamp at max_cooldown_seconds (4s).
  for (int i = 0; i < 100000; ++i) {
    rl.record_429(EndpointClass::Other);
  }
  REQUIRE(rl.breaker_open(EndpointClass::Other));

  // Cooldown is capped at 4s: still open just before, closed at the cap.
  clock.advance(3999ms);
  CHECK(rl.breaker_open(EndpointClass::Other));
  clock.advance(1ms);
  CHECK_FALSE(rl.breaker_open(EndpointClass::Other));
}

TEST_CASE("breakers are independent per endpoint class", "[endpoint][breaker][isolation]") {
  TestClock clock;
  EndpointRateLimiter rl(clock, EndpointLimits{}, BreakerConfig{});

  // Trip ONLY the Quote breaker.
  for (int i = 0; i < BreakerConfig{}.trip_after_consecutive_429; ++i) {
    rl.record_429(EndpointClass::Quote);
  }
  CHECK(rl.breaker_open(EndpointClass::Quote));
  // Order's breaker is unaffected: an Order entry still flows.
  CHECK_FALSE(rl.breaker_open(EndpointClass::Order));
  CHECK(rl.acquire(EndpointClass::Order, kEntry).has_value());
}

TEST_CASE("to_string(EndpointClass) is stable (observability contract)", "[endpoint][to_string]") {
  CHECK(to_string(EndpointClass::Order) == "order");
  CHECK(to_string(EndpointClass::Quote) == "quote");
  CHECK(to_string(EndpointClass::Historical) == "historical");
  CHECK(to_string(EndpointClass::Other) == "other");
}

TEST_CASE("MISCONFIG: an order_per_sec<=0 still keeps the Order EXIT lane alive (never trapped)",
          "[endpoint][misconfig]") {
  TestClock clock;
  // Pathological operator input: no Order tokens, but 2 reserved for exits.
  EndpointLimits bad{};
  bad.order_per_sec = 0;
  bad.reserved_exit_order = 2;
  EndpointRateLimiter rl(clock, bad, BreakerConfig{});

  // An ENTRY is (correctly) denied — there is no entry budget.
  CHECK_FALSE(rl.acquire(EndpointClass::Order, kEntry).has_value());
  // ...but a protective EXIT must STILL acquire: the floor guarantees the reserved
  // exit tokens exist regardless of the misconfig.
  CHECK(rl.acquire(EndpointClass::Order, kExit).has_value());
  CHECK(rl.acquire(EndpointClass::Order, kExit).has_value());
}

TEST_CASE("a non-positive trip_after_consecutive_429 trips on the FIRST 429 (never never-trips)",
          "[endpoint][misconfig]") {
  TestClock clock;
  EndpointRateLimiter rl(clock, EndpointLimits{}, BreakerConfig{/*trip_after=*/0, 1, 300});

  CHECK_FALSE(rl.breaker_open(EndpointClass::Order));
  rl.record_429(EndpointClass::Order);                                // a single 429
  CHECK(rl.breaker_open(EndpointClass::Order));                       // tripped immediately
  CHECK_FALSE(rl.acquire(EndpointClass::Order, kEntry).has_value());  // entry blocked
  CHECK(rl.acquire(EndpointClass::Order, kExit).has_value());         // exit still through
}

TEST_CASE("the breaker counts CONSECUTIVE 429s: an intervening success resets the run",
          "[endpoint][breaker]") {
  TestClock clock;
  EndpointRateLimiter rl(clock, EndpointLimits{}, BreakerConfig{/*trip_after=*/5, 1, 300});

  for (int i = 0; i < 4; ++i) {
    rl.record_429(EndpointClass::Quote);  // 4 < 5 -> not tripped
  }
  CHECK_FALSE(rl.breaker_open(EndpointClass::Quote));
  rl.record_success(EndpointClass::Quote);  // resets the consecutive run
  for (int i = 0; i < 4; ++i) {
    rl.record_429(EndpointClass::Quote);  // only 4 again since the reset
  }
  CHECK_FALSE(rl.breaker_open(EndpointClass::Quote));  // must NOT have tripped (no 5-in-a-row)
}
