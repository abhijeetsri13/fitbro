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
