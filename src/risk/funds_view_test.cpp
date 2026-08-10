#include "broker_exec/risk/funds_view.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <functional>

#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

using broker_exec::Result;
using broker_exec::clock::TestClock;
using broker_exec::domain::Money;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::make_error;
using broker_exec::errors::SuggestedAction;
using broker_exec::ports::FundsSnapshot;
using broker_exec::ports::Ok;
using broker_exec::risk::FundsView;
using namespace std::chrono_literals;

namespace {

// A controllable fetch seam: counts every invocation and either returns the
// configured snapshot or a configured Error, so a test can flip the broker from
// healthy to failing between calls and observe the cadence/fail-closed behavior.
struct FetchSeam {
  int calls = 0;
  bool fail = false;
  FundsSnapshot snapshot{};

  [[nodiscard]] Result<FundsSnapshot> operator()() {
    ++calls;
    if (fail) {
      return broker_exec::fail(make_error(ErrorCategory::Network, "fetch funds failed"));
    }
    return snapshot;
  }
};

// A snapshot with a known available margin (in paise) and arbitrary used margin.
[[nodiscard]] FundsSnapshot funds_with(std::int64_t available_paise) {
  return FundsSnapshot{.available_margin = Money::from_paise(available_paise),
                       .used_margin = Money::from_paise(0)};
}

constexpr std::chrono::seconds kCadence = 5s;

}  // namespace

TEST_CASE("FundsView: fresh view passes within margin and rejects a shortfall", "[risk][funds]") {
  TestClock clock;
  FetchSeam seam;
  seam.snapshot = funds_with(1'000'000);  // 10,000 rupees available.
  // The seam must outlive the FundsView's fetch closure; both live for the test.
  FundsView view(clock, std::ref(seam), kCadence);

  REQUIRE(view.refresh());

  // required <= available -> ok (including the exact-equality boundary).
  CHECK(view.check_margin(500'000));
  CHECK(view.check_margin(1'000'000));

  // required > available -> InsufficientFunds, do-not-retry.
  auto over = view.check_margin(1'000'001);
  REQUIRE_FALSE(over);
  CHECK(over.error().category == ErrorCategory::InsufficientFunds);
  CHECK(over.error().action == SuggestedAction::DoNotRetry);
}

TEST_CASE("FundsView: refetches only after the cadence elapses", "[risk][funds]") {
  TestClock clock;
  FetchSeam seam;
  seam.snapshot = funds_with(1'000'000);
  FundsView view(clock, std::ref(seam), kCadence);

  REQUIRE(view.refresh());
  CHECK(seam.calls == 1);

  // Within cadence: a second check does NOT refetch.
  clock.advance(4s);
  CHECK(view.check_margin(1));
  CHECK(seam.calls == 1);

  // Still within cadence at the inclusive boundary (exactly 5s of age).
  clock.advance(1s);
  CHECK(view.check_margin(1));
  CHECK(seam.calls == 1);

  // Past cadence (age now 6s): the next check triggers exactly ONE refetch.
  clock.advance(1s);
  CHECK(view.check_margin(1));
  CHECK(seam.calls == 2);
}

TEST_CASE("FundsView: fail-closed on unrefreshable-stale never reuses the old snapshot",
          "[risk][funds]") {
  TestClock clock;
  FetchSeam seam;
  seam.snapshot = funds_with(1'000'000);  // first fetch is healthy.
  FundsView view(clock, std::ref(seam), kCadence);

  REQUIRE(view.refresh());
  REQUIRE(view.check_margin(1));  // healthy while fresh.

  // Age the view past cadence, then make the broker fail.
  clock.advance(6s);
  seam.fail = true;

  // The healthy available=10,000 rupees must NOT be reused: a required of 1 paise
  // (trivially affordable on the old snapshot) still blocks with DataStale.
  auto stale = view.check_margin(1);
  REQUIRE_FALSE(stale);
  CHECK(stale.error().category == ErrorCategory::DataStale);
  CHECK(stale.error().action == SuggestedAction::BlockStrategy);
}

TEST_CASE("FundsView: a successful refresh uses the NEW value, not the stale higher one",
          "[risk][funds]") {
  TestClock clock;
  FetchSeam seam;
  seam.snapshot = funds_with(1'000'000);  // first fetch: 10,000 rupees available.
  FundsView view(clock, std::ref(seam), kCadence);

  REQUIRE(view.refresh());

  // Affordable against the high available margin.
  CHECK(view.check_margin(800'000));

  // Funds drop at the broker; age the view past cadence so the next access
  // refetches the LOWER value.
  seam.snapshot = funds_with(500'000);  // now only 5,000 rupees available.
  clock.advance(6s);

  // The refreshed (lower) value must be used: 800,000 paise no longer affordable.
  auto over = view.check_margin(800'000);
  REQUIRE_FALSE(over);
  CHECK(over.error().category == ErrorCategory::InsufficientFunds);

  // And the refreshed value is what the gate now sees: 400,000 paise is affordable.
  CHECK(view.check_margin(400'000));
}

TEST_CASE("FundsView: a zero requirement still fails closed when stale", "[risk][funds]") {
  TestClock clock;
  FetchSeam seam;
  seam.snapshot = funds_with(1'000'000);
  FundsView view(clock, std::ref(seam), kCadence);

  REQUIRE(view.refresh());
  clock.advance(6s);
  seam.fail = true;

  // required == 0 does not bypass freshness: fail-closed dominates.
  auto zero = view.check_margin(0);
  REQUIRE_FALSE(zero);
  CHECK(zero.error().category == ErrorCategory::DataStale);
  CHECK(zero.error().action == SuggestedAction::BlockStrategy);
}

TEST_CASE("FundsView: invalidate() forces a refetch after a fill", "[risk][funds]") {
  TestClock clock;
  FetchSeam seam;
  seam.snapshot = funds_with(1'000'000);
  FundsView view(clock, std::ref(seam), kCadence);

  REQUIRE(view.refresh());
  CHECK(seam.calls == 1);

  // Within cadence the view is fresh, so without invalidate there is no refetch.
  clock.advance(1s);
  CHECK(view.is_fresh());

  // After a fill the runtime invalidates: the next check MUST refetch even though
  // the cadence has not elapsed.
  view.invalidate();
  CHECK_FALSE(view.is_fresh());
  CHECK(view.check_margin(1));
  CHECK(seam.calls == 2);
}

TEST_CASE("FundsView: make_funds_check matches check_margin, including the stale path",
          "[risk][funds]") {
  TestClock clock;
  FetchSeam seam;
  seam.snapshot = funds_with(1'000'000);
  FundsView view(clock, std::ref(seam), kCadence);

  REQUIRE(view.refresh());

  // Fresh + affordable: the adapter passes.
  auto ok_check = view.make_funds_check(500'000);
  CHECK(ok_check());

  // Fresh + shortfall: the adapter rejects with InsufficientFunds.
  auto short_check = view.make_funds_check(1'000'001);
  auto shortfall = short_check();
  REQUIRE_FALSE(shortfall);
  CHECK(shortfall.error().category == ErrorCategory::InsufficientFunds);

  // Stale + unrefreshable: the adapter fails closed with DataStale, exactly like
  // check_margin would.
  clock.advance(6s);
  seam.fail = true;
  auto stale_check = view.make_funds_check(1);
  auto stale = stale_check();
  REQUIRE_FALSE(stale);
  CHECK(stale.error().category == ErrorCategory::DataStale);
  CHECK(stale.error().action == SuggestedAction::BlockStrategy);
}
