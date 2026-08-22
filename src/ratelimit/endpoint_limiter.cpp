#include "broker_exec/ratelimit/endpoint_limiter.hpp"

#include <chrono>
#include <limits>
#include <string>
#include <utility>

#include "broker_exec/errors/error.hpp"

namespace broker_exec::ratelimit {

namespace {

using errors::Error;
using errors::ErrorCategory;
using errors::make_error;
using errors::SuggestedAction;

// Integer token-per-second model (see header): one token accrues every
// 1000/N ms. N <= 0 yields a zero period => the bucket never refills (and its
// capacity clamps to 0 in RateLimiter), i.e. a permanently empty class.
[[nodiscard]] std::chrono::milliseconds refill_period_for(int per_sec) noexcept {
  if (per_sec <= 0) {
    return std::chrono::milliseconds::zero();
  }
  return std::chrono::milliseconds{1000 / per_sec};
}

// The Order bucket MUST keep a live exit lane even under an out-of-range config:
// an `order_per_sec <= 0` (or one below the reserved pool) would otherwise leave
// the Order bucket permanently empty, and since an EXIT consumes only while
// tokens >= 1, EVERY protective exit would be denied forever — the exact trapped
// position this module exists to prevent. For NON-Order classes denial is genuinely
// fail-safe, but for Order denial is NOT safety. Floor the capacity so at least the
// reserved exit tokens (and >= 1) always exist regardless of operator input.
[[nodiscard]] int order_capacity(const EndpointLimits& l) noexcept {
  const int reserved_floor = l.reserved_exit_order > 0 ? l.reserved_exit_order : 0;
  const int cap = l.order_per_sec > reserved_floor ? l.order_per_sec : reserved_floor;
  return cap > 0 ? cap : 1;
}

}  // namespace

std::string_view to_string(EndpointClass ep) noexcept {
  switch (ep) {
    case EndpointClass::Order:
      return "order";
    case EndpointClass::Quote:
      return "quote";
    case EndpointClass::Historical:
      return "historical";
    case EndpointClass::Other:
      return "other";
  }
  return "unknown";
}

// ── CircuitBreaker ──────────────────────────────────────────────────────────

CircuitBreaker::CircuitBreaker(const ports::ClockPort& clock, BreakerConfig cfg)
    : clock_(clock), cfg_(cfg), open_until_(clock.now_steady()) {
  // open_until_ starts at "now" => is_open() is false (now < now is false): the
  // breaker begins CLOSED.
}

int CircuitBreaker::cooldown_seconds_for(int trips) const noexcept {
  // Floor the config so the maths can never go pathological: a non-positive base
  // becomes 1, and the cap is at least the base. (Mirrors supervisor::backoff_for.)
  const int base = cfg_.base_cooldown_seconds > 0 ? cfg_.base_cooldown_seconds : 1;
  const int cap = cfg_.max_cooldown_seconds >= base ? cfg_.max_cooldown_seconds : base;

  // The first trip (or a non-positive index) waits exactly the base.
  if (trips <= 1) {
    return base;
  }

  // OVERFLOW-SAFE capped doubling. We want base * 2^(trips-1) but must NEVER
  // compute that shift directly (a large exponent overflows int => UB). Instead
  // we double in a loop and early-return the cap the instant doubling WOULD reach
  // it: if value > cap/2 then 2*value would reach/exceed the cap, so we clamp now
  // without doubling. That guard also proves the `value *= 2` is overflow-free
  // (value <= cap/2 <= INT_MAX/2). We iterate at most ~log2(cap/base) times no
  // matter how huge `trips` is.
  int value = base;
  for (int i = 1; i < trips; ++i) {
    if (value > cap / 2) {
      return cap;
    }
    value *= 2;  // safe: value <= cap/2 <= INT_MAX/2
    if (value >= cap) {
      return cap;
    }
  }
  return value;
}

void CircuitBreaker::record_429() {
  // A non-positive threshold is meaningless; treat it as "trip on the first 429".
  const int threshold = cfg_.trip_after_consecutive_429 > 0 ? cfg_.trip_after_consecutive_429 : 1;

  // Saturate the run counter so an arbitrarily long 429 storm never overflows.
  if (consecutive_429_ < std::numeric_limits<int>::max()) {
    ++consecutive_429_;
  }

  if (consecutive_429_ >= threshold) {
    // (Re)trip: grow the trip index (saturating) and open for a longer cooldown.
    if (trips_ < std::numeric_limits<int>::max()) {
      ++trips_;
    }
    const int cooldown = cooldown_seconds_for(trips_);
    open_until_ = clock_.now_steady() + std::chrono::seconds{cooldown};
  }
}

void CircuitBreaker::record_success() {
  // A healthy response clears the run AND fully closes the breaker: a recovered
  // broker should not stay caged by a stale cooldown.
  consecutive_429_ = 0;
  trips_ = 0;
  open_until_ = clock_.now_steady();
}

bool CircuitBreaker::is_open() const {
  return clock_.now_steady() < open_until_;
}

// ── EndpointRateLimiter ─────────────────────────────────────────────────────

EndpointRateLimiter::EndpointRateLimiter(const ports::ClockPort& clock, EndpointLimits limits,
                                         BreakerConfig breaker)
    // One bucket per class from the token model. Reserved exit lane on Order only
    // (exits are orders); quote/historical/other reserve 0.
    : order_bucket_(clock, order_capacity(limits), refill_period_for(order_capacity(limits)),
                    limits.reserved_exit_order),
      quote_bucket_(clock, limits.quote_per_sec, refill_period_for(limits.quote_per_sec), 0),
      historical_bucket_(clock, limits.historical_per_sec,
                         refill_period_for(limits.historical_per_sec), 0),
      other_bucket_(clock, limits.other_per_sec, refill_period_for(limits.other_per_sec), 0),
      order_breaker_(clock, breaker),
      quote_breaker_(clock, breaker),
      historical_breaker_(clock, breaker),
      other_breaker_(clock, breaker) {}

RateLimiter& EndpointRateLimiter::bucket_for(EndpointClass ep) noexcept {
  switch (ep) {
    case EndpointClass::Order:
      return order_bucket_;
    case EndpointClass::Quote:
      return quote_bucket_;
    case EndpointClass::Historical:
      return historical_bucket_;
    case EndpointClass::Other:
      return other_bucket_;
  }
  return other_bucket_;  // unreachable for a valid enumerator; fail to the catch-all.
}

CircuitBreaker& EndpointRateLimiter::breaker_for(EndpointClass ep) noexcept {
  switch (ep) {
    case EndpointClass::Order:
      return order_breaker_;
    case EndpointClass::Quote:
      return quote_breaker_;
    case EndpointClass::Historical:
      return historical_breaker_;
    case EndpointClass::Other:
      return other_breaker_;
  }
  return other_breaker_;  // unreachable for a valid enumerator.
}

const CircuitBreaker& EndpointRateLimiter::breaker_for(EndpointClass ep) const noexcept {
  switch (ep) {
    case EndpointClass::Order:
      return order_breaker_;
    case EndpointClass::Quote:
      return quote_breaker_;
    case EndpointClass::Historical:
      return historical_breaker_;
    case EndpointClass::Other:
      return other_breaker_;
  }
  return other_breaker_;  // unreachable for a valid enumerator.
}

Result<ports::Ok> EndpointRateLimiter::acquire(EndpointClass ep, bool is_exit) {
  // THE EXIT-ALWAYS-ALLOWED INVARIANT (enforced here): an EXIT never consults the
  // breaker — a protective exit must never be trapped by a breaker meant to stop
  // entry storms. It goes straight to its bucket, which still honors the Order
  // reserved exit lane. Only a genuinely empty bucket can deny an exit.
  if (!is_exit && breaker_for(ep).is_open()) {
    // ENTRY on an OPEN breaker: deny up front, do not touch the bucket. We stop
    // the storm LOCALLY so repeated 429s cannot escalate into an account-level
    // RMS ban. RiskRejected + DoNotRetry (a deliberate, non-retryable gate, not
    // the RateLimited/RetrySafe "slow & queue" of a merely-empty bucket). The
    // message names the endpoint CLASS only — redaction-safe.
    Error e = make_error(ErrorCategory::RiskRejected,
                         std::string{"endpoint rate limiter: circuit breaker open for endpoint '"} +
                             std::string{to_string(ep)} + "' - entry blocked");
    e.action = SuggestedAction::DoNotRetry;
    return fail(std::move(e));
  }

  // Breaker closed (or this is an exit): delegate to the endpoint's bucket and
  // propagate its verdict (RateLimited / RetrySafe on an empty bucket).
  return bucket_for(ep).acquire(is_exit);
}

void EndpointRateLimiter::record_429(EndpointClass ep) {
  breaker_for(ep).record_429();
}

void EndpointRateLimiter::record_success(EndpointClass ep) {
  breaker_for(ep).record_success();
}

bool EndpointRateLimiter::breaker_open(EndpointClass ep) const {
  return breaker_for(ep).is_open();
}

}  // namespace broker_exec::ratelimit
