#include "broker_exec/slicing/freeze_slicer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/idempotency/idempotency.hpp"
#include "broker_exec/result.hpp"

using broker_exec::slicing::FreezeSlicer;
using broker_exec::domain::Instrument;
using broker_exec::domain::OrderIntent;
using broker_exec::domain::OrderType;
using broker_exec::domain::Price;
using broker_exec::domain::Product;
using broker_exec::domain::Quantity;
using broker_exec::domain::Side;
using broker_exec::errors::ErrorCategory;

namespace {

// The known parent intent under test: a real OrderIntent built via the domain
// value type. A canonical parent client_ref ("<strategy>-<sig8>-<uuid>" shape),
// no '#'.
[[nodiscard]] OrderIntent make_parent(std::int64_t qty) {
  return OrderIntent{.client_ref = "alpha-1a2b3c4d-uuid",
                     .symbol = "NIFTY26JUL24000CE",
                     .side = Side::Buy,
                     .quantity = Quantity::of(qty),
                     .price = Price::from_rupees(120, 50),
                     .order_type = OrderType::Limit,
                     .product = Product::Intraday,
                     .strategy = "alpha"};
}

// The known instrument: NFO option, lot 50, freeze 1800, tick 5 paise (0.05).
[[nodiscard]] Instrument make_instrument(std::int64_t lot = 50, std::int64_t freeze = 1800) {
  return Instrument{.symbol = "NIFTY26JUL24000CE",
                    .token = 123456,
                    .exchange = "NFO",
                    .lot_size = Quantity::of(lot),
                    .tick_size = Price::from_paise(5),
                    .freeze_qty = Quantity::of(freeze),
                    .expiry = "2026-07-30"};
}

}  // namespace

TEST_CASE("over-freeze fans out into lot-aligned children that sum to qty") {
  const FreezeSlicer slicer;
  const OrderIntent parent = make_parent(4000);
  const Instrument inst = make_instrument();

  const auto result = slicer.slice(parent, inst);
  REQUIRE(result);
  const std::vector<OrderIntent>& children = result.value();

  REQUIRE(children.size() == 3);
  CHECK(children[0].quantity == Quantity::of(1800));
  CHECK(children[1].quantity == Quantity::of(1800));
  CHECK(children[2].quantity == Quantity::of(400));

  CHECK(children[0].client_ref == "alpha-1a2b3c4d-uuid#1");
  CHECK(children[1].client_ref == "alpha-1a2b3c4d-uuid#2");
  CHECK(children[2].client_ref == "alpha-1a2b3c4d-uuid#3");

  // Sum-invariant: children sum EXACTLY to the parent quantity; each child qty is
  // in [lot, freeze] and lot-aligned.
  std::int64_t sum = 0;
  for (const OrderIntent& child : children) {
    const std::int64_t q = child.quantity.value();
    sum += q;
    CHECK(q >= 50);
    CHECK(q <= 1800);
    CHECK(q % 50 == 0);
    // Non-quantity, non-ref fields are carried through unchanged from the parent.
    CHECK(child.symbol == parent.symbol);
    CHECK(child.side == parent.side);
    CHECK(child.price == parent.price);
    CHECK(child.order_type == parent.order_type);
    CHECK(child.product == parent.product);
    CHECK(child.strategy == parent.strategy);
    CHECK(child.trigger_price == parent.trigger_price);
  }
  CHECK(sum == 4000);
}

// ── IMP-11 AC-4: children inherit the parent's trigger UNCHANGED ─────────────
TEST_CASE("slicing an over-freeze STOP carries the trigger to every child",
          "[slicing][IMP-11]") {
  // The hazard this pins: slicing changes SIZE, never price. If a child lost the
  // trigger it would be placed as a plain order — so an over-freeze protective
  // stop would fan out into pieces that are not stops at all, and the position
  // would sit unprotected behind an order the caller believes is armed.
  const FreezeSlicer slicer;
  OrderIntent parent = make_parent(4000);
  parent.order_type = OrderType::StopLoss;
  // A well-formed BUY stop (make_parent is a Buy): it arms as the market rises
  // through 120 and then works UP to 121 — limit >= trigger, the shape the
  // validation gate requires for a Buy.
  parent.trigger_price = Price::from_rupees(120);  // the level that arms it
  parent.price = Price::from_rupees(121);          // the limit it then works at

  const auto result = slicer.slice(parent, make_instrument());
  REQUIRE(result);
  const std::vector<OrderIntent>& children = result.value();
  REQUIRE(children.size() == 3);

  for (const OrderIntent& child : children) {
    REQUIRE(child.trigger_price.has_value());
    // UNCHANGED, not merely present: every child arms at the parent's level.
    CHECK(*child.trigger_price == Price::from_rupees(120));
    CHECK(child.price == Price::from_rupees(121));  // and the two stay distinct
    CHECK(child.order_type == OrderType::StopLoss);
  }
}

TEST_CASE("a NON-stop parent's children stay trigger-free", "[slicing][IMP-11]") {
  // The other direction: slicing must not FABRICATE a trigger (which would make
  // every sliced limit order fail the gate's shape check).
  const FreezeSlicer slicer;
  const auto result = slicer.slice(make_parent(4000), make_instrument());
  REQUIRE(result);
  for (const OrderIntent& child : result.value()) {
    CHECK_FALSE(child.trigger_price.has_value());
  }
}

TEST_CASE("an under-freeze STOP passes through with its trigger intact", "[slicing][IMP-11]") {
  // The not-over-freeze path returns the parent itself; assert it is a genuine
  // pass-through rather than a rebuild that could drop a field.
  const FreezeSlicer slicer;
  OrderIntent parent = make_parent(100);
  parent.order_type = OrderType::StopLossMarket;
  parent.trigger_price = Price::from_rupees(120);

  const auto result = slicer.slice(parent, make_instrument());
  REQUIRE(result);
  REQUIRE(result.value().size() == 1);
  CHECK(result.value().front() == parent);  // whole-value equality, trigger included
}

TEST_CASE("exact multiple of chunk produces no remainder child") {
  const FreezeSlicer slicer;
  const auto result = slicer.slice(make_parent(3600), make_instrument());
  REQUIRE(result);
  const std::vector<OrderIntent>& children = result.value();

  REQUIRE(children.size() == 2);
  CHECK(children[0].quantity == Quantity::of(1800));
  CHECK(children[1].quantity == Quantity::of(1800));
  CHECK(children[0].client_ref == "alpha-1a2b3c4d-uuid#1");
  CHECK(children[1].client_ref == "alpha-1a2b3c4d-uuid#2");
}

TEST_CASE("not over-freeze passes the parent through unchanged") {
  const FreezeSlicer slicer;
  const OrderIntent parent = make_parent(1000);
  const auto result = slicer.slice(parent, make_instrument());
  REQUIRE(result);
  const std::vector<OrderIntent>& children = result.value();

  REQUIRE(children.size() == 1);
  CHECK(children[0] == parent);
  CHECK(children[0].client_ref == "alpha-1a2b3c4d-uuid");
  CHECK(children[0].client_ref.find('#') == std::string::npos);
  CHECK(children[0].quantity == Quantity::of(1000));
}

TEST_CASE("freeze that is not a lot multiple floors the chunk down to a lot") {
  const FreezeSlicer slicer;
  // freeze 1825, lot 50 -> chunk = (1825/50)*50 = 1800.
  const auto result = slicer.slice(make_parent(4000), make_instrument(50, 1825));
  REQUIRE(result);
  const std::vector<OrderIntent>& children = result.value();

  REQUIRE(children.size() == 3);
  CHECK(children[0].quantity == Quantity::of(1800));
  CHECK(children[1].quantity == Quantity::of(1800));
  CHECK(children[2].quantity == Quantity::of(400));
}

TEST_CASE("slice is deterministic — two calls are deep-equal") {
  const FreezeSlicer slicer;
  const OrderIntent parent = make_parent(4000);
  const Instrument inst = make_instrument();

  const auto a = slicer.slice(parent, inst);
  const auto b = slicer.slice(parent, inst);
  REQUIRE(a);
  REQUIRE(b);
  REQUIRE(a.value().size() == b.value().size());
  for (std::size_t i = 0; i < a.value().size(); ++i) {
    CHECK(a.value()[i] == b.value()[i]);
    CHECK(a.value()[i].quantity == b.value()[i].quantity);
    CHECK(a.value()[i].client_ref == b.value()[i].client_ref);
  }
}

TEST_CASE("child refs are parity-identical to idempotency::child_ref") {
  const FreezeSlicer slicer;
  const OrderIntent parent = make_parent(4000);
  const auto result = slicer.slice(parent, make_instrument());
  REQUIRE(result);
  const std::vector<OrderIntent>& children = result.value();

  for (std::size_t i = 0; i < children.size(); ++i) {
    const int k = static_cast<int>(i) + 1;
    CHECK(children[i].client_ref ==
          broker_exec::idempotency::child_ref(parent.client_ref, k));
  }
}

TEST_CASE("a child ref is recognized as a child of the parent (AC-3 doc)") {
  const FreezeSlicer slicer;
  const OrderIntent parent = make_parent(4000);
  const auto result = slicer.slice(parent, make_instrument());
  REQUIRE(result);
  const std::vector<OrderIntent>& children = result.value();

  for (const OrderIntent& child : children) {
    CHECK(broker_exec::idempotency::is_child_ref(child.client_ref));
    CHECK(broker_exec::idempotency::parent_of(child.client_ref) == parent.client_ref);
  }
}

TEST_CASE("invalid inputs fail closed with a single Validation error") {
  const FreezeSlicer slicer;

  SECTION("quantity not lot-aligned") {
    const auto result = slicer.slice(make_parent(75), make_instrument());
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::Validation);
  }

  SECTION("freeze below one lot") {
    const auto result = slicer.slice(make_parent(4000), make_instrument(50, 25));
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::Validation);
  }

  SECTION("parent client_ref already a child") {
    OrderIntent parent = make_parent(4000);
    parent.client_ref = "alpha-x-uuid#1";
    const auto result = slicer.slice(parent, make_instrument());
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::Validation);
  }

  SECTION("zero quantity") {
    const auto result = slicer.slice(make_parent(0), make_instrument());
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::Validation);
  }
}

TEST_CASE("freeze-slice boundaries and guards", "[slicing][boundary]") {
  const FreezeSlicer slicer;

  SECTION("qty == freeze is a passthrough (not over-freeze)") {
    const OrderIntent parent = make_parent(1800);
    const auto result = slicer.slice(parent, make_instrument(50, 1800));
    REQUIRE(result);
    REQUIRE(result.value().size() == 1U);
    CHECK(result.value()[0].client_ref == parent.client_ref);  // unchanged, no '#'
    CHECK(result.value()[0].quantity == Quantity::of(1800));
  }

  SECTION("qty == freeze + lot -> smallest over-freeze split [freeze, lot]") {
    const auto result = slicer.slice(make_parent(1850), make_instrument(50, 1800));
    REQUIRE(result);
    const auto& c = result.value();
    REQUIRE(c.size() == 2U);
    CHECK(c[0].quantity == Quantity::of(1800));
    CHECK(c[1].quantity == Quantity::of(50));
    std::int64_t sum = 0;
    for (const auto& ch : c) {
      sum += ch.quantity.value();
    }
    CHECK(sum == 1850);
  }

  SECTION("lot <= 0 is rejected before any modulo (mod-by-zero guard)") {
    const auto result = slicer.slice(make_parent(4000), make_instrument(0, 1800));
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::Validation);
  }

  SECTION("freeze == 0 is rejected") {
    const auto result = slicer.slice(make_parent(4000), make_instrument(50, 0));
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::Validation);
  }

  SECTION("large quantity preserves the sum invariant and contiguous refs") {
    const std::int64_t qty = 100000;  // lot 50, freeze 1800 -> chunk 1800
    const auto result = slicer.slice(make_parent(qty), make_instrument(50, 1800));
    REQUIRE(result);
    const auto& c = result.value();
    std::int64_t sum = 0;
    for (std::size_t k = 0; k < c.size(); ++k) {
      sum += c[k].quantity.value();
      CHECK(c[k].quantity.value() >= 50);
      CHECK(c[k].quantity.value() <= 1800);
      CHECK(c[k].quantity.value() % 50 == 0);
      CHECK(c[k].client_ref == make_parent(qty).client_ref + "#" + std::to_string(k + 1));
    }
    CHECK(sum == qty);  // 100000 = 55*1800 + 1000 -> 56 children
    CHECK(c.size() == 56U);
  }
}
