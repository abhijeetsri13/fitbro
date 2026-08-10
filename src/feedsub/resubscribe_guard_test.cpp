#include "broker_exec/feedsub/resubscribe_guard.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "broker_exec/clock/test_clock.hpp"

using broker_exec::clock::TestClock;
using broker_exec::feedsub::ResubscribeGuard;
using broker_exec::feedsub::to_string;
using broker_exec::feedsub::TokenFeedState;
using namespace std::chrono_literals;

namespace {

// Whether `tokens` contains `token` (order-independent membership check).
[[nodiscard]] bool contains(const std::vector<std::string>& tokens, const std::string& token) {
  return std::find(tokens.begin(), tokens.end(), token) != tokens.end();
}

}  // namespace

TEST_CASE("on_reconnect returns the FULL desired set and arms each token", "[feedsub]") {
  TestClock clock;
  ResubscribeGuard guard(clock, 1000ms);
  guard.subscribe("256265");  // NIFTY
  guard.subscribe("260105");  // BANKNIFTY
  guard.subscribe("265");     // SENSEX

  const std::vector<std::string> resubscribe = guard.on_reconnect();

  // The full set comes back, deterministically (std::map -> sorted order).
  REQUIRE(resubscribe.size() == 3);
  CHECK(resubscribe == std::vector<std::string>{"256265", "260105", "265"});

  // Every token is now awaiting its first post-resubscribe tick and NOT tradable
  // (fail-closed: it must prove it ticks before trading resumes).
  for (const std::string& token : {std::string("256265"), std::string("260105"),
                                   std::string("265")}) {
    CHECK(guard.state_of(token) == TokenFeedState::AwaitingFirstTick);
    CHECK_FALSE(guard.is_tradable(token));
  }
}

TEST_CASE("a tick marks only that token Live; the rest stay awaiting", "[feedsub]") {
  TestClock clock;
  ResubscribeGuard guard(clock, 1000ms);
  guard.subscribe("A");
  guard.subscribe("B");
  guard.subscribe("C");
  (void)guard.on_reconnect();

  guard.record_tick("B");

  CHECK(guard.state_of("B") == TokenFeedState::Live);
  CHECK(guard.is_tradable("B"));

  CHECK(guard.state_of("A") == TokenFeedState::AwaitingFirstTick);
  CHECK(guard.state_of("C") == TokenFeedState::AwaitingFirstTick);
  CHECK_FALSE(guard.is_tradable("A"));
  CHECK_FALSE(guard.is_tradable("C"));
}

TEST_CASE("MUTE: a token silent past its deadline is connected-but-mute", "[feedsub]") {
  TestClock clock;
  ResubscribeGuard guard(clock, 1000ms);
  guard.subscribe("LIVE");
  guard.subscribe("MUTE");
  (void)guard.on_reconnect();

  // LIVE ticks before the deadline; MUTE never ticks.
  clock.advance(500ms);
  guard.record_tick("LIVE");

  // Advance PAST the deadline (total 1500ms > 1000ms).
  clock.advance(1000ms);

  CHECK(guard.state_of("MUTE") == TokenFeedState::MuteAfterResubscribe);
  CHECK_FALSE(guard.is_tradable("MUTE"));
  CHECK(contains(guard.mute_tokens(), "MUTE"));

  // The token that ticked is Live and NEVER appears in mute_tokens().
  CHECK(guard.state_of("LIVE") == TokenFeedState::Live);
  CHECK(guard.is_tradable("LIVE"));
  CHECK_FALSE(contains(guard.mute_tokens(), "LIVE"));
}

TEST_CASE("boundary: exactly == deadline is still awaiting; just past is mute", "[feedsub]") {
  TestClock clock;
  ResubscribeGuard guard(clock, 1000ms);
  guard.subscribe("T");
  (void)guard.on_reconnect();

  // At exactly the deadline (age == 1000ms): STRICT boundary -> still awaiting.
  clock.advance(1000ms);
  CHECK(guard.state_of("T") == TokenFeedState::AwaitingFirstTick);
  CHECK_FALSE(guard.is_tradable("T"));
  CHECK_FALSE(contains(guard.mute_tokens(), "T"));

  // One tick past the deadline (> 1000ms) -> mute.
  clock.advance(1ms);
  CHECK(guard.state_of("T") == TokenFeedState::MuteAfterResubscribe);
  CHECK(contains(guard.mute_tokens(), "T"));
}

TEST_CASE("a second reconnect re-arms a previously-Live token", "[feedsub]") {
  TestClock clock;
  ResubscribeGuard guard(clock, 1000ms);
  guard.subscribe("X");
  (void)guard.on_reconnect();
  guard.record_tick("X");
  REQUIRE(guard.state_of("X") == TokenFeedState::Live);
  REQUIRE(guard.is_tradable("X"));

  // Reconnect again: the FULL set comes back AND the Live token reverts to
  // AwaitingFirstTick — it must re-prove it ticks on the new socket.
  const std::vector<std::string> resubscribe = guard.on_reconnect();
  CHECK(contains(resubscribe, "X"));
  CHECK(guard.state_of("X") == TokenFeedState::AwaitingFirstTick);
  CHECK_FALSE(guard.is_tradable("X"));

  // And if it now stays silent past the deadline, it goes mute.
  clock.advance(1001ms);
  CHECK(guard.state_of("X") == TokenFeedState::MuteAfterResubscribe);
  CHECK(contains(guard.mute_tokens(), "X"));
}

TEST_CASE("unsubscribe removes a token from the resubscribe set and from mute", "[feedsub]") {
  TestClock clock;
  ResubscribeGuard guard(clock, 1000ms);
  guard.subscribe("KEEP");
  guard.subscribe("DROP");
  (void)guard.on_reconnect();

  // DROP would be mute (never ticked, deadline elapsed) — until we unsubscribe it.
  clock.advance(2000ms);
  REQUIRE(contains(guard.mute_tokens(), "DROP"));

  guard.unsubscribe("DROP");
  CHECK_FALSE(contains(guard.mute_tokens(), "DROP"));

  // And it is no longer re-issued on the next reconnect.
  const std::vector<std::string> resubscribe = guard.on_reconnect();
  CHECK_FALSE(contains(resubscribe, "DROP"));
  CHECK(contains(resubscribe, "KEEP"));
}

TEST_CASE("a tick for an unknown/unsubscribed token is ignored", "[feedsub]") {
  TestClock clock;
  ResubscribeGuard guard(clock, 1000ms);
  guard.subscribe("KNOWN");
  (void)guard.on_reconnect();

  // No spurious entry/state is created for an unknown token.
  guard.record_tick("GHOST");
  CHECK(guard.state_of("GHOST") == TokenFeedState::Subscribed);  // unknown baseline
  CHECK_FALSE(guard.is_tradable("GHOST"));
  CHECK_FALSE(contains(guard.mute_tokens(), "GHOST"));

  // The known token is untouched by the ghost tick.
  CHECK(guard.state_of("KNOWN") == TokenFeedState::AwaitingFirstTick);
}

TEST_CASE("a non-positive deadline is clamped, not disabled", "[feedsub]") {
  TestClock clock;
  // A zero / negative deadline must NOT switch the mute check off (fail-closed):
  // it is clamped up to the positive floor and still fires.
  ResubscribeGuard guard(clock, 0ms);
  guard.subscribe("T");
  (void)guard.on_reconnect();

  // Advance well past the clamped floor with no tick -> mute fires.
  clock.advance(1h);
  CHECK(guard.state_of("T") == TokenFeedState::MuteAfterResubscribe);
  CHECK_FALSE(guard.is_tradable("T"));
  CHECK(contains(guard.mute_tokens(), "T"));

  // A negative deadline behaves identically (clamped, still armed).
  ResubscribeGuard neg(clock, -50ms);
  neg.subscribe("U");
  (void)neg.on_reconnect();
  clock.advance(1h);
  CHECK(neg.state_of("U") == TokenFeedState::MuteAfterResubscribe);
}

TEST_CASE("is_tradable is true ONLY for Live", "[feedsub]") {
  TestClock clock;
  ResubscribeGuard guard(clock, 1000ms);
  guard.subscribe("AWAIT");
  guard.subscribe("MUTE");
  guard.subscribe("LIVE");

  // Freshly subscribed, never connected -> Subscribed, not tradable (fail-closed).
  CHECK(guard.state_of("AWAIT") == TokenFeedState::Subscribed);
  CHECK_FALSE(guard.is_tradable("AWAIT"));

  (void)guard.on_reconnect();
  guard.record_tick("LIVE");

  // AwaitingFirstTick -> not tradable.
  CHECK_FALSE(guard.is_tradable("AWAIT"));
  // Live -> the only tradable state.
  CHECK(guard.is_tradable("LIVE"));

  clock.advance(1001ms);  // MUTE never ticked -> mute -> not tradable.
  CHECK(guard.state_of("MUTE") == TokenFeedState::MuteAfterResubscribe);
  CHECK_FALSE(guard.is_tradable("MUTE"));
  // Unknown -> not tradable.
  CHECK_FALSE(guard.is_tradable("UNKNOWN"));
}

TEST_CASE("to_string names are the stable observability contract", "[feedsub]") {
  CHECK(to_string(TokenFeedState::Subscribed) == "subscribed");
  CHECK(to_string(TokenFeedState::AwaitingFirstTick) == "awaiting_first_tick");
  CHECK(to_string(TokenFeedState::Live) == "live");
  CHECK(to_string(TokenFeedState::MuteAfterResubscribe) == "mute_after_resubscribe");
}

TEST_CASE("a LATE tick (after the deadline elapsed) still proves life -> Live, clears mute") {
  TestClock clock;
  ResubscribeGuard guard(clock, 1000ms);
  guard.subscribe("A");
  (void)guard.on_reconnect();
  clock.advance(2000ms);  // past the deadline -> mute
  REQUIRE(guard.state_of("A") == TokenFeedState::MuteAfterResubscribe);
  REQUIRE_FALSE(guard.is_tradable("A"));
  guard.record_tick("A");  // a late tick proves the feed is alive
  CHECK(guard.state_of("A") == TokenFeedState::Live);
  CHECK(guard.is_tradable("A"));
}

TEST_CASE("a token subscribed AFTER a reconnect is not tradable until the next reconnect arms it") {
  TestClock clock;
  ResubscribeGuard guard(clock, 1000ms);
  guard.subscribe("A");
  (void)guard.on_reconnect();   // arms A only
  guard.subscribe("B");         // B added after the reconnect batch
  // B was never armed -> not AwaitingFirstTick, not Live -> not tradable (fail-closed).
  CHECK_FALSE(guard.is_tradable("B"));
  // The next reconnect includes B and arms it.
  const std::vector<std::string> set = guard.on_reconnect();
  CHECK(std::find(set.begin(), set.end(), "B") != set.end());
  CHECK(guard.state_of("B") == TokenFeedState::AwaitingFirstTick);
}
