#include "broker_exec/runtime/unknown_resolver.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "broker_exec/adapters/fake/fake_broker.hpp"
#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/lifecycle/lifecycle.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/store/store.hpp"

using broker_exec::adapters::fake::FakeBroker;
using broker_exec::clock::TestClock;
using broker_exec::domain::Order;
using broker_exec::domain::OrderIntent;
using broker_exec::domain::OrderState;
using broker_exec::domain::OrderType;
using broker_exec::domain::Price;
using broker_exec::domain::Product;
using broker_exec::domain::Quantity;
using broker_exec::domain::Side;
using broker_exec::lifecycle::LifecycleEngine;
using broker_exec::ports::AlertLevel;
using broker_exec::ports::AlertSink;
using broker_exec::runtime::MatchKind;
using broker_exec::runtime::UnknownResolution;
using broker_exec::runtime::UnknownResolver;
using broker_exec::store::Store;

namespace bx = broker_exec;

namespace {

// An in-test AlertSink that records every alert it is asked to deliver, so a test
// can assert that (and only that) a fail-closed NoMatch escalates.
class RecordingAlertSink final : public AlertSink {
 public:
  struct Entry {
    AlertLevel level;
    std::string message;
    bx::ports::AlertContext provenance;  // IMP-16: the TYPED ids that rode alongside
  };

  bx::Result<bx::ports::Ok> send(AlertLevel level, const std::string& message) override {
    entries_.push_back({level, message, bx::ports::AlertContext{}});
    return bx::ports::ok();
  }
  // IMP-16: overriding send_with_context (rather than inheriting the base
  // default, which drops the context) is what lets a test assert that the
  // fail-closed Critical alert NAMES THE ORDER.
  bx::Result<bx::ports::Ok> send_with_context(AlertLevel level, const std::string& message,
                                              const bx::ports::AlertContext& provenance) override {
    entries_.push_back({level, message, provenance});
    return bx::ports::ok();
  }
  bx::Result<bx::ports::Ok> send_test_alert() override { return bx::ports::ok(); }

  [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }
  [[nodiscard]] std::size_t count() const noexcept { return entries_.size(); }

 private:
  std::vector<Entry> entries_;
};

OrderIntent sample_intent(std::string client_ref) {
  OrderIntent intent;
  intent.client_ref = std::move(client_ref);
  intent.symbol = "NIFTY24JUN24000CE";
  intent.side = Side::Sell;
  intent.quantity = Quantity::of(50);
  intent.price = Price::from_rupees(123, 50);
  intent.order_type = OrderType::Limit;
  intent.product = Product::Intraday;
  intent.strategy = "alpha";
  return intent;
}

// A local order in OrderState::Unknown (the dispatcher's posture for an ambiguous
// in-flight order). `broker_order_id` may be empty (id-less correlation/attribute
// scenarios).
Order unknown_order(const std::string& client_ref, const std::string& broker_order_id = {}) {
  Order order;
  order.intent = sample_intent(client_ref);
  order.state = OrderState::Unknown;
  order.broker_order_id = broker_order_id;
  return order;
}

// The collaborators a resolver needs, all on the stack, with a ":memory:" store.
struct Harness {
  TestClock clock;
  FakeBroker broker;
  Store store;
  LifecycleEngine fsm;
  RecordingAlertSink alerts;
  UnknownResolver resolver;

  Harness()
      : clock(std::chrono::steady_clock::time_point{}, std::chrono::system_clock::time_point{}),
        broker(clock),
        store(open_store()),
        resolver(broker, store, fsm, alerts, clock) {}

  // Place an order at the broker (broker truth) and return the broker-assigned id.
  // Uses the FakeBroker directly: this seeds broker truth WITHOUT going through the
  // dispatcher, so the test controls exactly what the broker holds.
  std::string seed_broker(const OrderIntent& intent) {
    auto ack = broker.place(intent);
    REQUIRE(ack.has_value());
    return ack.value().broker_order_id;
  }

 private:
  Store open_store() {
    auto opened = Store::open(":memory:");
    REQUIRE(opened.has_value());
    return std::move(opened.value());
  }
};

}  // namespace

// ── (a) broker_order_id match resolves the UNKNOWN to the broker's state ──────
TEST_CASE("resolve: a broker_order_id match adopts broker truth, resolved_ok",
          "[runtime][unknown][resolve]") {
  Harness h;
  // Seed broker truth: an order the broker holds (state becomes Filled).
  const std::string ref = "alpha-aaaa-0001";
  const std::string broker_id = h.seed_broker(sample_intent(ref));

  // The local order carries the SAME broker_order_id -> strongest rung.
  Order local = unknown_order(ref, broker_id);
  REQUIRE(h.store.insert_order(local).has_value());

  const std::size_t reads_before = h.broker.request_count();
  auto result = h.resolver.resolve(local);
  REQUIRE(result.has_value());
  const UnknownResolution& res = result.value();

  CHECK(res.kind == MatchKind::BrokerOrderId);
  CHECK(res.resolved_ok);
  REQUIRE(res.resolved.has_value());
  // The broker's state (Filled in the fake's deterministic model) was adopted.
  CHECK(res.new_state == OrderState::Filled);
  CHECK(res.resolved->state == OrderState::Filled);
  CHECK(res.resolved->broker_order_id == broker_id);

  // The projection now reflects broker truth (no longer Unknown).
  auto stored = h.store.find_order(ref);
  REQUIRE(stored.has_value());
  REQUIRE(stored.value().has_value());
  CHECK(stored.value()->state == OrderState::Filled);

  // ONLY reads happened (no second fire) and NO alert was raised on a match.
  CHECK(h.broker.request_count() == reads_before + 1);  // exactly one fetch_orders
  CHECK(h.alerts.count() == 0);
}

// ── (b) correlation-token match when broker_order_id is empty ────────────────
TEST_CASE("resolve: a correlation-token match when the local broker id is empty",
          "[runtime][unknown][resolve]") {
  Harness h;
  const std::string ref = "alpha-bbbb-0002";
  h.seed_broker(sample_intent(ref));  // broker echoes the client_ref on its order

  // The local order has NO broker_order_id, but its client_ref matches -> rung 2.
  Order local = unknown_order(ref, /*broker_order_id=*/"");
  REQUIRE(h.store.insert_order(local).has_value());

  auto result = h.resolver.resolve(local);
  REQUIRE(result.has_value());
  CHECK(result.value().kind == MatchKind::CorrelationToken);
  CHECK(result.value().resolved_ok);
  REQUIRE(result.value().resolved.has_value());
  // The id-less local order adopts the broker's id during resolution.
  CHECK_FALSE(result.value().resolved->broker_order_id.empty());
  CHECK(result.value().new_state == OrderState::Filled);
  CHECK(h.alerts.count() == 0);
}

// ── (c) attribute corroboration when both ids absent but attributes + window ──
TEST_CASE("resolve: attribute corroboration when neither id matches",
          "[runtime][unknown][resolve]") {
  Harness h;
  // Broker truth carries a DIFFERENT client_ref and a broker-minted id, so neither
  // the id rung nor the correlation rung can hit; only attributes corroborate.
  OrderIntent broker_intent = sample_intent("broker-side-ref-zzzz");
  const std::string broker_id = h.seed_broker(broker_intent);

  // The local UNKNOWN order shares (symbol, side, qty, price) but has its OWN,
  // non-matching client_ref and no broker id.
  Order local = unknown_order("alpha-cccc-0003", /*broker_order_id=*/"");
  REQUIRE(h.store.insert_order(local).has_value());

  auto result = h.resolver.resolve(local);
  REQUIRE(result.has_value());
  CHECK(result.value().kind == MatchKind::AttributeCorroboration);
  CHECK(result.value().resolved_ok);
  REQUIRE(result.value().resolved.has_value());
  CHECK(result.value().new_state == OrderState::Filled);
  // The corroborated order adopts the broker's id.
  CHECK(result.value().resolved->broker_order_id == broker_id);
  CHECK(h.alerts.count() == 0);
}

// ── (c2) IMP-11: the TRIGGER is part of attribute corroboration ───────────────
//
// For a stop, the trigger is not a detail — it IS the order. Two protective stops
// on the same symbol, side and size, differing only in the level at which they
// arm, are different orders carrying different risk. Corroborating on
// (symbol, side, qty, price) alone would let this rung adopt a stop armed at the
// WRONG level as though it were ours, and the local order would then be marked
// resolved against protection that fires somewhere else entirely.
TEST_CASE("resolve: stops at DIFFERENT trigger levels do not cross-corroborate",
          "[runtime][unknown][resolve][IMP-11]") {
  const auto stop_intent = [](std::string ref, std::int64_t trigger_rupees) {
    OrderIntent intent = sample_intent(std::move(ref));
    intent.order_type = OrderType::StopLoss;
    intent.price = Price::from_rupees(123, 50);  // identical limit on both
    intent.trigger_price = Price::from_rupees(trigger_rupees);
    return intent;
  };

  SECTION("a different trigger level blocks the match -> fails closed to NoMatch") {
    Harness h;
    h.seed_broker(stop_intent("broker-side-ref-zzzz", /*trigger=*/124));

    Order local;
    local.intent = stop_intent("alpha-dddd-0004", /*trigger=*/125);  // armed elsewhere
    local.state = OrderState::Unknown;
    REQUIRE(h.store.insert_order(local).has_value());

    auto result = h.resolver.resolve(local);
    REQUIRE(result.has_value());
    CHECK(result.value().kind == MatchKind::NoMatch);
    CHECK_FALSE(result.value().resolved_ok);
    CHECK(h.alerts.count() == 1);  // fail-closed escalation, nothing adopted
  }

  SECTION("the SAME trigger level still corroborates") {
    Harness h;
    h.seed_broker(stop_intent("broker-side-ref-zzzz", /*trigger=*/124));

    Order local;
    local.intent = stop_intent("alpha-eeee-0005", /*trigger=*/124);
    local.state = OrderState::Unknown;
    REQUIRE(h.store.insert_order(local).has_value());

    auto result = h.resolver.resolve(local);
    REQUIRE(result.has_value());
    CHECK(result.value().kind == MatchKind::AttributeCorroboration);
    CHECK(result.value().resolved_ok);
  }

  SECTION("an ARMED stop never corroborates an UNARMED order") {
    // std::optional equality gives the right answer at the boundary: absent is
    // not "zero", so a plain order can never stand in for a stop.
    Harness h;
    h.seed_broker(sample_intent("broker-side-ref-zzzz"));  // plain Limit, no trigger

    Order local;
    local.intent = stop_intent("alpha-ffff-0006", /*trigger=*/124);
    local.state = OrderState::Unknown;
    REQUIRE(h.store.insert_order(local).has_value());

    auto result = h.resolver.resolve(local);
    REQUIRE(result.has_value());
    CHECK(result.value().kind == MatchKind::NoMatch);
  }
}

// ── (d) NoMatch fails closed: stays Unknown, alert raised, NOTHING sent ───────
TEST_CASE("resolve: no authoritative match fails closed with a Critical alert, nothing sent",
          "[runtime][unknown][resolve][fail-closed]") {
  Harness h;
  // Broker truth holds a COMPLETELY different order (different symbol/qty/price and
  // refs) -> no rung can match.
  OrderIntent other = sample_intent("broker-other-ref");
  other.symbol = "BANKNIFTY24JUN50000PE";
  other.quantity = Quantity::of(15);
  other.price = Price::from_rupees(200, 0);
  h.seed_broker(other);

  Order local = unknown_order("alpha-dddd-0004", "LOCAL-ID-NOT-AT-BROKER");
  REQUIRE(h.store.insert_order(local).has_value());

  const std::size_t reads_before = h.broker.request_count();
  auto result = h.resolver.resolve(local);
  REQUIRE(result.has_value());
  const UnknownResolution& res = result.value();

  // Fail-closed: kind NoMatch, NOT resolved, state stays Unknown.
  CHECK(res.kind == MatchKind::NoMatch);
  CHECK_FALSE(res.resolved_ok);
  CHECK_FALSE(res.resolved.has_value());
  CHECK(res.new_state == OrderState::Unknown);

  // The order is STILL Unknown in the projection (never flipped on a guess).
  auto stored = h.store.find_order("alpha-dddd-0004");
  REQUIRE(stored.has_value());
  REQUIRE(stored.value().has_value());
  CHECK(stored.value()->state == OrderState::Unknown);

  // A Critical alert WAS raised (the dead-man's-switch escalation).
  REQUIRE(h.alerts.count() == 1);
  CHECK(h.alerts.entries().front().level == AlertLevel::Critical);

  // ...AND IT NAMES THE ORDER (IMP-16). The ids ride in the TYPED context, not
  // interpolated into the free-form body — a sink scrubs the body, and a
  // client_ref is one long token-shaped run, so an interpolated ref reached the
  // operator as `ref=***REDACTED***`: the most urgent alert in the system named
  // no order at all.
  const auto& alert = h.alerts.entries().front();
  CHECK(alert.provenance.client_ref == "alpha-dddd-0004");
  CHECK(alert.provenance.broker_order_id == "LOCAL-ID-NOT-AT-BROKER");
  CHECK(alert.provenance.strategy == "alpha");
  // The body itself no longer carries the ids (that is the point of the move).
  CHECK(alert.message.find("alpha-dddd-0004") == std::string::npos);

  // NOTHING was sent: only the single fetch_orders read advanced the request
  // count, and the broker book is unchanged (no place/modify/cancel happened).
  CHECK(h.broker.request_count() == reads_before + 1);  // exactly one read
  CHECK(h.broker.book().size() == 1);                   // only the seeded order
}

// ── precedence: a broker_order_id hit WINS over a colliding attribute match ───
TEST_CASE("resolve: broker_order_id takes precedence over a weaker attribute match",
          "[runtime][unknown][resolve][precedence]") {
  Harness h;
  // Two broker orders with identical attributes. Only the SECOND shares the local
  // order's broker_order_id; precedence must pick it, not the attribute-equal first.
  OrderIntent a = sample_intent("broker-ref-A");
  OrderIntent b = sample_intent("broker-ref-B");
  h.seed_broker(a);
  const std::string broker_id_b = h.seed_broker(b);

  Order local = unknown_order("alpha-eeee-0005", broker_id_b);
  REQUIRE(h.store.insert_order(local).has_value());

  auto result = h.resolver.resolve(local);
  REQUIRE(result.has_value());
  CHECK(result.value().kind == MatchKind::BrokerOrderId);
  REQUIRE(result.value().resolved.has_value());
  CHECK(result.value().resolved->broker_order_id == broker_id_b);
}

// ── resolve_all: resolves only the Unknown orders in the store ────────────────
TEST_CASE("resolve_all: resolves Unknown orders and leaves others untouched",
          "[runtime][unknown][resolve]") {
  Harness h;
  const std::string ref = "alpha-ffff-0006";
  const std::string broker_id = h.seed_broker(sample_intent(ref));

  // One Unknown order (resolvable by id) and one already-Acknowledged order.
  Order u = unknown_order(ref, broker_id);
  REQUIRE(h.store.insert_order(u).has_value());

  Order ack;
  ack.intent = sample_intent("alpha-gggg-0007");
  ack.intent.quantity = Quantity::of(25);  // distinct ref/order
  ack.state = OrderState::Acknowledged;
  ack.broker_order_id = "FAKE-ORD-ACK";
  REQUIRE(h.store.insert_order(ack).has_value());

  auto results = h.resolver.resolve_all();
  REQUIRE(results.has_value());
  // Exactly ONE resolution (only the Unknown order is processed).
  REQUIRE(results.value().size() == 1);
  CHECK(results.value().front().kind == MatchKind::BrokerOrderId);
  CHECK(results.value().front().resolved_ok);

  // The Acknowledged order is untouched.
  auto stored = h.store.find_order("alpha-gggg-0007");
  REQUIRE(stored.has_value());
  REQUIRE(stored.value().has_value());
  CHECK(stored.value()->state == OrderState::Acknowledged);
}
