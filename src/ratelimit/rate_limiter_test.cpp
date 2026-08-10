#include "broker_exec/ratelimit/rate_limiter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

using broker_exec::clock::TestClock;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::SuggestedAction;
using broker_exec::ratelimit::RateLimiter;
using namespace std::chrono_literals;

namespace {

// Lane readability: entries vs exits.
constexpr bool kEntry = false;
constexpr bool kExit = true;

}  // namespace

TEST_CASE("HEADLINE: entries drain to the reserved floor, exits still dispatch (AC-1)",
          "[ratelimit][reserved]") {
  TestClock clock;
  // capacity 10, refill 1 token / 1s, reserved 3 -> entries may take only 7.
  RateLimiter rl(clock, 10, 1000ms, 3);

  // With NO time advance, the first 7 ENTRY acquires succeed (10 - reserved 3).
  for (int i = 0; i < 7; ++i) {
    INFO("entry acquire #" << i);
    REQUIRE(rl.acquire(kEntry).has_value());
  }
  REQUIRE(rl.tokens() == 3);

  // The 8th ENTRY is DENIED: RateLimited + RetrySafe (it would dip into reserved).
  const auto denied = rl.acquire(kEntry);
  REQUIRE_FALSE(denied.has_value());
  CHECK(denied.error().category == ErrorCategory::RateLimited);
  CHECK(denied.error().action == SuggestedAction::RetrySafe);
  CHECK(rl.tokens() == 3);  // a denied acquire consumes nothing.

  // An EXIT acquire STILL succeeds — it draws from the reserved pool.
  REQUIRE(rl.acquire(kExit).has_value());
  CHECK(rl.tokens() == 2);

  // Exits drain the remaining reserved pool (2 -> 0); an exit succeeds while
  // tokens >= 1, a further exit at 0 is denied.
  REQUIRE(rl.try_acquire(kExit));  // 2 -> 1
  REQUIRE(rl.try_acquire(kExit));  // 1 -> 0
  REQUIRE(rl.tokens() == 0);
  CHECK_FALSE(rl.try_acquire(kExit));  // 0 -> denied
}

TEST_CASE("entries can never touch the reserved pool", "[ratelimit][reserved]") {
  TestClock clock;
  RateLimiter rl(clock, 10, 1000ms, 3);

  // Drain entries until tokens == reserved_exit (3).
  while (rl.tokens() > 3) {
    REQUIRE(rl.try_acquire(kEntry));
  }
  REQUIRE(rl.tokens() == 3);

  // At the floor: an entry is denied but an exit succeeds.
  CHECK_FALSE(rl.try_acquire(kEntry));
  CHECK(rl.available(kEntry) == 0);
  CHECK(rl.available(kExit) == 3);
  CHECK(rl.try_acquire(kExit));
}

TEST_CASE("exit can drain to zero, then is denied", "[ratelimit][reserved]") {
  TestClock clock;
  RateLimiter rl(clock, 4, 1000ms, 4);  // entire bucket is reserved.

  // No entry can ever consume (floor == capacity).
  CHECK_FALSE(rl.try_acquire(kEntry));

  // Exits drain all 4, then the 5th is denied.
  for (int i = 0; i < 4; ++i) {
    INFO("exit acquire #" << i);
    REQUIRE(rl.try_acquire(kExit));
  }
  REQUIRE(rl.tokens() == 0);
  CHECK_FALSE(rl.try_acquire(kExit));
}

TEST_CASE("refill: N whole periods accrue exactly N tokens, capped at capacity (AC-3)",
          "[ratelimit][refill]") {
  TestClock clock;
  RateLimiter rl(clock, 10, 1000ms, 3);

  // Drain fully with exits (10 -> 0).
  for (int i = 0; i < 10; ++i) {
    REQUIRE(rl.try_acquire(kExit));
  }
  REQUIRE(rl.tokens() == 0);

  // Advance exactly 5 periods -> exactly 5 tokens accrue (peek refills).
  clock.advance(5 * 1000ms);
  CHECK(rl.peek(kExit) == 5);
  CHECK(rl.tokens() == 5);

  // An entry now succeeds again (5 > reserved 3).
  CHECK(rl.try_acquire(kEntry));

  SECTION("cap: advancing far past capacity never exceeds it") {
    clock.advance(100 * 1000ms);  // would be +100 tokens, but caps at 10.
    CHECK(rl.peek(kExit) == 10);
    CHECK(rl.tokens() == 10);
  }
}

TEST_CASE("sub-period time carries with no token drift", "[ratelimit][refill][carry]") {
  TestClock clock;
  RateLimiter rl(clock, 10, 1000ms, 0);  // reserved 0 keeps the arithmetic plain.

  // Drain fully.
  for (int i = 0; i < 10; ++i) {
    REQUIRE(rl.try_acquire(kExit));
  }
  REQUIRE(rl.tokens() == 0);

  // Half a period accrues nothing yet...
  clock.advance(600ms);
  CHECK(rl.peek(kExit) == 0);
  CHECK(rl.tokens() == 0);

  // ...another 600ms makes a whole 1200ms => exactly 1 token (the 200ms carries,
  // it is NOT lost). If the remainder were dropped this would still read 0.
  clock.advance(600ms);
  CHECK(rl.peek(kExit) == 1);
  CHECK(rl.tokens() == 1);

  // The carried 200ms means only 800ms more is needed for the next token.
  clock.advance(800ms);
  CHECK(rl.peek(kExit) == 2);
}

TEST_CASE("acquire() error shape: denied -> RateLimited/RetrySafe, granted -> ok()",
          "[ratelimit][errors]") {
  TestClock clock;
  RateLimiter rl(clock, 1, 0ms, 0);  // single token, no refill.

  const auto granted = rl.acquire(kEntry);
  REQUIRE(granted.has_value());

  const auto denied = rl.acquire(kEntry);
  REQUIRE_FALSE(denied.has_value());
  CHECK(denied.error().category == ErrorCategory::RateLimited);
  CHECK(denied.error().action == SuggestedAction::RetrySafe);
  CHECK_FALSE(denied.error().message.empty());
}

TEST_CASE("clamping: negative/oversized config is made safe", "[ratelimit][config]") {
  TestClock clock;

  // reserved_exit > capacity clamps to capacity (whole bucket reserved).
  RateLimiter over(clock, 5, 1000ms, 99);
  CHECK(over.tokens() == 5);
  CHECK(over.available(kEntry) == 0);  // floor == capacity
  CHECK(over.available(kExit) == 5);

  // Negative capacity clamps to 0; reserved clamps into [0, 0].
  RateLimiter neg(clock, -3, 1000ms, -1);
  CHECK(neg.tokens() == 0);
  CHECK_FALSE(neg.try_acquire(kEntry));
  CHECK_FALSE(neg.try_acquire(kExit));
}

TEST_CASE("determinism: identical clock-advance sequences yield identical outcomes",
          "[ratelimit][determinism]") {
  TestClock clock_a;
  TestClock clock_b;
  RateLimiter a(clock_a, 4, 1000ms, 1);
  RateLimiter b(clock_b, 4, 1000ms, 1);

  // A scripted sequence of (advance_ms, is_exit) steps applied to both limiters.
  struct Step {
    std::chrono::milliseconds advance;
    bool is_exit;
  };
  const Step steps[] = {
      {0ms, kEntry},    {0ms, kEntry},    {0ms, kEntry},  {0ms, kEntry},
      {0ms, kEntry},    {0ms, kExit},     {0ms, kExit},   {1500ms, kEntry},
      {500ms, kEntry},  {3000ms, kExit},  {0ms, kExit},   {0ms, kEntry},
  };

  for (const auto& s : steps) {
    clock_a.advance(s.advance);
    clock_b.advance(s.advance);
    const bool ga = a.acquire(s.is_exit).has_value();
    const bool gb = b.acquire(s.is_exit).has_value();
    INFO("advance=" << s.advance.count() << "ms is_exit=" << s.is_exit);
    CHECK(ga == gb);
    CHECK(a.tokens() == b.tokens());
  }
}
