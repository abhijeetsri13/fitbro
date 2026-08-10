#pragma once

// broker_exec::risk — the cadence'd, fail-closed funds/margin view (Story 2.11,
// FR-13).
//
// A margin-sensitive entry must NEVER be let through on stale funds data. This
// view caches the last `ports::FundsSnapshot` together with the steady-clock
// instant it was fetched, and treats it as usable only while its age is within
// the configured cadence. A margin check first ensures freshness (auto-refetch
// when stale); if the view cannot be refreshed it FAILS CLOSED with a
// `DataStale` Error (SuggestedAction::BlockStrategy) — the old snapshot value is
// never reused, not even for a zero requirement. Only once the view is fresh is
// the available margin compared against the requirement (integer paise), with a
// shortfall reported as an `InsufficientFunds` Error.
//
// Time is read through the injected `ports::ClockPort::now_steady()` (monotonic),
// so the cadence is an elapsed-duration test immune to wall-clock jumps. The
// fetch is an injected seam (`std::function`) so the view has no broker/SDK
// dependency: the runtime binds a real `BrokerPort::fetch_funds()` adapter; tests
// bind a controllable lambda.
//
// Conventions: no double/float (margin compared in int64 paise; durations via
// std::chrono); Result<T> is no-throw; no OS APIs, no `#ifdef`.

#include <chrono>
#include <cstdint>
#include <functional>

#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/clock_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::risk {

// A freshness-gated cache over a funds/margin snapshot. NOT thread-safe: it lives
// on, and is driven by, the single main loop (refresh on cadence and after fills;
// queried by the validation gate within one pass).
class FundsView {
 public:
  // Bind the view to a monotonic clock, a fetch seam and a freshness cadence.
  // `clock` MUST outlive the view (it is held by reference, as the injected
  // process clock). `fetch` is invoked to obtain a fresh snapshot; it returns a
  // typed Error on failure (which leaves the view stale). `cadence` is the
  // maximum age (in whole seconds) a snapshot may have and still be used.
  FundsView(const ports::ClockPort& clock,
            std::function<Result<ports::FundsSnapshot>()> fetch,
            std::chrono::seconds cadence)
      : clock_(clock), fetch_(std::move(fetch)), cadence_(cadence) {}

  // Pull a new snapshot through the fetch seam. On success the snapshot and its
  // `fetched_at_` stamp (now_steady) are stored and the view becomes fresh; on
  // failure the Error is returned and NOTHING is updated — the view deliberately
  // stays as stale as it was (freshness never advances on a failed fetch).
  [[nodiscard]] Result<ports::Ok> refresh();

  // True iff a snapshot is held AND its age (now_steady - fetched_at_) is within
  // the cadence. Pure duration arithmetic (steady clock), no float.
  [[nodiscard]] bool is_fresh() const;

  // Force the next freshness check to refetch (used after a fill — AC-2). Cheap
  // and noexcept: it only clears the has-snapshot flag so ensure_fresh() will
  // refetch before answering.
  void invalidate() noexcept { has_snapshot_ = false; }

  // Return a guaranteed-fresh snapshot or fail closed. If not fresh, attempt a
  // single refresh() (its ok/err is ignored, then freshness is re-tested); if it
  // is STILL not fresh, return a DataStale Error (BlockStrategy). The cached
  // snapshot value is NEVER returned while stale (AC-3).
  [[nodiscard]] Result<ports::FundsSnapshot> ensure_fresh();

  // Gate the order's required margin against a FRESH view. ensure_fresh() runs
  // first, so a stale, unrefreshable view fails closed (DataStale) — this
  // dominates and blocks even a zero requirement. Only on a fresh view is the
  // available margin compared: ok() iff available_margin >= required, else an
  // InsufficientFunds Error (DoNotRetry) naming the shortfall.
  [[nodiscard]] Result<ports::Ok> check_margin(std::int64_t required_margin_paise);

  // Adapter: a nullary predicate bound to `required_margin_paise` that calls
  // check_margin, so it drops directly into GateContext::funds_check (Story 2.8).
  // The closure captures THIS view by pointer; the FundsView MUST outlive the
  // closure (both live on the main loop, so the gate pass that holds the closure
  // is bounded by the view's lifetime).
  [[nodiscard]] std::function<Result<ports::Ok>()> make_funds_check(
      std::int64_t required_margin_paise);

 private:
  const ports::ClockPort& clock_;
  std::function<Result<ports::FundsSnapshot>()> fetch_;
  std::chrono::seconds cadence_;
  ports::FundsSnapshot snapshot_{};
  std::chrono::steady_clock::time_point fetched_at_{};
  bool has_snapshot_ = false;
};

}  // namespace broker_exec::risk
