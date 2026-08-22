#pragma once

// broker_exec::ratelimit::RateLimiter — a token-bucket send-rate limiter with a
// reserved exit lane (Story 2.12, FR-24, architecture CC-1/CC-5).
//
// The library throttles broker requests itself so it SLOWS/queues + alerts under
// pressure instead of letting the broker reject us (AC-3). The headline property
// is the reserved exit lane (AC-1): a portion of the bucket — `reserved_exit`
// tokens — can NEVER be consumed by an entry. Square-off / cancel (an "exit")
// may always draw a token while one exists, including from that reserved pool, so
// throttling can never block a square-off.
//
// Token model:
//   * `capacity`      = the bucket size (max tokens, == starting/full level).
//   * `refill_period` = the steady-clock time to accrue ONE token.
//   * `reserved_exit` = tokens an ENTRY may never consume (the exit-only floor).
//   * An ENTRY (is_exit == false) consumes only while tokens > reserved_exit.
//   * An EXIT  (is_exit == true)  consumes while tokens >= 1 (floor 0).
//
// Refill is measured on the injected monotonic clock with pure std::chrono
// integer arithmetic (NO float): each acquire first credits the whole periods
// elapsed since the last refill, capping at `capacity`, and advances the refill
// stamp by ONLY those consumed whole periods so the sub-period remainder carries
// to the next call — there is no token drift over many calls.
//
// Admission only: this limiter decides WHICH request enters dispatch and WHEN a
// token is free. It never owns the broker socket or the Store. The per-send
// timeout => UNKNOWN (AC-2) is the dispatch chokepoint's job (Story 1.9); a token
// consumed for a request that then times out is NOT returned (the request was
// really sent — this is a send-rate limiter, not a concurrency pool).
//
// NOT thread-safe: it lives on, and is driven by, the single main loop.
//
// Conventions: no double/float (integer tokens + std::chrono integer durations);
// Result<T> is no-throw; no OS APIs, no `#ifdef`. Depends inward only on
// `ports` (ClockPort/Ok) and `errors`.

#include <chrono>

#include "broker_exec/ports/clock_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::ratelimit {

class RateLimiter {
 public:
  // Bind the limiter to a monotonic clock and configure the bucket. `clock` MUST
  // outlive the limiter (held by reference, as the injected process clock).
  // Inputs are clamped sanely so a misconfiguration cannot violate invariants:
  //   * `capacity`      clamped to >= 0.
  //   * `reserved_exit` clamped to [0, capacity].
  //   * `refill_period` <= 0 is treated as "no refill" (a fixed budget bucket).
  // The bucket starts FULL (tokens == capacity) and stamps last_refill from
  // clock.now_steady().
  RateLimiter(const ports::ClockPort& clock, int capacity, std::chrono::milliseconds refill_period,
              int reserved_exit);

  // Try to take one token for an entry (is_exit == false) or an exit
  // (is_exit == true). Refills first, then applies the lane floor: an entry
  // consumes only while tokens > reserved_exit (leaving the reserved pool
  // intact); an exit consumes while tokens >= 1 (floor 0, may draw the reserved
  // pool). Returns true and decrements on success, false otherwise.
  [[nodiscard]] bool try_acquire(bool is_exit);

  // try_acquire wrapped as a Result: ok() when granted; otherwise a RateLimited
  // Error with SuggestedAction::RetrySafe (the slow+alert signal — the runtime
  // queues/backs off rather than hammering the broker, AC-3). The message names
  // the lane (entry vs exit) but is redaction-safe.
  [[nodiscard]] Result<ports::Ok> acquire(bool is_exit);

  // A cheap snapshot of how many tokens the given lane could currently take:
  // max(0, tokens - (is_exit ? 0 : reserved_exit)). NOTE: this does NOT refill —
  // it reflects the token level as of the last acquire()/peek(). Use peek() for
  // a value that first credits elapsed time.
  [[nodiscard]] int available(bool is_exit) const noexcept;

  // Like available(), but refills first (credits the whole periods elapsed since
  // the last refill) and so returns an up-to-date count. Non-const because it
  // mutates the bucket's token/refill state; it does NOT consume a token.
  [[nodiscard]] int peek(bool is_exit);

  // Current raw token level (diagnostics/tests). Reflects the last refill only;
  // does not itself refill.
  [[nodiscard]] int tokens() const noexcept { return tokens_; }

 private:
  // Credit whole refill periods elapsed since last_refill_, capped at capacity_,
  // advancing last_refill_ by only the consumed whole periods (remainder carries
  // — no drift). Pure std::chrono integer arithmetic. Called at the top of each
  // acquire/peek. No-op when refill is disabled or no whole period has elapsed.
  void refill_();

  const ports::ClockPort& clock_;
  int capacity_;
  int reserved_exit_;
  std::chrono::milliseconds refill_period_;
  int tokens_;
  std::chrono::steady_clock::time_point last_refill_;
};

}  // namespace broker_exec::ratelimit
