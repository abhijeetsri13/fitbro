#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "broker_exec/domain/domain.hpp"

using namespace broker_exec::domain;

TEST_CASE("Money stores exact integer paise with no float", "[domain][money]") {
  const Money a = Money::from_paise(12345);
  REQUIRE(a.paise() == 12345);

  // from_rupees composes rupees + paise, sign-correct on the fractional part.
  REQUIRE(Money::from_rupees(123, 45).paise() == 12345);
  REQUIRE(Money::from_rupees(0, 7).paise() == 7);
  REQUIRE(Money::from_rupees(-5, 50).paise() == -550);
  REQUIRE(Money::from_rupees(10).paise() == 1000);
}

TEST_CASE("Money arithmetic and comparisons are exact", "[domain][money]") {
  const Money a = Money::from_paise(1000);
  const Money b = Money::from_paise(250);

  REQUIRE((a + b).paise() == 1250);
  REQUIRE((a - b).paise() == 750);
  REQUIRE((-b).paise() == -250);

  REQUIRE(a == Money::from_paise(1000));
  REQUIRE(a != b);
  REQUIRE(b < a);
  REQUIRE(a > b);
  REQUIRE(a >= Money::from_paise(1000));
  REQUIRE(b <= Money::from_paise(250));

  // A repeated-add stays bit-exact (the float-drift trap this type avoids).
  Money sum;
  for (int i = 0; i < 10; ++i) {
    sum = sum + Money::from_paise(10);
  }
  REQUIRE(sum.paise() == 100);
}

TEST_CASE("Money::to_string formats rupees.paise with sign", "[domain][money]") {
  REQUIRE(Money::from_paise(0).to_string() == "0.00");
  REQUIRE(Money::from_paise(5).to_string() == "0.05");
  REQUIRE(Money::from_paise(50).to_string() == "0.50");
  REQUIRE(Money::from_paise(12345).to_string() == "123.45");
  REQUIRE(Money::from_paise(-550).to_string() == "-5.50");
  REQUIRE(Money::from_paise(-7).to_string() == "-0.07");
}

TEST_CASE("Price stores paise and compares by value", "[domain][price]") {
  const Price p = Price::from_rupees(100, 25);
  REQUIRE(p.paise() == 10025);
  REQUIRE(p == Price::from_paise(10025));
  REQUIRE(Price::from_paise(5) < Price::from_paise(10));
  REQUIRE(p.to_string() == "100.25");
}

TEST_CASE("Price::round_to_tick rounds half-up to the nearest tick", "[domain][price]") {
  const Price tick = Price::from_paise(5);  // 5 paise tick

  // Already on a tick -> unchanged.
  REQUIRE(Price::from_paise(100).round_to_tick(tick) == Price::from_paise(100));

  // Below half -> down; at/above half -> up.
  REQUIRE(Price::from_paise(102).round_to_tick(tick) == Price::from_paise(100));
  REQUIRE(Price::from_paise(103).round_to_tick(tick) == Price::from_paise(105));

  // Exact half rounds up (half-up rule). tick 10, value 25 -> 30.
  const Price tick10 = Price::from_paise(10);
  REQUIRE(Price::from_paise(25).round_to_tick(tick10) == Price::from_paise(30));
  REQUIRE(Price::from_paise(24).round_to_tick(tick10) == Price::from_paise(20));

  // Negative values: magnitude-symmetric half-up (-25 -> -20 mirrors +25 -> +30
  // by rounding the half toward +inf consistently).
  REQUIRE(Price::from_paise(-25).round_to_tick(tick10) == Price::from_paise(-20));
  REQUIRE(Price::from_paise(-26).round_to_tick(tick10) == Price::from_paise(-30));

  // Non-positive tick is a no-op (validated upstream at the gate).
  REQUIRE(Price::from_paise(103).round_to_tick(Price::from_paise(0)) == Price::from_paise(103));
  REQUIRE(Price::from_paise(103).round_to_tick(Price::from_paise(-5)) == Price::from_paise(103));
}

TEST_CASE("Quantity is a strong integer with arithmetic and comparisons", "[domain][quantity]") {
  const Quantity q = Quantity::of(50);
  REQUIRE(q.value() == 50);
  REQUIRE((q + Quantity::of(25)).value() == 75);
  REQUIRE((q - Quantity::of(10)).value() == 40);
  REQUIRE(q == Quantity::of(50));
  REQUIRE(Quantity::of(10) < Quantity::of(20));
  REQUIRE(q.to_string() == "50");

  // Signed quantities model short positions.
  REQUIRE(Quantity::of(-75).value() == -75);
}

TEST_CASE("Default-constructed value types are zero", "[domain][money]") {
  REQUIRE(Money{}.paise() == 0);
  REQUIRE(Price{}.paise() == 0);
  REQUIRE(Quantity{}.value() == 0);
}

TEST_CASE("Domain enums round-trip to stable serialization names", "[domain][enums]") {
  REQUIRE(to_string(OrderState::Created) == "CREATED");
  REQUIRE(to_string(OrderState::Unknown) == "UNKNOWN");
  REQUIRE(to_string(OrderState::Reconciled) == "RECONCILED");
  REQUIRE(to_string(OrderState::PartiallyPlaced) == "PARTIALLY_PLACED");
  REQUIRE(to_string(OrderState::ManualInterventionRequired) == "MANUAL_INTERVENTION_REQUIRED");
  REQUIRE(to_string(OrderState::Filled) == "FILLED");

  REQUIRE(to_string(Side::Buy) == "BUY");
  REQUIRE(to_string(Side::Sell) == "SELL");

  REQUIRE(to_string(OrderType::StopLossMarket) == "SL-M");
  REQUIRE(to_string(Product::Delivery) == "DELIVERY");
}

TEST_CASE("Aggregate value types compare by value", "[domain][types]") {
  const Instrument inst{.symbol = "NIFTY24JUN24000CE",
                        .token = 12345,
                        .exchange = "NFO",
                        .lot_size = Quantity::of(50),
                        .tick_size = Price::from_paise(5),
                        .freeze_qty = Quantity::of(1800),
                        .expiry = "2024-06-27"};
  Instrument inst_copy = inst;
  REQUIRE(inst == inst_copy);
  inst_copy.token = 999;
  REQUIRE(inst != inst_copy);

  const OrderIntent intent{.client_ref = "alpha-ab12cd34-uuid",
                           .symbol = "NIFTY24JUN24000CE",
                           .side = Side::Sell,
                           .quantity = Quantity::of(50),
                           .price = Price::from_rupees(120, 50),
                           .order_type = OrderType::Limit,
                           .product = Product::Intraday,
                           .strategy = "alpha"};
  REQUIRE(intent == intent);
  OrderIntent intent2 = intent;
  intent2.side = Side::Buy;
  REQUIRE(intent != intent2);

  // The trigger price (IMP-11) is part of value identity, and — because it is an
  // optional — ABSENT and PRESENT are distinguishable, not collapsed onto a zero.
  REQUIRE_FALSE(intent.trigger_price.has_value());  // default: not a stop order
  OrderIntent armed = intent;
  armed.trigger_price = Price::from_rupees(119, 50);
  REQUIRE(intent != armed);
  OrderIntent armed_higher = armed;
  armed_higher.trigger_price = Price::from_rupees(121);
  REQUIRE(armed != armed_higher);  // a different LEVEL is a different order
  OrderIntent armed_at_zero = intent;
  armed_at_zero.trigger_price = Price::from_paise(0);
  REQUIRE(intent != armed_at_zero);  // absent != engaged-at-zero

  const Order order{.intent = intent,
                    .state = OrderState::PartiallyFilled,
                    .broker_order_id = "BRK-1",
                    .filled_qty = Quantity::of(25),
                    .avg_price = Price::from_rupees(120, 50)};
  REQUIRE(order == order);

  const Trade trade{.trade_id = "T-1",
                    .client_ref = intent.client_ref,
                    .broker_order_id = "BRK-1",
                    .quantity = Quantity::of(25),
                    .price = Price::from_rupees(120, 50)};
  REQUIRE(trade == trade);

  const Position pos{
      .symbol = "NIFTY24JUN24000CE", .net_qty = Quantity::of(-25), .avg_price = Price::from_rupees(120, 50)};
  Position pos2 = pos;
  REQUIRE(pos == pos2);
  pos2.net_qty = Quantity::of(0);
  REQUIRE(pos != pos2);
}

TEST_CASE("Value types serialize to a stable, non-empty string", "[domain][serialize]") {
  // The to_string() forms are the simple serialize surface for logs/round-trip.
  const Instrument inst{.symbol = "ACME",
                        .token = 7,
                        .exchange = "NSE",
                        .lot_size = Quantity::of(1),
                        .tick_size = Price::from_paise(5),
                        .freeze_qty = Quantity::of(100000),
                        .expiry = ""};
  const std::string s = inst.to_string();
  REQUIRE(s.find("symbol=ACME") != std::string::npos);
  REQUIRE(s.find("token=7") != std::string::npos);
  REQUIRE(s.find("tick_size=0.05") != std::string::npos);

  const OrderIntent intent{.client_ref = "alpha-ab12cd34-uuid",
                           .symbol = "ACME",
                           .side = Side::Buy,
                           .quantity = Quantity::of(10),
                           .price = Price::from_rupees(99, 95),
                           .order_type = OrderType::Limit,
                           .product = Product::Delivery,
                           .strategy = "alpha"};
  const std::string is = intent.to_string();
  REQUIRE(is.find("side=BUY") != std::string::npos);
  REQUIRE(is.find("order_type=LIMIT") != std::string::npos);
  REQUIRE(is.find("product=DELIVERY") != std::string::npos);
  REQUIRE(is.find("price=99.95") != std::string::npos);
  // An absent trigger prints "none" — a log reader must be able to tell "not a
  // stop order" from "a stop armed at 0.00".
  REQUIRE(is.find("trigger_price=none") != std::string::npos);

  OrderIntent stop = intent;
  stop.order_type = OrderType::StopLoss;
  stop.trigger_price = Price::from_rupees(99, 50);
  const std::string ss = stop.to_string();
  REQUIRE(ss.find("trigger_price=99.50") != std::string::npos);
  REQUIRE(ss.find("price=99.95") != std::string::npos);  // BOTH numbers, distinctly

  const Order order{.intent = intent,
                    .state = OrderState::Sent,
                    .broker_order_id = "BRK-9",
                    .filled_qty = Quantity::of(0),
                    .avg_price = Price{}};
  REQUIRE(order.to_string().find("state=SENT") != std::string::npos);

  const Trade trade{.trade_id = "T-9",
                    .client_ref = intent.client_ref,
                    .broker_order_id = "BRK-9",
                    .quantity = Quantity::of(10),
                    .price = Price::from_rupees(99, 95)};
  REQUIRE(trade.to_string().find("trade_id=T-9") != std::string::npos);

  const Position pos{.symbol = "ACME", .net_qty = Quantity::of(10), .avg_price = Price::from_rupees(99, 95)};
  REQUIRE(pos.to_string().find("net_qty=10") != std::string::npos);
}
