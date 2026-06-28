#pragma once

// broker_exec::ratelimit::EndpointRateLimiter — a PER-ENDPOINT token-bucket
// limiter with a per-endpoint 429 CIRCUIT-BREAKER, hardening the single-bucket
// RateLimiter for the Kite/Kotak-Neo reality (FR-24, architecture CC-1/CC-5).
//
// WHY THIS EXISTS (real broker complaints):
//   * Kite enforces DIFFERENT limits per endpoint class — order 10/s, quote 1/s,
//     historical 3/s, "other" ~10/s — not one global budget. A single shared
//     bucket lets a quote/poll flood drain the tokens an ORDER (or a protective
//     exit) needs: the cheap, high-frequency call starves the safety-critical
//     one. Splitting the budget per endpoint class fixes that starvation.
//   * Worse: repeated 429 breaches escalate at the broker into an account-level
//     RMS block for an EXTENDED period — which can block EXITS. A reconnection /
//     poll storm that keeps hitting 429 must therefore be stopped LOCALLY before
//     it trips that ban. The per-endpoint CircuitBreaker opens after a run of
//     consecutive 429s and backs off (capped-exponential) so we stop hammering.
//
// THE LOAD-BEARING INVARIANT: a protective EXIT is NEVER trapped. The breaker
// exists to stop ENTRY storms; it must not become a second cage around exits. So
// an EXIT (is_exit == true) ALWAYS bypasses the breaker and is allowed to attempt
// its bucket; only a genuinely empty bucket can stop it (and the Order bucket
// still keeps a reserved exit lane, exactly as RateLimiter does). An ENTRY on an
// OPEN breaker is denied up front (RiskRejected / DoNotRetry, naming the
// endpoint) so a storm cannot escalate into an account ban.
//
// COMPOSITION, not reimplementation: each endpoint class owns ONE RateLimiter
// (the verbatim single-bucket limiter) plus ONE CircuitBreaker. This module adds
// the routing + breaker gate around the existing, tested bucket; it does not
// re-derive the token maths.
//
// Token model (documented, integer-only — NO float):
//   For a class rated N per second we build a RateLimiter with
//     capacity      = N                       (a full second's worth of tokens)
//     refill_period = 1000ms / N  (integer)   (one token accrues every 1000/N ms)
//     reserved_exit = reserved_exit_order on Order only; 0 on quote/historical/
//                     other (exits are orders, so only the Order bucket reserves).
//   So one full 1-second window refills the whole bucket (N * (1000/N) <= 1000ms),
//   and the steady drip means a class can sustain ~N tokens/second. Integer
//   division floors the period (e.g. 1000/3 = 333ms, so 3 tokens accrue in 999ms)
//   — a hair conservative, never permissive, which is the safe direction for a
//   rate gate. N <= 0 yields an empty, never-refilling bucket (RateLimiter clamps).
//
// Time is injected: the breaker reads the SAME monotonic ClockPort as the bucket
// (NO ambient std::chrono::now / Date::now — this codebase forbids ambient time).
//
// NOT thread-safe: lives on, and is driven by, the single main loop (as RateLimiter).
//
// Conventions: no double/float (integer tokens + integer seconds); Result<T> is
// no-throw; redaction-safe error messages (name the endpoint CLASS, never a
// secret); no OS API, no `#ifdef`. Depends inward only on `ports`, `errors`, and
// the sibling RateLimiter.

#include <chrono>
#include <string_view>

#include "broker_exec/ports/clock_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/ratelimit/rate_limiter.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::ratelimit {

// The endpoint classes Kite/Kotak-Neo rate-limit independently. `Other` is the
// catch-all bucket for everything not separately limited. These names are part
// of the observability contract (to_string below); renaming is breaking (NFR-8).
enum class EndpointClass { Order, Quote, Historical, Other };

// Stable, log/serialization-friendly name for an EndpointClass. Redaction-safe
// (a fixed class word, never a secret).
[[nodiscard]] std::string_view to_string(EndpointClass ep) noexcept;

// Per-second budgets per endpoint class (Kite defaults) plus the Order-only
// reserved exit floor. Integer tokens-per-second only.
//   *_per_sec          — the class's sustained rate (bucket capacity == this).
//   reserved_exit_order— tokens in the ORDER bucket an entry may never consume,
//                        leaving them for protective exits (square-off/cancel).
//                        Quote/Historical/Other reserve nothing (they are not
//                        exit-bearing endpoints).
struct EndpointLimits {
  int order_per_sec = 10;
  int quote_per_sec = 1;
  int historical_per_sec = 3;
  int other_per_sec = 10;
  int reserved_exit_order = 2;
};

// The 429 circuit-breaker tuning. Integer seconds only.
//   trip_after_consecutive_429 — open the breaker once this many 429s arrive
//                                back-to-back with no intervening success.
//   base_cooldown_seconds      — the first/minimum open duration (the floor).
//   max_cooldown_seconds       — the open-duration cap (the ceiling).
struct BreakerConfig {
  int trip_after_consecutive_429 = 5;
  int base_cooldown_seconds = 1;
  int max_cooldown_seconds = 300;
};

// CircuitBreaker — one per endpoint class. It tracks the run of consecutive 429s
// the caller reports and, once that run reaches the trip threshold, OPENS for a
// capped-exponential cooldown measured on the injected monotonic clock. While
// open, the EndpointRateLimiter denies ENTRIES for that class (exits bypass it).
//
// Backoff growth: each (re)trip lengthens the cooldown — the Nth trip opens for
// min(base * 2^(N-1), max) seconds — so a sustained storm backs off harder, up to
// the cap. A reported SUCCESS clears the run AND closes the breaker (a healthy
// response means the broker is no longer throttling us).
//
// NOT thread-safe (single main loop). `clock` MUST outlive the breaker.
class CircuitBreaker {
 public:
  CircuitBreaker(const ports::ClockPort& clock, BreakerConfig cfg);

  // Report one 429 (or 429-equivalent Indeterminate) for this endpoint. Extends
  // the consecutive run; on reaching the trip threshold it opens (or re-opens
  // with a longer cooldown) from clock.now_steady(). The internal counters
  // saturate, so an arbitrarily long storm never overflows / UBs.
  void record_429();

  // Report one success for this endpoint: resets the consecutive run and CLOSES
  // the breaker immediately (the broker is responding normally again).
  void record_success();

  // True while the breaker is open (now < the open-until instant). Reads the
  // injected clock; const and side-effect-free.
  [[nodiscard]] bool is_open() const;

 private:
  // Capped, overflow-safe exponential cooldown for the Nth trip:
  //   min(base * 2^(N-1), max), clamped >= base. N <= 1 ⇒ base.
  // Mirrors the supervisor::backoff_for pattern (doubles in a loop, early-returns
  // the cap the instant doubling WOULD reach it, so the shift never overflows int
  // however large N is). Reimplemented locally to avoid a cross-module link.
  [[nodiscard]] int cooldown_seconds_for(int trips) const noexcept;

  const ports::ClockPort& clock_;
  BreakerConfig cfg_;
  int consecutive_429_ = 0;  // length of the current back-to-back 429 run
  int trips_ = 0;            // number of times the breaker has opened (backoff index)
  std::chrono::steady_clock::time_point open_until_;  // open while now < this
};

// EndpointRateLimiter — the per-endpoint gate. Routes each request to its
// endpoint class's RateLimiter, guarded by that class's CircuitBreaker, with the
// exit-always-allowed invariant enforced here.
//
// NOT thread-safe (single main loop). `clock` MUST outlive the limiter.
class EndpointRateLimiter {
 public:
  // Build one RateLimiter + one CircuitBreaker per EndpointClass from `limits`
  // (see the token model in the file header) and `breaker`.
  EndpointRateLimiter(const ports::ClockPort& clock, EndpointLimits limits, BreakerConfig breaker);

  // The gate. For an ENTRY (is_exit == false): if `ep`'s breaker is OPEN, deny
  // immediately with a RiskRejected / DoNotRetry Error NAMING the endpoint class
  // (redaction-safe) — do NOT touch the bucket. Otherwise delegate to `ep`'s
  // RateLimiter.acquire(is_exit) and propagate its verdict.
  //
  // For an EXIT (is_exit == true): the breaker is ALWAYS bypassed — a protective
  // exit must never be trapped by a breaker meant to stop entry storms — and the
  // request goes straight to `ep`'s bucket (which still honors the Order reserved
  // exit lane). Only a genuinely empty bucket can deny an exit.
  [[nodiscard]] Result<ports::Ok> acquire(EndpointClass ep, bool is_exit);

  // Feed broker responses to drive `ep`'s breaker: a 429 / Indeterminate ⇒
  // record_429; a success ⇒ record_success.
  void record_429(EndpointClass ep);
  void record_success(EndpointClass ep);

  // Observability/tests: is `ep`'s breaker currently open?
  [[nodiscard]] bool breaker_open(EndpointClass ep) const;

 private:
  // Route an EndpointClass to its owned bucket / breaker (const + mutable forms).
  [[nodiscard]] RateLimiter& bucket_for(EndpointClass ep) noexcept;
  [[nodiscard]] CircuitBreaker& breaker_for(EndpointClass ep) noexcept;
  [[nodiscard]] const CircuitBreaker& breaker_for(EndpointClass ep) const noexcept;

  // One bucket + one breaker per class. Declared in EndpointClass order so the
  // ctor init-list reads top-to-bottom.
  RateLimiter order_bucket_;
  RateLimiter quote_bucket_;
  RateLimiter historical_bucket_;
  RateLimiter other_bucket_;
  CircuitBreaker order_breaker_;
  CircuitBreaker quote_breaker_;
  CircuitBreaker historical_breaker_;
  CircuitBreaker other_breaker_;
};

}  // namespace broker_exec::ratelimit
