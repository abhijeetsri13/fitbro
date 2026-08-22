#include "broker_exec/runtime/dispatcher.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "broker_exec/adapters/fake/fake_broker.hpp"
#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/idempotency/idempotency.hpp"
#include "broker_exec/idempotency/uuid.hpp"
#include "broker_exec/intentlog/intent_log.hpp"
#include "broker_exec/lifecycle/lifecycle.hpp"
#include "broker_exec/store/store.hpp"

namespace fs = std::filesystem;

using broker_exec::adapters::fake::FakeBroker;
using broker_exec::adapters::fake::FaultConfig;
using broker_exec::clock::TestClock;
using broker_exec::domain::Order;
using broker_exec::domain::OrderIntent;
using broker_exec::domain::OrderState;
using broker_exec::domain::OrderType;
using broker_exec::domain::Price;
using broker_exec::domain::Product;
using broker_exec::domain::Quantity;
using broker_exec::domain::Side;
using broker_exec::errors::ErrorCategory;
using broker_exec::intentlog::IntentLog;
using broker_exec::intentlog::IntentOp;
using broker_exec::intentlog::IntentRecord;
using broker_exec::lifecycle::LifecycleEngine;
using broker_exec::runtime::Dispatcher;
using broker_exec::store::Store;

namespace idem = broker_exec::idempotency;

namespace {

// A representative intent the tests reuse. client_ref is intentionally empty —
// the dispatcher mints + stamps it via reserve().
OrderIntent sample_intent() {
  OrderIntent intent;
  intent.symbol = "NIFTY24JUN24000CE";
  intent.side = Side::Sell;
  intent.quantity = Quantity::of(50);
  intent.price = Price::from_rupees(123, 50);
  intent.order_type = OrderType::Limit;
  intent.product = Product::Intraday;
  intent.strategy = "alpha";
  return intent;
}

// A unique temp intent-log path per test, removed on scope exit.
struct TempLog {
  fs::path path;
  explicit TempLog(const std::string& tag)
      : path(fs::temp_directory_path() /
             ("broker_exec_runtime_" + tag + "_" +
              std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".log")) {
    std::error_code ec;
    fs::remove(path, ec);
  }
  ~TempLog() {
    std::error_code ec;
    fs::remove(path, ec);
  }
  TempLog(const TempLog&) = delete;
  TempLog& operator=(const TempLog&) = delete;
};

// Read the whole intent-log file as raw bytes via a fresh read-only stream. Safe
// to call while the IntentLog's append handle is still open (a read-only
// std::ifstream coexists with the append handle on all platforms).
std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// The fixture bundles the collaborators a Dispatcher needs, all on the stack and
// reset()-free (a fresh in-memory Store + temp IntentLog per test).
struct Harness {
  TempLog tmp;
  TestClock clock;
  FakeBroker broker;
  IntentLog log;
  Store store;
  idem::IdempotencyIndex index;
  idem::SeededUuidGenerator uuids;
  LifecycleEngine fsm;
  Dispatcher dispatcher;

  explicit Harness(const std::string& tag, FaultConfig cfg = {})
      : tmp(tag),
        clock(std::chrono::steady_clock::time_point{}, std::chrono::system_clock::time_point{}),
        broker(clock, cfg),
        log(open_log()),
        store(open_store()),
        uuids(0xC0FFEE),
        dispatcher(broker, log, store, index, uuids, fsm, clock) {}

 private:
  IntentLog open_log() {
    auto opened = IntentLog::open(tmp.path, clock);
    REQUIRE(opened.has_value());
    return std::move(opened.value());
  }
  Store open_store() {
    auto opened = Store::open(":memory:");
    REQUIRE(opened.has_value());
    return std::move(opened.value());
  }
};

}  // namespace

// ── (a) happy place ──────────────────────────────────────────────────────────
TEST_CASE("place: happy path records PlaceOrder then Result, one order, acknowledged",
          "[runtime][dispatch][place]") {
  Harness h("happy");
  const OrderIntent intent = sample_intent();

  auto result = h.dispatcher.place(intent.strategy, intent);
  REQUIRE(result.has_value());
  const Order& order = result.value();

  // The order is acknowledged at the broker and carries the broker id + the
  // reserved client_ref.
  CHECK(order.state == OrderState::Acknowledged);
  CHECK_FALSE(order.broker_order_id.empty());
  CHECK_FALSE(order.intent.client_ref.empty());

  // The broker was sent exactly one mutation.
  CHECK(h.broker.request_count() == 1);
  CHECK(h.broker.book().size() == 1);

  // Exactly one order in the projection, matching the returned order.
  auto all = h.store.all_orders();
  REQUIRE(all.has_value());
  REQUIRE(all.value().size() == 1);
  CHECK(all.value().front().intent.client_ref == order.intent.client_ref);
  CHECK(all.value().front().state == OrderState::Acknowledged);

  // The intent log holds PlaceOrder THEN Result, in that order, for this ref.
  auto replayed = h.log.replay();
  REQUIRE(replayed.has_value());
  const std::vector<IntentRecord>& recs = replayed.value();
  REQUIRE(recs.size() == 2);
  CHECK(recs[0].op == IntentOp::PlaceOrder);
  CHECK(recs[0].client_ref == order.intent.client_ref);
  CHECK(recs[1].op == IntentOp::Result);
  CHECK(recs[1].client_ref == order.intent.client_ref);
  // The PlaceOrder payload is the canonical idempotency payload (restart-dedup).
  CHECK(recs[0].payload_json == idem::intent_payload_json(order.intent));
}

// ── (b) fsync-before-send ────────────────────────────────────────────────────
TEST_CASE("place: the PlaceOrder intent is durable on disk BEFORE the broker send",
          "[runtime][dispatch][durability]") {
  Harness h("barrier");
  const OrderIntent intent = sample_intent();

  // The reserved client_ref is what the barrier looks for. Reproduce the mint
  // deterministically so we know exactly what to find on disk.
  const std::string sig = idem::signal_signature(intent);
  idem::SeededUuidGenerator probe(0xC0FFEE);  // same seed as the harness uuids
  const std::string expected_ref = idem::make_client_ref(intent.strategy, sig, probe.next());

  bool barrier_ran = false;
  // The barrier runs AFTER the fsync'd append and BEFORE broker.place(). At this
  // instant: the broker must not have been touched yet, and the intent record
  // must already be on disk.
  h.dispatcher.set_pre_send_barrier([&] {
    barrier_ran = true;
    CHECK(h.broker.request_count() == 0);  // send has NOT happened yet
    CHECK(h.broker.book().empty());

    const std::string bytes = read_file(h.tmp.path);
    REQUIRE_FALSE(bytes.empty());
    // The durable record is present: its client_ref and the place_order op name.
    CHECK(bytes.find(expected_ref) != std::string::npos);
    CHECK(bytes.find("place_order") != std::string::npos);
  });

  auto result = h.dispatcher.place(intent.strategy, intent);
  REQUIRE(result.has_value());
  CHECK(barrier_ran);
  CHECK(result.value().intent.client_ref == expected_ref);
  // After the call the broker WAS sent (exactly once).
  CHECK(h.broker.request_count() == 1);
}

// ── (c) idempotency: place twice -> one order, zero second-send ───────────────
TEST_CASE("place: duplicate signal returns the existing order with ZERO broker sends",
          "[runtime][dispatch][idempotency]") {
  Harness h("dedup");
  const OrderIntent intent = sample_intent();

  auto first = h.dispatcher.place(intent.strategy, intent);
  REQUIRE(first.has_value());
  const std::string ref = first.value().intent.client_ref;
  REQUIRE(h.broker.request_count() == 1);

  // Second place of the SAME signal: no new broker send, returns the existing.
  auto second = h.dispatcher.place(intent.strategy, intent);
  REQUIRE(second.has_value());
  CHECK(second.value().intent.client_ref == ref);
  // The headline assertion: ZERO additional broker calls.
  CHECK(h.broker.request_count() == 1);
  CHECK(h.broker.book().size() == 1);

  // Still exactly one order, and no extra PlaceOrder appended.
  auto all = h.store.all_orders();
  REQUIRE(all.has_value());
  CHECK(all.value().size() == 1);

  auto replayed = h.log.replay();
  REQUIRE(replayed.has_value());
  int place_count = 0;
  for (const auto& r : replayed.value()) {
    if (r.op == IntentOp::PlaceOrder) {
      ++place_count;
    }
  }
  CHECK(place_count == 1);
}

// ── (d) no-blind-retry: ack-lost-but-placed -> Unknown, sent exactly once ─────
TEST_CASE("place: a lost ack marks the order UNKNOWN, sends exactly once, never retries",
          "[runtime][dispatch][no-blind-retry]") {
  FaultConfig cfg;
  cfg.ack_lost_but_placed = true;  // place() errors Timeout/ReconcileFirst, but IS placed
  Harness h("unknown", cfg);
  const OrderIntent intent = sample_intent();

  auto result = h.dispatcher.place(intent.strategy, intent);
  // The dispatcher returns the Unknown order (not an Error) — durably recorded.
  REQUIRE(result.has_value());
  const Order& order = result.value();
  CHECK(order.state == OrderState::Unknown);

  // Broker.place() was called EXACTLY ONCE — no blind repeat of a dangerous op.
  CHECK(h.broker.request_count() == 1);

  // The order is persisted Unknown.
  auto found = h.store.find_order(order.intent.client_ref);
  REQUIRE(found.has_value());
  REQUIRE(found.value().has_value());
  CHECK(found.value()->state == OrderState::Unknown);

  // Reconciliation against broker truth shows the order IS at the broker — proof
  // that a blind retry would have created a duplicate (the safety core must not
  // worsen this; it reconciles instead).
  auto orders = h.broker.fetch_orders();
  REQUIRE(orders.has_value());
  REQUIRE(orders.value().size() == 1);
  CHECK(orders.value().front().intent.client_ref == order.intent.client_ref);

  // The intent log holds PlaceOrder then a Result(unknown), and only ONE
  // PlaceOrder (the no-retry guarantee at the log level too).
  auto replayed = h.log.replay();
  REQUIRE(replayed.has_value());
  int place_count = 0;
  bool saw_result = false;
  for (const auto& r : replayed.value()) {
    if (r.op == IntentOp::PlaceOrder) {
      ++place_count;
    }
    if (r.op == IntentOp::Result) {
      saw_result = true;
    }
  }
  CHECK(place_count == 1);
  CHECK(saw_result);
}

TEST_CASE("place: a clean (non-reconcile) rejection yields a Rejected order, never placed",
          "[runtime][dispatch][reject]") {
  // rate_limit_after=0 -> the very first request is RateLimited. RateLimited's
  // action is RetrySafe (NOT ReconcileFirst), and nothing reached the broker book,
  // so this is a clean non-ambiguous outcome: the order is Rejected, not Unknown.
  FaultConfig cfg;
  cfg.rate_limit_after = 0;
  Harness h("reject", cfg);
  const OrderIntent intent = sample_intent();

  auto result = h.dispatcher.place(intent.strategy, intent);
  REQUIRE(result.has_value());
  // A rate-limit is not a reconcile-first ambiguity, so the order is Rejected, not
  // Unknown — and the broker book stays empty (nothing reached it).
  CHECK(result.value().state == OrderState::Rejected);
  CHECK(h.broker.book().empty());
}

// ── (e) cancel / square_off basic + UNKNOWN-on-timeout ───────────────────────
TEST_CASE("cancel: happy path records CancelOrder then Result and returns Ok",
          "[runtime][dispatch][cancel]") {
  Harness h("cancel_ok");
  const OrderIntent intent = sample_intent();

  auto placed = h.dispatcher.place(intent.strategy, intent);
  REQUIRE(placed.has_value());
  const std::string ref = placed.value().intent.client_ref;
  const std::string broker_id = placed.value().broker_order_id;

  auto cancelled = h.dispatcher.cancel(broker_id, ref);
  REQUIRE(cancelled.has_value());

  auto replayed = h.log.replay();
  REQUIRE(replayed.has_value());
  bool saw_cancel_intent = false;
  for (const auto& r : replayed.value()) {
    if (r.op == IntentOp::CancelOrder && r.client_ref == ref) {
      saw_cancel_intent = true;
    }
  }
  CHECK(saw_cancel_intent);
}

TEST_CASE("cancel: a lost ack marks the local order UNKNOWN, surfaces the error, no retry",
          "[runtime][dispatch][cancel][no-blind-retry]") {
  Harness h("cancel_unknown");
  const OrderIntent intent = sample_intent();

  auto placed = h.dispatcher.place(intent.strategy, intent);
  REQUIRE(placed.has_value());
  const std::string ref = placed.value().intent.client_ref;
  const std::string broker_id = placed.value().broker_order_id;
  const std::size_t before = h.broker.request_count();

  // Flip the broker to ack-lost-but-placed for the cancel.
  FaultConfig cfg;
  cfg.ack_lost_but_placed = true;
  h.broker.set_fault(cfg);

  auto cancelled = h.dispatcher.cancel(broker_id, ref);
  // The dangerous cancel surfaces the typed error (ReconcileFirst) to the caller.
  REQUIRE_FALSE(cancelled.has_value());
  CHECK(cancelled.error().category == ErrorCategory::Timeout);

  // Cancel was attempted exactly once (no blind retry of a dangerous op).
  CHECK(h.broker.request_count() == before + 1);

  // The local order is now Unknown (reconcile against broker truth resolves it).
  auto found = h.store.find_order(ref);
  REQUIRE(found.has_value());
  REQUIRE(found.value().has_value());
  CHECK(found.value()->state == OrderState::Unknown);
}

TEST_CASE("square_off: happy path returns Ok and records the intent; lost ack -> UNKNOWN",
          "[runtime][dispatch][square_off]") {
  Harness h("squareoff");
  const OrderIntent intent = sample_intent();

  auto placed = h.dispatcher.place(intent.strategy, intent);
  REQUIRE(placed.has_value());
  const std::string ref = placed.value().intent.client_ref;
  const std::string broker_id = placed.value().broker_order_id;

  // Happy square-off.
  auto ok = h.dispatcher.square_off(broker_id, ref);
  REQUIRE(ok.has_value());

  auto replayed = h.log.replay();
  REQUIRE(replayed.has_value());
  bool saw_squareoff = false;
  for (const auto& r : replayed.value()) {
    if (r.op == IntentOp::SquareOff && r.client_ref == ref) {
      saw_squareoff = true;
    }
  }
  CHECK(saw_squareoff);

  // Now a lost-ack square-off on a second order marks it Unknown.
  OrderIntent other = sample_intent();
  other.quantity = Quantity::of(75);  // a different signal -> a second order
  auto placed2 = h.dispatcher.place(other.strategy, other);
  REQUIRE(placed2.has_value());
  const std::string ref2 = placed2.value().intent.client_ref;
  const std::string id2 = placed2.value().broker_order_id;

  FaultConfig cfg;
  cfg.ack_lost_but_placed = true;
  h.broker.set_fault(cfg);

  auto so = h.dispatcher.square_off(id2, ref2);
  REQUIRE_FALSE(so.has_value());
  CHECK(so.error().category == ErrorCategory::Timeout);

  auto found = h.store.find_order(ref2);
  REQUIRE(found.has_value());
  REQUIRE(found.value().has_value());
  CHECK(found.value()->state == OrderState::Unknown);
}
