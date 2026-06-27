#include "broker_exec/adapters/fake/fake_broker.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::adapters::fake {

namespace {

// A short, redaction-safe broker code shared by the fault-injected errors so a
// classifier/test can correlate them without exposing anything token-shaped.
constexpr const char* kRateLimitCode = "FAKE-429";
constexpr const char* kLostAckCode = "FAKE-LOSTACK";
constexpr const char* kDelayCode = "FAKE-DELAY";
constexpr const char* kDropCode = "FAKE-DROP";

// Build the broker order id for a given monotonic sequence. Stable + parseable.
[[nodiscard]] std::string make_order_id(std::int64_t seq) {
  return "FAKE-ORD-" + std::to_string(seq);
}

}  // namespace

FakeBroker::FakeBroker(ports::ClockPort& clock, FaultConfig cfg)
    : clock_(clock), cfg_(std::move(cfg)), start_steady_(clock.now_steady()) {}

std::int64_t FakeBroker::elapsed_ticks() const noexcept {
  // Work entirely in nanoseconds so both operands share one signed integer rep
  // regardless of the platform's steady_clock period (no narrowing under /W4).
  const auto tick = static_cast<std::int64_t>(
      cfg_.tick_duration.count() > 0 ? cfg_.tick_duration.count() : 1);
  const auto elapsed = static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(clock_.now_steady() - start_steady_)
          .count());
  // Monotonic clock never goes backward, so elapsed is non-negative; clamp
  // defensively to keep the result well-defined regardless.
  return elapsed > 0 ? elapsed / tick : 0;
}

bool FakeBroker::rate_limited() noexcept {
  // `request_count_` is the total number of ACCEPTED broker requests (every
  // place/modify/cancel/square_off/fetch that reaches the broker), which callers
  // use to prove no-blind-retry / zero-send-on-duplicate. A throttled request is
  // rejected before reaching the broker and therefore does NOT count (matching a
  // real token-bucket reject).
  if (cfg_.rate_limit_after >= 0 &&
      request_count_ >= static_cast<std::size_t>(cfg_.rate_limit_after)) {
    return true;  // throttled: not counted
  }
  ++request_count_;  // accepted: counted on every code path (rate limiting on or off)
  return false;
}

BookEntry* FakeBroker::find(const std::string& broker_order_id) noexcept {
  const auto it = std::find_if(book_.begin(), book_.end(), [&](const BookEntry& e) {
    return e.order.broker_order_id == broker_order_id;
  });
  return it == book_.end() ? nullptr : &*it;
}

Result<ports::BrokerAck> FakeBroker::place(const domain::OrderIntent& intent) {
  if (rate_limited()) {
    return fail(errors::make_error(errors::ErrorCategory::RateLimited, "rate limited by fake broker",
                                   kRateLimitCode));
  }

  // The order ALWAYS enters the broker-truth book — this is what makes the
  // ack-lost / dropped / delayed faults dangerous and reconcilable. What varies
  // is only whether (and when) the CALLER observes the ack.
  const std::int64_t seq = next_broker_seq_++;
  const std::string broker_id = make_order_id(seq);

  domain::Order order;
  order.intent = intent;
  order.state = domain::OrderState::Acknowledged;
  order.broker_order_id = broker_id;

  const ports::BrokerAck ack{broker_id, intent.client_ref};

  // ack_lost_but_placed / drop_ack: the order is placed, but the caller gets a
  // Timeout it cannot distinguish from a real failure. The order stays in the
  // book with ack_returned=false so fetch_orders() reveals it on reconcile.
  if (cfg_.ack_lost_but_placed) {
    book_.push_back(BookEntry{order, /*ack_returned=*/false, seq});
    maybe_emit_fill(book_.back());
    return fail(errors::make_error(errors::ErrorCategory::Timeout,
                                   "ack lost but order placed at broker", kLostAckCode));
  }
  if (cfg_.drop_ack) {
    book_.push_back(BookEntry{order, /*ack_returned=*/false, seq});
    maybe_emit_fill(book_.back());
    return fail(errors::make_error(errors::ErrorCategory::Timeout, "ack dropped by fake broker",
                                   kDropCode));
  }

  // delay_ack_ticks: the order is placed now, but the ack is withheld until the
  // injected clock has advanced >= N ticks since construction. Until then the
  // caller sees a Timeout; the order is already visible to fetch_orders().
  if (cfg_.delay_ack_ticks > 0 && elapsed_ticks() < cfg_.delay_ack_ticks) {
    book_.push_back(BookEntry{order, /*ack_returned=*/false, seq});
    maybe_emit_fill(book_.back());
    return fail(errors::make_error(errors::ErrorCategory::Timeout, "ack delayed by fake broker",
                                   kDelayCode));
  }

  // Happy path: order placed and the caller observes the ack.
  book_.push_back(BookEntry{order, /*ack_returned=*/true, seq});
  maybe_emit_fill(book_.back());
  return ack;
}

Result<ports::BrokerAck> FakeBroker::modify(const std::string& broker_order_id,
                                            const domain::OrderIntent& intent) {
  if (rate_limited()) {
    return fail(errors::make_error(errors::ErrorCategory::RateLimited, "rate limited by fake broker",
                                   kRateLimitCode));
  }

  BookEntry* entry = find(broker_order_id);
  if (entry == nullptr) {
    return fail(errors::make_error(errors::ErrorCategory::OrderNotFound,
                                   "modify on unknown order at fake broker"));
  }

  // The modify is applied to broker truth regardless; ack visibility follows the
  // same delayed/dropped/lost rules as place().
  entry->order.intent = intent;

  const ports::BrokerAck ack{broker_order_id, intent.client_ref};
  if (cfg_.ack_lost_but_placed) {
    entry->ack_returned = false;
    return fail(errors::make_error(errors::ErrorCategory::Timeout,
                                   "ack lost but modify applied at broker", kLostAckCode));
  }
  if (cfg_.drop_ack) {
    entry->ack_returned = false;
    return fail(errors::make_error(errors::ErrorCategory::Timeout, "ack dropped by fake broker",
                                   kDropCode));
  }
  if (cfg_.delay_ack_ticks > 0 && elapsed_ticks() < cfg_.delay_ack_ticks) {
    entry->ack_returned = false;
    return fail(errors::make_error(errors::ErrorCategory::Timeout, "ack delayed by fake broker",
                                   kDelayCode));
  }
  entry->ack_returned = true;
  return ack;
}

Result<ports::Ok> FakeBroker::cancel(const std::string& broker_order_id) {
  if (rate_limited()) {
    return fail(errors::make_error(errors::ErrorCategory::RateLimited, "rate limited by fake broker",
                                   kRateLimitCode));
  }
  BookEntry* entry = find(broker_order_id);
  if (entry == nullptr) {
    return fail(errors::make_error(errors::ErrorCategory::OrderNotFound,
                                   "cancel on unknown order at fake broker"));
  }
  // The cancel is recorded as broker truth even when the ack is withheld — the
  // order IS cancelled, the caller just may not learn it (reconcile finds it).
  entry->order.state = domain::OrderState::Cancelled;
  if (cfg_.ack_lost_but_placed || cfg_.drop_ack) {
    entry->ack_returned = false;
    return fail(errors::make_error(errors::ErrorCategory::Timeout,
                                   "ack lost but cancel applied at broker", kLostAckCode));
  }
  if (cfg_.delay_ack_ticks > 0 && elapsed_ticks() < cfg_.delay_ack_ticks) {
    entry->ack_returned = false;
    return fail(errors::make_error(errors::ErrorCategory::Timeout, "ack delayed by fake broker",
                                   kDelayCode));
  }
  entry->ack_returned = true;
  return ports::ok();
}

Result<ports::Ok> FakeBroker::square_off(const std::string& broker_order_id) {
  if (rate_limited()) {
    return fail(errors::make_error(errors::ErrorCategory::RateLimited, "rate limited by fake broker",
                                   kRateLimitCode));
  }
  BookEntry* entry = find(broker_order_id);
  if (entry == nullptr) {
    return fail(errors::make_error(errors::ErrorCategory::OrderNotFound,
                                   "square_off on unknown order at fake broker"));
  }
  entry->order.state = domain::OrderState::Cancelled;
  if (cfg_.ack_lost_but_placed || cfg_.drop_ack) {
    entry->ack_returned = false;
    return fail(errors::make_error(errors::ErrorCategory::Timeout,
                                   "ack lost but square_off applied at broker", kLostAckCode));
  }
  if (cfg_.delay_ack_ticks > 0 && elapsed_ticks() < cfg_.delay_ack_ticks) {
    entry->ack_returned = false;
    return fail(errors::make_error(errors::ErrorCategory::Timeout, "ack delayed by fake broker",
                                   kDelayCode));
  }
  entry->ack_returned = true;
  return ports::ok();
}

Result<std::vector<domain::Order>> FakeBroker::fetch_orders() {
  if (rate_limited()) {
    return fail(errors::make_error(errors::ErrorCategory::RateLimited, "rate limited by fake broker",
                                   kRateLimitCode));
  }
  // Reads return BROKER TRUTH: every order in the book, INCLUDING orders whose
  // ack was lost/dropped/delayed (ack_returned==false). This is what lets the
  // safety core reconcile an ambiguous mutation against the broker.
  std::vector<domain::Order> out;
  out.reserve(book_.size());
  for (const BookEntry& e : book_) {
    out.push_back(e.order);
  }
  if (cfg_.out_of_order_events) {
    std::reverse(out.begin(), out.end());
  }
  return out;
}

Result<std::vector<domain::Trade>> FakeBroker::fetch_trades() {
  if (rate_limited()) {
    return fail(errors::make_error(errors::ErrorCategory::RateLimited, "rate limited by fake broker",
                                   kRateLimitCode));
  }
  std::vector<domain::Trade> out = trades_;  // copy of broker-truth fills
  if (cfg_.out_of_order_events) {
    std::reverse(out.begin(), out.end());
  }
  return out;
}

Result<std::vector<domain::Position>> FakeBroker::fetch_positions() {
  if (rate_limited()) {
    return fail(errors::make_error(errors::ErrorCategory::RateLimited, "rate limited by fake broker",
                                   kRateLimitCode));
  }
  // Positions are derived from the filled trades: net signed quantity per symbol.
  // Kept simple and deterministic; the safety core reconciles against this view.
  std::vector<domain::Position> out;
  for (const BookEntry& e : book_) {
    if (e.order.state != domain::OrderState::Filled &&
        e.order.state != domain::OrderState::PartiallyFilled) {
      continue;
    }
    const domain::OrderIntent& intent = e.order.intent;
    const std::int64_t signed_qty = intent.side == domain::Side::Buy
                                        ? e.order.filled_qty.value()
                                        : -e.order.filled_qty.value();
    const auto pos_it = std::find_if(out.begin(), out.end(), [&](const domain::Position& p) {
      return p.symbol == intent.symbol;
    });
    if (pos_it == out.end()) {
      domain::Position p;
      p.symbol = intent.symbol;
      p.net_qty = domain::Quantity::of(signed_qty);
      p.avg_price = e.order.avg_price;
      out.push_back(p);
    } else {
      pos_it->net_qty = domain::Quantity::of(pos_it->net_qty.value() + signed_qty);
    }
  }
  return out;
}

Result<ports::FundsSnapshot> FakeBroker::fetch_funds() {
  if (rate_limited()) {
    return fail(errors::make_error(errors::ErrorCategory::RateLimited, "rate limited by fake broker",
                                   kRateLimitCode));
  }
  return funds_;
}

void FakeBroker::maybe_emit_fill(BookEntry& entry) {
  // A placed order is treated as immediately, fully filled at its intent price
  // (the fake's deterministic execution model). The fault we care about for the
  // trade path is duplicate_fill: emit the SAME fill twice so the dedup logic in
  // the trade-application path is exercised with byte-identical rows.
  entry.order.state = domain::OrderState::Filled;
  entry.order.filled_qty = entry.order.intent.quantity;
  entry.order.avg_price = entry.order.intent.price;

  const std::int64_t trade_seq = next_trade_seq_++;
  domain::Trade trade;
  trade.trade_id = "FAKE-TRD-" + std::to_string(trade_seq);
  trade.client_ref = entry.order.intent.client_ref;
  trade.broker_order_id = entry.order.broker_order_id;
  trade.quantity = entry.order.intent.quantity;
  trade.price = entry.order.intent.price;

  trades_.push_back(trade);
  if (cfg_.duplicate_fill) {
    // Identical row (same trade_id) — the broker double-reported the fill.
    trades_.push_back(trade);
  }
}

}  // namespace broker_exec::adapters::fake
