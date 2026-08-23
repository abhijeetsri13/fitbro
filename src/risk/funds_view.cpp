#include "broker_exec/risk/funds_view.hpp"

#include <chrono>
#include <string>
#include <utility>

#include "broker_exec/errors/error.hpp"

namespace broker_exec::risk {

namespace {

using errors::Error;
using errors::ErrorCategory;
using errors::make_error;
using errors::SuggestedAction;

}  // namespace

Result<ports::Ok> FundsView::refresh() {
  auto fetched = fetch_();
  if (!fetched) {
    // Failed fetch: propagate the Error and DO NOT touch the snapshot or its
    // stamp — the view stays exactly as stale as it was. Freshness must never
    // advance on a failed fetch (AC-1).
    return fail(std::move(fetched.error()));
  }
  snapshot_ = std::move(fetched.value());
  fetched_at_ = clock_.now_steady();
  has_snapshot_ = true;
  return ports::ok();
}

bool FundsView::is_fresh() const {
  if (!has_snapshot_) {
    return false;
  }
  // Pure steady-clock duration arithmetic, no float: compare the elapsed age
  // against the cadence after casting it to the steady clock's duration. Age is
  // non-negative (now_steady is monotonic) and "within cadence" is inclusive.
  const std::chrono::steady_clock::duration age = clock_.now_steady() - fetched_at_;
  return age <= std::chrono::duration_cast<std::chrono::steady_clock::duration>(cadence_);
}

Result<ports::FundsSnapshot> FundsView::ensure_fresh() {
  std::string root_cause;
  if (!is_fresh()) {
    // One refetch attempt; what matters for freshness is the state AFTERWARDS (a
    // failed refresh leaves it stale and we fall through to fail closed). But
    // capture a failed refresh's Error so the operator sees the root cause
    // (Network/RateLimited/broker_code) instead of a bare "stale" verdict.
    if (auto refreshed = refresh(); !refreshed) {
      const Error& inner = refreshed.error();
      root_cause = inner.broker_code.empty() ? inner.message
                                             : inner.message + " [" + inner.broker_code + "]";
    }
  }
  if (!is_fresh()) {
    // Fail closed: the snapshot value is NEVER returned while stale (AC-3). The
    // verdict MUST stay category=DataStale / action=BlockStrategy (stale =>
    // block); the inner Error's category/action never leak — only its message
    // and broker_code are threaded into the message for diagnosis.
    std::string message = "funds view is stale and could not be refreshed";
    if (!root_cause.empty()) {
      message += ": " + root_cause;
    }
    return fail(make_error(ErrorCategory::DataStale, message));
  }
  return snapshot_;
}

Result<ports::Ok> FundsView::check_margin(std::int64_t required_margin_paise) {
  // Freshness first: a stale, unrefreshable view fails closed and that DataStale
  // verdict DOMINATES — it blocks even a zero requirement (AC-3).
  auto f = ensure_fresh();
  if (!f) {
    return fail(std::move(f.error()));
  }

  const std::int64_t available = f.value().available_margin.paise();
  if (available >= required_margin_paise) {
    return ports::ok();
  }

  // Shortfall on a FRESH view: an InsufficientFunds Error, do-not-retry (fix the
  // sizing, don't repeat the request). Available vs required paise are not
  // secrets, so naming the shortfall is safe and aids diagnosis.
  Error e = make_error(ErrorCategory::InsufficientFunds,
                       "insufficient margin: available " + std::to_string(available) +
                           " paise < required " + std::to_string(required_margin_paise) + " paise");
  e.action = SuggestedAction::DoNotRetry;
  return fail(std::move(e));
}

std::function<Result<ports::Ok>()> FundsView::make_funds_check(std::int64_t required_margin_paise) {
  // Capture THIS view by pointer plus the requirement by value. The view must
  // outlive the closure (documented in the header); both live on the main loop,
  // so the gate pass holding the closure is bounded by the view's lifetime.
  return [self = this, required_margin_paise]() -> Result<ports::Ok> {
    return self->check_margin(required_margin_paise);
  };
}

}  // namespace broker_exec::risk
