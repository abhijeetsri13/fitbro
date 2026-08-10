#include "broker_exec/ratelimit/rate_limiter.hpp"

#include <chrono>
#include <cstdint>
#include <utility>

#include "broker_exec/errors/error.hpp"

namespace broker_exec::ratelimit {

namespace {

using errors::Error;
using errors::ErrorCategory;
using errors::make_error;
using errors::SuggestedAction;

// Clamp the configured reserved-exit floor into [0, capacity].
[[nodiscard]] int clamp_reserved(int reserved_exit, int capacity) noexcept {
  if (reserved_exit < 0) {
    return 0;
  }
  if (reserved_exit > capacity) {
    return capacity;
  }
  return reserved_exit;
}

}  // namespace

RateLimiter::RateLimiter(const ports::ClockPort& clock, int capacity,
                         std::chrono::milliseconds refill_period, int reserved_exit)
    : clock_(clock),
      capacity_(capacity < 0 ? 0 : capacity),
      reserved_exit_(clamp_reserved(reserved_exit, capacity_)),
      refill_period_(refill_period > std::chrono::milliseconds::zero()
                         ? refill_period
                         : std::chrono::milliseconds::zero()),
      tokens_(capacity_),
      last_refill_(clock.now_steady()) {}

void RateLimiter::refill_() {
  // Refill disabled (non-positive period): a fixed-budget bucket, never refills.
  if (refill_period_ <= std::chrono::milliseconds::zero()) {
    return;
  }

  const std::chrono::steady_clock::duration elapsed = clock_.now_steady() - last_refill_;
  if (elapsed <= std::chrono::steady_clock::duration::zero()) {
    return;  // monotonic clock has not advanced — nothing to credit.
  }

  // Integer count of WHOLE refill periods elapsed. duration / duration yields the
  // common-type integer rep (computed in nanoseconds here), so this is exact
  // integer arithmetic with no float and the sub-period remainder discarded from
  // the quotient (but preserved below via last_refill_).
  const std::int64_t whole_periods = elapsed / refill_period_;
  if (whole_periods <= 0) {
    return;  // less than one period — leave last_refill_ so the remainder carries.
  }

  // Credit tokens, capping at capacity without overflow (only add the headroom).
  const std::int64_t headroom = static_cast<std::int64_t>(capacity_) - tokens_;
  if (headroom > 0) {
    const std::int64_t gain = whole_periods < headroom ? whole_periods : headroom;
    tokens_ += static_cast<int>(gain);
  }

  // Advance the refill stamp by ONLY the consumed whole periods so the leftover
  // sub-period time carries into the next call — this is what prevents drift.
  last_refill_ += whole_periods * refill_period_;
}

bool RateLimiter::try_acquire(bool is_exit) {
  refill_();
  // An entry can never draw below the reserved pool; an exit floors at 0.
  const int floor = is_exit ? 0 : reserved_exit_;
  if (tokens_ > floor) {
    --tokens_;
    return true;
  }
  return false;
}

Result<ports::Ok> RateLimiter::acquire(bool is_exit) {
  if (try_acquire(is_exit)) {
    return ports::ok();
  }
  // Denied: RateLimited + RetrySafe is the slow+alert signal (AC-3) — the runtime
  // queues/backs off rather than letting the broker reject. The message names the
  // throttled lane and is redaction-safe (no token-shaped content).
  Error e = make_error(ErrorCategory::RateLimited,
                       is_exit ? "rate limiter: throttled (exit lane)"
                               : "rate limiter: throttled (entry lane)");
  e.action = SuggestedAction::RetrySafe;
  return fail(std::move(e));
}

int RateLimiter::available(bool is_exit) const noexcept {
  const int floor = is_exit ? 0 : reserved_exit_;
  const int avail = tokens_ - floor;
  return avail > 0 ? avail : 0;
}

int RateLimiter::peek(bool is_exit) {
  refill_();
  return available(is_exit);
}

}  // namespace broker_exec::ratelimit
