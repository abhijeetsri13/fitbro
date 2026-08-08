// Kite adapter conformance (Story 2.14, AC-1; FR-37, NFR-3, TO-6). The CAPSTONE
// of Epic 2: it runs the SAME broker-agnostic conformance kit that certifies the
// FakeBroker (Epic 1) against the REAL KiteBrokerAdapter, driven by a stateful,
// fault-injecting recorded Kite HTTP endpoint. The kit is reused VERBATIM — only
// the BrokerFactory differs — proving the zero-duplicate invariant holds for Kite.
//
// Tier-1 certification: this runs in CI with a fake SecretProvider + the in-memory
// RecordedKiteServer (NO network, NO live credentials). Tier-2 (live-SDK
// verification capturing real `unknown` payloads as VCR fixtures + the live
// min-qty smoke) is the operator-run production-checklist step (see
// docs/kite-min-qty-smoke.md, architecture TO-6).
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`, no float.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include "broker_exec/adapters/fake/fake_broker.hpp"  // FaultConfig (the fault selector)
#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/clock_port.hpp"

#include "conformance_kit.hpp"
#include "recorded_kite_server.hpp"

namespace conf = broker_exec::conformance;

namespace {

using broker_exec::adapters::fake::FaultConfig;

// THE RECORDED KITE SERVER + THE OWNING ADAPTER STACK NOW LIVE IN
// tests/conformance/recorded_kite_server.hpp. They moved there VERBATIM in Story
// 6.3 so the broker-portability proof (tests/portability/) drives the SAME
// fixture instead of a copy that would quietly drift from this one. Nothing about
// this suite's behavior changed with the move.
using broker_exec::conformance::kite_fixture::OwningKiteAdapter;

// The Kite BrokerFactory: per scenario, build a fresh recorded server for the
// FaultConfig, a KiteRestClient over it + a fake SecretProvider, and a
// KiteBrokerAdapter — all owned by the returned BrokerPort.
conf::BrokerFactory kite_factory() {
  return [](broker_exec::ports::ClockPort& clock,
            FaultConfig fault) -> std::unique_ptr<broker_exec::ports::BrokerPort> {
    return std::make_unique<OwningKiteAdapter>(clock, fault);
  };
}

}  // namespace

TEST_CASE("conformance: the Kite adapter passes the full fault matrix with zero duplicates",
          "[conformance][kite]") {
  const conf::ConformanceReport report = conf::run_conformance(kite_factory());

  // Surface every failure line so a regression names the exact scenario+property.
  for (const std::string& f : report.failures) {
    UNSCOPED_INFO("kite conformance failure: " << f);
  }

  // The whole matrix ran, the headline zero-duplicate invariant held, and every
  // scenario passed all three properties (no-blind-retry, UNKNOWN handling, zero
  // duplicates) — i.e. the SAME kit that certifies the FakeBroker now gates Kite.
  CHECK(report.scenarios_run > 0);
  CHECK(report.duplicate_orders == 0);
  CHECK(report.scenarios_passed == report.scenarios_run);
  CHECK(report.ok());
}

TEST_CASE("conformance: every scenario in the matrix is exercised against Kite",
          "[conformance][kite]") {
  const conf::ConformanceReport report = conf::run_conformance(kite_factory());
  CHECK(report.scenarios_run == 7);
}

// Directly exercise the adapter (NOT via run_conformance) to prove two things the
// kit cannot prove for Kite by itself:
//   * Fix 1 — the tag->client_ref / id->client_ref recovery actually round-trips a
//     KNOWN, non-empty client_ref. The kit counts duplicates via
//     `o.intent.client_ref == client_ref`; an adapter that recovered an EMPTY
//     client_ref would report zero duplicates VACUOUSLY (an empty ref matches no
//     signal). Asserting the exact minted ref comes back closes that hole.
//   * Fix 2 — a single place() issues exactly ONE broker POST (no retry hidden
//     inside the adapter). The kit's no-blind-retry probe is gated on
//     `dynamic_cast<FakeBroker*>` and is SKIPPED for Kite; place_count() restores
//     that coverage at the adapter level.
TEST_CASE("[conformance][kite][recovery] ack-lost order is recovered with its client_ref",
          "[conformance][kite][recovery]") {
  // A KNOWN, non-empty client_ref in the canonical "<strategy>-<sig8>-<uuid>" shape
  // the dispatcher mints (Story 1.7). The recovery must hand this exact ref back.
  const std::string kClientRef = "alpha-1a2b3c4d-deadbeefcafebabe0123456789abcdef";

  const auto make_intent = [&] {
    broker_exec::domain::OrderIntent intent;
    intent.client_ref = kClientRef;
    intent.symbol = "NIFTY24JUN24000CE";
    intent.side = broker_exec::domain::Side::Sell;
    intent.quantity = broker_exec::domain::Quantity::of(50);
    intent.price = broker_exec::domain::Price::from_rupees(123, 50);
    intent.order_type = broker_exec::domain::OrderType::Limit;
    intent.product = broker_exec::domain::Product::Intraday;
    intent.strategy = "alpha";
    return intent;
  };

  SECTION("ack-lost place is recovered from the tag -> client_ref map") {
    // ack_lost_but_placed: the order IS live at the broker but the caller sees a
    // transport failure — the headline duplicate-risk the safety core must survive.
    broker_exec::clock::TestClock clock;
    OwningKiteAdapter owner(clock, FaultConfig{.ack_lost_but_placed = true});

    const broker_exec::domain::OrderIntent intent = make_intent();

    // place() returns an Error (ack lost). The dispatcher would mark this UNKNOWN
    // and reconcile — never blindly retry.
    auto placed = owner.adapter.place(intent);
    REQUIRE_FALSE(placed.has_value());

    // Fix 2: exactly ONE broker POST per place() call — the adapter does not retry
    // internally (the only legitimate re-fire is the dispatcher's, after reserve()).
    CHECK(owner.server->place_count() == 1);

    // Fix 1: fetch_orders() recovers the live order AND its originating client_ref
    // from the echoed tag. A non-empty, EXACT match proves the zero-duplicate pass
    // is not vacuous.
    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    const broker_exec::domain::Order& recovered = orders.value().front();
    CHECK_FALSE(recovered.intent.client_ref.empty());
    CHECK(recovered.intent.client_ref == kClientRef);
  }

  SECTION("clean place is recovered from the broker_order_id -> client_ref map") {
    broker_exec::clock::TestClock clock;
    OwningKiteAdapter owner(clock, FaultConfig{});

    const broker_exec::domain::OrderIntent intent = make_intent();

    // A clean place returns an ack carrying the broker_order_id and the echoed ref.
    auto placed = owner.adapter.place(intent);
    REQUIRE(placed.has_value());
    CHECK_FALSE(placed.value().broker_order_id.empty());
    CHECK(placed.value().client_ref == kClientRef);
    CHECK(owner.server->place_count() == 1);  // still exactly one POST

    // The same ref is recovered on reconcile — here via the (broker_order_id ->
    // client_ref) map populated by the returned ack.
    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    const broker_exec::domain::Order& recovered = orders.value().front();
    CHECK(recovered.intent.client_ref == kClientRef);
    CHECK(recovered.broker_order_id == placed.value().broker_order_id);
  }
}

// ── IMP-11 AC-2: the stop TRIGGER reaches the wire, and comes back ───────────
//
// Kite spells the activation price `trigger_price` in the order form. Before
// OrderIntent carried a distinct trigger, this adapter sent `price` under BOTH
// keys — so a live SL armed at its own limit. The recorded server captures the
// form field UNDER ITS EXACT NAME and echoes it on the orderbook, so an adapter
// that emitted the trigger under any other key (or duplicated `price` into it)
// fails these assertions rather than passing vacuously.
TEST_CASE("[conformance][kite][IMP-11] SL sends a DISTINCT trigger_price and parses it back",
          "[conformance][kite][IMP-11]") {
  const auto stop_intent = [](broker_exec::domain::OrderType type) {
    broker_exec::domain::OrderIntent intent;
    intent.client_ref = "alpha-1a2b3c4d-deadbeefcafebabe0123456789abcdef";
    intent.symbol = "NIFTY24JUN24000CE";
    intent.side = broker_exec::domain::Side::Sell;
    intent.quantity = broker_exec::domain::Quantity::of(50);
    intent.price = broker_exec::domain::Price::from_rupees(119);        // the LIMIT
    intent.trigger_price = broker_exec::domain::Price::from_rupees(120, 50);  // the TRIGGER
    intent.order_type = type;
    intent.product = broker_exec::domain::Product::Intraday;
    intent.strategy = "alpha";
    return intent;
  };

  SECTION("stop-loss LIMIT (SL): both numbers cross the wire, and they DIFFER") {
    broker_exec::clock::TestClock clock;
    OwningKiteAdapter owner(clock, FaultConfig{});

    REQUIRE(owner.adapter.place(stop_intent(broker_exec::domain::OrderType::StopLoss)).has_value());

    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    const broker_exec::domain::Order& back = orders.value().front();

    // The ORDER TYPE round-trips first: without it the recovered row would look
    // like a Market order carrying a trigger, a shape the validation gate refuses
    // outright — so the next touch of this order would fail closed on data we
    // invented during reconcile.
    CHECK(back.intent.order_type == broker_exec::domain::OrderType::StopLoss);
    // The trigger round-trips as 120.50 — NOT the 119.00 limit, which is what a
    // duplicated `price` would have produced.
    REQUIRE(back.intent.trigger_price.has_value());
    CHECK(*back.intent.trigger_price == broker_exec::domain::Price::from_paise(12050));
    // The limit went out under `price` (the fixture echoes it as average_price).
    CHECK(back.avg_price == broker_exec::domain::Price::from_paise(11900));
    CHECK(*back.intent.trigger_price != back.avg_price);
  }

  SECTION("stop-loss MARKET (SL-M): the trigger goes out, the ignored limit does not") {
    broker_exec::clock::TestClock clock;
    OwningKiteAdapter owner(clock, FaultConfig{});

    REQUIRE(owner.adapter.place(stop_intent(broker_exec::domain::OrderType::StopLossMarket))
                .has_value());

    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    const broker_exec::domain::Order& back = orders.value().front();
    CHECK(back.intent.order_type == broker_exec::domain::OrderType::StopLossMarket);
    REQUIRE(back.intent.trigger_price.has_value());
    CHECK(*back.intent.trigger_price == broker_exec::domain::Price::from_paise(12050));
    // No `price` form field was sent for a market-style order, so the fixture has
    // nothing to echo: the limit is genuinely absent from the wire.
    CHECK(back.avg_price == broker_exec::domain::Price::from_paise(0));
  }

  SECTION("an UNRECOGNIZED order_type falls closed to Market AND drops the trigger") {
    // The fail-safe pairing. A type we cannot read means we do not know what the
    // row is — and the one thing we must never do is publish it as an armed stop,
    // or as a Market carrying a trigger (a refused shape). Both must go together:
    // dropping only one of them still produces an order the gate rejects.
    broker_exec::clock::TestClock clock;
    OwningKiteAdapter owner(clock, FaultConfig{});

    REQUIRE(owner.adapter.place(stop_intent(broker_exec::domain::OrderType::StopLoss)).has_value());
    // Broker truth now reports a type from a vocabulary we do not know (a new Kite
    // variant, say). The trigger is still there and still positive.
    owner.server->set_order_type_override("CO-SL");

    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    CHECK(orders.value().front().intent.order_type == broker_exec::domain::OrderType::Market);
    CHECK_FALSE(orders.value().front().intent.trigger_price.has_value());
  }

  SECTION("a plain LIMIT order reports NO trigger (Kite's 0.00 stays nullopt)") {
    broker_exec::clock::TestClock clock;
    OwningKiteAdapter owner(clock, FaultConfig{});

    broker_exec::domain::OrderIntent plain = stop_intent(broker_exec::domain::OrderType::Limit);
    plain.trigger_price.reset();
    REQUIRE(owner.adapter.place(plain).has_value());

    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    CHECK(orders.value().front().intent.order_type == broker_exec::domain::OrderType::Limit);
    // Kite reports trigger_price 0.00 on every non-stop row; reading that as an
    // ENGAGED trigger of zero would make every reconciled limit order look like a
    // stop (and fail the gate's shape check on the next touch).
    CHECK_FALSE(orders.value().front().intent.trigger_price.has_value());
  }

  SECTION("a stop with NO trigger emits no trigger_price field at all") {
    // The adapter's own fail-closed backstop. Falling back to `price` would arm a
    // real stop at the wrong level; omitting the field earns a definitive broker
    // rejection instead. The fixture proves the field never left.
    broker_exec::clock::TestClock clock;
    OwningKiteAdapter owner(clock, FaultConfig{});

    broker_exec::domain::OrderIntent unarmed =
        stop_intent(broker_exec::domain::OrderType::StopLoss);
    unarmed.trigger_price.reset();
    REQUIRE(owner.adapter.place(unarmed).has_value());

    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    CHECK_FALSE(orders.value().front().intent.trigger_price.has_value());
  }
}
