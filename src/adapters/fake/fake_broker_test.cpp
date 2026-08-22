#include "broker_exec/adapters/fake/fake_broker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>
#include <vector>

#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/broker_port.hpp"

using namespace std::chrono_literals;
using broker_exec::adapters::fake::FakeBroker;
using broker_exec::adapters::fake::FaultConfig;
using broker_exec::clock::TestClock;
using broker_exec::domain::OrderIntent;
using broker_exec::domain::Price;
using broker_exec::domain::Quantity;
using broker_exec::domain::Side;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::SuggestedAction;

namespace {

// A minimal, deterministic order intent for the tests.
OrderIntent make_intent(std::string client_ref = "strat-abc12345-0001") {
  OrderIntent intent;
  intent.client_ref = std::move(client_ref);
  intent.symbol = "NIFTY24JUN24000CE";
  intent.side = Side::Buy;
  intent.quantity = Quantity::of(50);
  intent.price = Price::from_rupees(100, 0);
  intent.strategy = "strat";
  return intent;
}

}  // namespace

TEST_CASE("fault-free fake places, acks, and reflects the order in fetch_orders", "[fake]") {
  TestClock clk;
  FakeBroker broker(clk);

  const auto ack = broker.place(make_intent());
  REQUIRE(ack.has_value());
  CHECK(ack.value().broker_order_id == "FAKE-ORD-1");
  CHECK(ack.value().client_ref == "strat-abc12345-0001");

  const auto orders = broker.fetch_orders();
  REQUIRE(orders.has_value());
  REQUIRE(orders.value().size() == 1);
  CHECK(orders.value().front().broker_order_id == "FAKE-ORD-1");
}

TEST_CASE("ack_lost_but_placed: place() errors with ReconcileFirst but the order IS in the book",
          "[fake]") {
  TestClock clk;
  FaultConfig cfg;
  cfg.ack_lost_but_placed = true;
  FakeBroker broker(clk, cfg);

  const auto ack = broker.place(make_intent());
  // The caller sees a dangerous failure it must NOT blindly retry.
  REQUIRE_FALSE(ack.has_value());
  CHECK(ack.error().category == ErrorCategory::Timeout);
  CHECK(ack.error().action == SuggestedAction::ReconcileFirst);

  // But broker truth holds the order — reconciliation finds it, proving a blind
  // retry would have created a duplicate.
  const auto orders = broker.fetch_orders();
  REQUIRE(orders.has_value());
  REQUIRE(orders.value().size() == 1);
  CHECK(orders.value().front().broker_order_id == "FAKE-ORD-1");
  CHECK(orders.value().front().intent.client_ref == "strat-abc12345-0001");

  // Deterministic: a second broker built identically yields the identical id.
  TestClock clk2;
  FakeBroker broker2(clk2, cfg);
  const auto ack2 = broker2.place(make_intent());
  REQUIRE_FALSE(ack2.has_value());
  CHECK(broker2.book().size() == 1);
  CHECK(broker2.book().front().order.broker_order_id == "FAKE-ORD-1");
  CHECK_FALSE(broker2.book().front().ack_returned);
}

TEST_CASE("drop_ack: place() times out but the order is placed in the book", "[fake]") {
  TestClock clk;
  FaultConfig cfg;
  cfg.drop_ack = true;
  FakeBroker broker(clk, cfg);

  const auto ack = broker.place(make_intent());
  REQUIRE_FALSE(ack.has_value());
  CHECK(ack.error().category == ErrorCategory::Timeout);

  const auto orders = broker.fetch_orders();
  REQUIRE(orders.has_value());
  CHECK(orders.value().size() == 1);
  CHECK_FALSE(broker.book().front().ack_returned);
}

TEST_CASE("delay_ack_ticks: ack invisible until the injected clock advances N ticks", "[fake]") {
  TestClock clk;
  FaultConfig cfg;
  cfg.delay_ack_ticks = 3;
  cfg.tick_duration = 1ms;
  FakeBroker broker(clk, cfg);

  // Tick 0: ack is withheld (delayed), but the order is already in the book.
  const auto early = broker.place(make_intent("strat-abc12345-0001"));
  REQUIRE_FALSE(early.has_value());
  CHECK(early.error().category == ErrorCategory::Timeout);
  REQUIRE(broker.fetch_orders().value().size() == 1);

  // Advance 2 ticks: still before the threshold -> still withheld.
  clk.advance(2ms);
  const auto mid = broker.place(make_intent("strat-abc12345-0002"));
  REQUIRE_FALSE(mid.has_value());
  CHECK(mid.error().category == ErrorCategory::Timeout);

  // Advance to tick 3: threshold reached -> the ack now returns normally.
  clk.advance(1ms);
  const auto late = broker.place(make_intent("strat-abc12345-0003"));
  REQUIRE(late.has_value());
  CHECK(late.value().broker_order_id == "FAKE-ORD-3");

  // Reproducible: replaying the same timeline yields the same flip point.
  TestClock clk2;
  FakeBroker broker2(clk2, cfg);
  CHECK_FALSE(broker2.place(make_intent()).has_value());
  clk2.advance(3ms);
  CHECK(broker2.place(make_intent()).has_value());
}

TEST_CASE("rate_limit_after: the Nth+1 request returns RateLimited", "[fake]") {
  TestClock clk;
  FaultConfig cfg;
  cfg.rate_limit_after = 2;  // first two requests pass, then throttle
  FakeBroker broker(clk, cfg);

  CHECK(broker.place(make_intent("strat-abc12345-0001")).has_value());
  CHECK(broker.place(make_intent("strat-abc12345-0002")).has_value());

  const auto throttled = broker.place(make_intent("strat-abc12345-0003"));
  REQUIRE_FALSE(throttled.has_value());
  CHECK(throttled.error().category == ErrorCategory::RateLimited);

  // A throttled request does not consume budget and never reached the book.
  CHECK(broker.book().size() == 2);
  CHECK(broker.request_count() == 2);

  // Reads are throttled too once over budget (a real bucket doesn't discriminate).
  const auto orders = broker.fetch_orders();
  REQUIRE_FALSE(orders.has_value());
  CHECK(orders.error().category == ErrorCategory::RateLimited);
}

TEST_CASE("duplicate_fill: the same fill is reported twice", "[fake]") {
  TestClock clk;
  FaultConfig cfg;
  cfg.duplicate_fill = true;
  FakeBroker broker(clk, cfg);

  REQUIRE(broker.place(make_intent()).has_value());

  const auto trades = broker.fetch_trades();
  REQUIRE(trades.has_value());
  REQUIRE(trades.value().size() == 2);
  // Byte-identical rows: same trade_id, qty, price — a true double-report.
  CHECK(trades.value()[0] == trades.value()[1]);
  CHECK(trades.value()[0].trade_id == "FAKE-TRD-1");
}

TEST_CASE("out_of_order_events: fetch_* deliver rows in reversed sequence", "[fake]") {
  TestClock clk;
  FaultConfig cfg;
  cfg.out_of_order_events = true;
  FakeBroker broker(clk, cfg);

  REQUIRE(broker.place(make_intent("strat-abc12345-0001")).has_value());
  REQUIRE(broker.place(make_intent("strat-abc12345-0002")).has_value());
  REQUIRE(broker.place(make_intent("strat-abc12345-0003")).has_value());

  const auto orders = broker.fetch_orders();
  REQUIRE(orders.has_value());
  REQUIRE(orders.value().size() == 3);
  // Reversed: newest first. The apply-on-loop logic must reorder by the broker
  // ordering key, not trust arrival order.
  CHECK(orders.value()[0].broker_order_id == "FAKE-ORD-3");
  CHECK(orders.value()[2].broker_order_id == "FAKE-ORD-1");

  // Raw broker truth is still in insertion order — only the delivered view flips.
  REQUIRE(broker.book().size() == 3);
  CHECK(broker.book()[0].order.broker_order_id == "FAKE-ORD-1");
}

TEST_CASE("set_fault reconfigures between steps while preserving counters", "[fake]") {
  TestClock clk;
  FakeBroker broker(clk);  // start fault-free

  REQUIRE(broker.place(make_intent("strat-abc12345-0001")).has_value());
  CHECK(broker.book().size() == 1);

  // Flip to ack-lost-but-placed for the next step; the broker sequence continues.
  FaultConfig cfg;
  cfg.ack_lost_but_placed = true;
  broker.set_fault(cfg);

  const auto lost = broker.place(make_intent("strat-abc12345-0002"));
  REQUIRE_FALSE(lost.has_value());
  CHECK(broker.book().size() == 2);
  CHECK(broker.book()[1].order.broker_order_id == "FAKE-ORD-2");  // sequence preserved
  CHECK_FALSE(broker.book()[1].ack_returned);
}

TEST_CASE("modify/cancel on an unknown order returns OrderNotFound", "[fake]") {
  TestClock clk;
  FakeBroker broker(clk);

  const auto modr = broker.modify("FAKE-ORD-999", make_intent());
  REQUIRE_FALSE(modr.has_value());
  CHECK(modr.error().category == ErrorCategory::OrderNotFound);

  const auto canr = broker.cancel("FAKE-ORD-999");
  REQUIRE_FALSE(canr.has_value());
  CHECK(canr.error().category == ErrorCategory::OrderNotFound);
}

TEST_CASE("cancel under ack_lost_but_placed: caller errors but broker truth shows cancelled",
          "[fake]") {
  TestClock clk;
  FakeBroker broker(clk);
  REQUIRE(broker.place(make_intent()).has_value());

  FaultConfig cfg;
  cfg.ack_lost_but_placed = true;
  broker.set_fault(cfg);

  const auto canr = broker.cancel("FAKE-ORD-1");
  REQUIRE_FALSE(canr.has_value());
  CHECK(canr.error().category == ErrorCategory::Timeout);
  CHECK(canr.error().action == SuggestedAction::ReconcileFirst);

  // Reconcile against broker truth: the cancel DID take effect.
  CHECK(broker.book().front().order.state == broker_exec::domain::OrderState::Cancelled);
}

TEST_CASE("fetch_funds returns the seeded snapshot", "[fake]") {
  TestClock clk;
  FakeBroker broker(clk);
  broker.set_funds(broker_exec::ports::FundsSnapshot{
      broker_exec::domain::Money::from_rupees(100000), broker_exec::domain::Money::from_rupees(0)});

  const auto funds = broker.fetch_funds();
  REQUIRE(funds.has_value());
  CHECK(funds.value().available_margin == broker_exec::domain::Money::from_rupees(100000));
}
