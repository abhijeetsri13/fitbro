#pragma once

// broker_exec::adapters::fake::FakeBroker — an adversarial, in-test broker that
// implements `ports::BrokerPort` and injects the dangerous faults the safety
// core must survive (Story 1.11, FR-37): delayed acks, dropped acks, duplicate
// fills, out-of-order events, 429s (rate-limit), and the headline danger,
// ack-lost-but-placed.
//
// WHY IT EXISTS
//   The zero-duplicate-orders invariant (NFR-3) can only be *proven* against a
//   broker that misbehaves on demand. A real broker cannot be told to drop an
//   ack on the third request at tick 5; this fake can, deterministically, so the
//   conformance kit (Story 1.12) and the SIGKILL durability harness can drive a
//   full fault matrix with reproducible outcomes.
//
// DETERMINISM CONTRACT
//   No real time, no randomness, no OS APIs, no `#ifdef`. Every fault is keyed
//   off (a) explicit `FaultConfig` and (b) the injected `ports::ClockPort` plus
//   internal monotonic counters. Given the same config, the same clock timeline,
//   and the same sequence of calls, the fake produces byte-identical results on
//   every platform and every run.
//
// "BROKER TRUTH" MODEL
//   The fake maintains an internal order book and trade list that represent what
//   the *broker* believes — independent of what the caller observed. The
//   safety-critical case is `ack_lost_but_placed`: `place()` returns an Error
//   (the caller never saw the ack) BUT the order is recorded in the book, so a
//   subsequent `fetch_orders()` reveals it. This is exactly the duplicate-risk
//   the upper layers must reconcile rather than blindly retry.
//
// Cross-platform: C++20 standard library only.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "broker_exec/domain/types.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/clock_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::adapters::fake {

// Declarative description of the faults a single test step wants injected. Every
// field is explicit and independently toggleable; defaults are the benign,
// fault-free broker. Faults compose (e.g. a delayed ack on a rate-limited fake).
struct FaultConfig {
  // place()/modify() "succeed" at the broker (order enters the book) but the ack
  // never returns to the caller -> the call yields a Timeout the caller cannot
  // distinguish from a true failure. The order IS present in fetch_orders().
  // This is the same dangerous shape as ack_lost_but_placed; kept as a distinct
  // toggle for readable test intent.
  bool drop_ack = false;

  // The ack is withheld for the first N ticks after the mutation: the order is
  // placed in the book immediately, but place() returns Timeout until the
  // injected clock has advanced >= N ticks, after which place() returns the ack
  // normally. With N == 0 the ack is immediate (no delay).
  int delay_ack_ticks = 0;

  // THE headline danger: the caller sees a failure (Timeout, action
  // ReconcileFirst) but the order IS placed at the broker. A blind retry here
  // would create a duplicate; the safety core must reconcile against
  // fetch_orders() and find the placed order instead.
  bool ack_lost_but_placed = false;

  // Emit the same fill twice for a placed order: fetch_trades() returns two
  // byte-identical Trade rows (same trade_id, qty, price). Tests the dedup of
  // the trade-application path.
  bool duplicate_fill = false;

  // Deliver status/event updates out of sequence: fetch_orders() returns rows in
  // reverse insertion order, and fetch_trades() reverses too. Exercises the
  // forward-progressing / ordering-key apply logic (a stale view must be
  // dropped, not applied).
  bool out_of_order_events = false;

  // After this many ACCEPTED requests across the whole port surface, every
  // subsequent request returns RateLimited (HTTP-429 shaped). -1 disables it.
  // Counts mutations and queries alike (a real throttle does not discriminate).
  int rate_limit_after = -1;

  // The simulated broker tick. Faults that are "after N ticks" measure elapsed
  // ticks as floor(elapsed_steady / tick_duration) since construction, read from
  // the injected clock. A test advances the TestClock by multiples of this to
  // cross a threshold. Must be > 0 (a non-positive value is treated as 1ns).
  std::chrono::nanoseconds tick_duration{std::chrono::milliseconds{1}};
};

// A single row of the fake's internal broker-truth order book. Exposed via the
// raw-state hooks so a conformance test can assert what the broker "really"
// holds independent of what the caller observed.
struct BookEntry {
  domain::Order order;      // The order as the broker sees it.
  bool ack_returned;        // Did the caller receive the ack for this entry?
  std::int64_t placed_seq;  // Monotonic broker sequence (the ordering key).
};

class FakeBroker final : public ports::BrokerPort {
 public:
  // Construct with the injected clock (drives tick-based faults) and an initial
  // fault config. The clock outlives the broker (reference semantics, matching
  // the rest of the codebase's ClockPort injection).
  explicit FakeBroker(ports::ClockPort& clock, FaultConfig cfg = {});

  // ── BrokerPort surface (honor the configured faults deterministically) ──
  [[nodiscard]] Result<ports::BrokerAck> place(const domain::OrderIntent& intent) override;
  [[nodiscard]] Result<ports::BrokerAck> modify(const std::string& broker_order_id,
                                                const domain::OrderIntent& intent) override;
  [[nodiscard]] Result<ports::Ok> cancel(const std::string& broker_order_id) override;
  [[nodiscard]] Result<ports::Ok> square_off(const std::string& broker_order_id) override;

  [[nodiscard]] Result<std::vector<domain::Order>> fetch_orders() override;
  [[nodiscard]] Result<std::vector<domain::Trade>> fetch_trades() override;
  [[nodiscard]] Result<std::vector<domain::Position>> fetch_positions() override;
  [[nodiscard]] Result<ports::FundsSnapshot> fetch_funds() override;

  // ── Test / conformance hooks ──
  // Reconfigure the injected faults between steps. Counters (request count,
  // broker sequence) are preserved so a multi-step scenario stays continuous.
  void set_fault(const FaultConfig& cfg) noexcept { cfg_ = cfg; }
  [[nodiscard]] const FaultConfig& fault() const noexcept { return cfg_; }

  // Seed available funds the fake reports from fetch_funds() (default zero).
  void set_funds(ports::FundsSnapshot funds) noexcept { funds_ = funds; }

  // Raw broker-truth inspection (independent of caller-observed acks): the full
  // book including ack-lost-but-placed entries, and the count of accepted
  // requests so far (the rate-limit denominator).
  [[nodiscard]] const std::vector<BookEntry>& book() const noexcept { return book_; }
  [[nodiscard]] std::size_t request_count() const noexcept { return request_count_; }

 private:
  // Ticks elapsed on the injected steady clock since construction.
  [[nodiscard]] std::int64_t elapsed_ticks() const noexcept;

  // Account a request against the rate-limit budget. Returns true if this request
  // must be rejected as RateLimited (and does NOT count a rejected request).
  [[nodiscard]] bool rate_limited() noexcept;

  // Locate a book entry by broker order id; nullptr if absent.
  [[nodiscard]] BookEntry* find(const std::string& broker_order_id) noexcept;

  // Mark a freshly-placed book entry as filled and append its fill to the trade
  // list — emitting the fill twice when duplicate_fill is configured.
  void maybe_emit_fill(BookEntry& entry);

  ports::ClockPort& clock_;
  FaultConfig cfg_;
  ports::FundsSnapshot funds_{};

  std::vector<BookEntry> book_;
  std::vector<domain::Trade> trades_;
  std::chrono::steady_clock::time_point start_steady_;
  std::int64_t next_broker_seq_ = 1;  // Monotonic order id / ordering key source.
  std::int64_t next_trade_seq_ = 1;   // Monotonic trade id source.
  std::size_t request_count_ = 0;     // Accepted requests (rate-limit denominator).
};

}  // namespace broker_exec::adapters::fake
