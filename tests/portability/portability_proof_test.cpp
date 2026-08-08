// BROKER-PORTABILITY PROOF (Story 6.3, SM-3, AC-1 + AC-2; FR-1/FR-2, IA-1/IA-2,
// CAP-2 reject-only).
//
// AC-1 — THE CONFIG FLIP. One strategy function (tests/portability/
// portable_strategy.cpp, which contains no broker identifier at all) is composed
// twice through `composition::make_broker()`: once with `broker.name = "kite"`
// over the recorded Kite endpoint, once with `broker.name = "kotak"` over the
// recorded Kotak endpoint. The SAME `BrokerDeps` value serves both runs, the same
// `StrategyParams` drives both, and the ONLY difference between the two calls is
// that one string. The assertion is not "both worked" — it is that the two
// `StrategyOutcome` VALUES compare EQUAL, which pins the whole decision path
// (which legs went on, in what order, what broker truth said about them, what the
// position book reported) and not merely the happy-path endpoint.
//
// WHY THE OUTCOME CARRIES NO BROKER ORDER ID: id FORMATS are broker-specific
// ("O1" vs "KOT1"), so comparing them would fail for a reason unrelated to
// portability. `LegOutcome` records whether an id was assigned and whether the
// row came back attributed to us — the properties that must hold everywhere —
// and the ids themselves are asserted separately, as formats, below.
//
// AC-2 — LOAD-TIME REJECTION. The same required-capability list, against the
// DEFAULT (production) capability posture, is admitted for one broker and
// refused for the other, at composition time, with a typed error naming the
// capability. Nothing is constructed and nothing crosses the wire on the refusal
// path — asserted from the recorded servers' own counters.
//
// ── THE FIXTURE-CERTIFICATION POSTURE, STATED PLAINLY ───────────────────────
// The AC-1 run supplies `BrokerOptions::capability_override`. It has to: every
// entry in `kotak_capabilities()` is `Unknown` by design and stays that way until
// the operator-run live min-qty smoke (docs/kotak-min-qty-smoke.md, architecture
// TO-6), and `Unknown` is fail-closed at the gate. The override says "for the
// purposes of THIS recorded-fixture run, treat PlaceOrder as certified". It is
// applied to BOTH runs, symmetrically, so the two compositions still differ in
// exactly one thing: the broker string.
//
// It is a FIXTURE POSTURE, NOT EVIDENCE. It promotes nothing in either adapter's
// real profile, and the AC-2 tests below prove the default path still refuses the
// very same capability against the very same broker. Read the override as a
// statement about this test's substrate, never about a broker.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`, no float.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "broker_exec/adapters/fake/fake_broker.hpp"  // FaultConfig (clean, no faults here)
#include "broker_exec/adapters/kotak/kotak_capabilities.hpp"
#include "broker_exec/adapters/kotak/kotak_session.hpp"
#include "broker_exec/capabilities/capabilities.hpp"
#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/composition/broker_factory.hpp"
#include "broker_exec/config/config.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/result.hpp"

#include "portable_strategy.hpp"
#include "recorded_kite_server.hpp"
#include "recorded_kotak_server.hpp"

namespace caps = broker_exec::capabilities;
namespace comp = broker_exec::composition;

namespace {

using broker_exec::adapters::fake::FaultConfig;
using broker_exec::conformance::kite_fixture::RecordedKiteServer;
using broker_exec::conformance::kotak_fixture::RecordedKotakServer;

// The recorded substrate for one proof run: BOTH brokers' endpoints, both live at
// once, wired into ONE BrokerDeps. That is deliberate — it is what makes "flip the
// config value" a literal one-string change at the call site rather than a
// different wiring per broker.
struct Substrate {
  broker_exec::clock::TestClock clock;
  RecordedKiteServer kite_server{clock, FaultConfig{}};
  RecordedKotakServer kotak_server{clock, FaultConfig{}};
  broker_exec::conformance::kite_fixture::FakeSecretProvider secrets =
      broker_exec::conformance::kite_fixture::make_secrets();

  [[nodiscard]] comp::BrokerDeps deps() {
    comp::BrokerDeps d;
    d.kite_http = &kite_server;
    d.kite_secrets = &secrets;
    d.kotak_http = &kotak_server;
    d.kotak_session =
        [bundle = broker_exec::conformance::kotak_fixture::make_bundle()]()
        -> broker_exec::Result<broker_exec::adapters::kotak::KotakSessionBundle> {
      return bundle;
    };
    return d;
  }
};

// ══════════════════════════════════════════════════════════════════════════
// ██ FIXTURE-CERTIFICATION POSTURE — TEST ONLY. NOT EVIDENCE ABOUT A BROKER ██
// ══════════════════════════════════════════════════════════════════════════
// The capability profile the AC-1 run is composed under. It marks exactly the
// capabilities the strategy declares — nothing else, so it stays fail-closed for
// everything it does not name — and it is applied to BOTH brokers so the two runs
// remain identical apart from the broker string.
//
// It exists because a recorded fixture cannot certify a broker: we authored both
// the request and the response, so a green run proves our adapter agrees with our
// own assumption. Real promotion happens only via the operator-run live min-qty
// smoke. The AC-2 tests below run WITHOUT this override and prove the default
// posture still rejects.
//
// NOTE that it is derived FROM the strategy's own declaration rather than being
// a hand-written list. If the strategy starts needing another capability, this
// grows with it automatically — and, more importantly, it can never certify
// something the strategy did not declare, so the per-call mutation gate still
// has teeth inside the proof run.
[[nodiscard]] comp::FixtureCertification fixture_certified_profile() {
  auto builder = caps::CapabilitySet::builder();
  for (const caps::Capability capability : portable::required_capabilities()) {
    builder.set(capability, caps::Support::Supported);
  }
  return comp::FixtureCertification(builder.build());
}

// The one strategy input, shared by both runs.
[[nodiscard]] portable::StrategyParams make_params() {
  portable::StrategyParams params;
  params.strategy_id = "hedged_short";
  params.hedge_symbol = "NIFTY24JUN23800PE";
  params.short_symbol = "NIFTY24JUN24000PE";
  params.quantity = broker_exec::domain::Quantity::of(50);
  params.hedge_price = broker_exec::domain::Price::from_rupees(41, 25);
  params.short_price = broker_exec::domain::Price::from_rupees(123, 50);
  params.hedge_ref = "hedged_short-1a2b3c4d-deadbeefcafebabe0123456789abcdef";
  params.short_ref = "hedged_short-1a2b3c4d-0123456789abcdefdeadbeefcafebabe";
  return params;
}

// THE CONFIG FLIP, ISOLATED IN ONE PLACE. Everything below the `broker.name`
// assignment is byte-identical between the two runs.
[[nodiscard]] broker_exec::config::Config config_for(const std::string& broker_name) {
  broker_exec::config::Config config;
  config.broker.name = broker_name;
  config.engine.account_id = "ACC-PORTABILITY";
  return config;
}

// The hedge leg the strategy would build, reconstructed here for the one test
// that needs to inspect a raw broker acknowledgement.
[[nodiscard]] broker_exec::domain::OrderIntent hedge_intent(
    const portable::StrategyParams& params) {
  broker_exec::domain::OrderIntent intent;
  intent.client_ref = params.hedge_ref;
  intent.symbol = params.hedge_symbol;
  intent.side = broker_exec::domain::Side::Buy;
  intent.quantity = params.quantity;
  intent.price = params.hedge_price;
  intent.order_type = broker_exec::domain::OrderType::Limit;
  intent.product = broker_exec::domain::Product::Intraday;
  intent.strategy = params.strategy_id;
  return intent;
}

}  // namespace

// ── AC-1: the same strategy, two brokers, one config value ──────────────────

TEST_CASE("portability: the SAME strategy produces the SAME outcome on both brokers",
          "[portability][ac1]") {
  Substrate substrate;
  const comp::BrokerDeps deps = substrate.deps();
  const portable::StrategyParams params = make_params();

  comp::BrokerOptions options;
  options.required_capabilities = portable::required_capabilities();
  options.capability_override = fixture_certified_profile();  // see the banner above

  // ── Run A ──────────────────────────────────────────────────────────────
  auto broker_a = comp::make_broker(config_for("kite"), deps, options);
  REQUIRE(broker_a.has_value());
  const portable::StrategyOutcome outcome_a =
      portable::run(broker_a.value().broker(), params);

  // ── Run B — the ONLY difference is the string on the next line ─────────
  auto broker_b = comp::make_broker(config_for("kotak"), deps, options);
  REQUIRE(broker_b.has_value());
  const portable::StrategyOutcome outcome_b =
      portable::run(broker_b.value().broker(), params);

  // THE HEADLINE ASSERTION. Not "both succeeded" — the whole decision path,
  // compared as one value.
  UNSCOPED_INFO("run A halt_reason: '" << outcome_a.halt_reason << "'");
  UNSCOPED_INFO("run B halt_reason: '" << outcome_b.halt_reason << "'");
  CHECK(outcome_a == outcome_b);

  // NON-VACUITY. Two identical *failures* would also compare equal, so pin what
  // the shared outcome actually is: the full hedge-first path ran to completion
  // on both, with both legs live and attributed at the broker.
  CHECK(outcome_a.completed);
  CHECK(outcome_a.halt_reason.empty());
  CHECK(outcome_a.hedge.placed);
  CHECK(outcome_a.hedge.broker_id_assigned);
  CHECK(outcome_a.hedge.present_in_orderbook);
  CHECK(outcome_a.hedge.attributed_to_us);
  CHECK(outcome_a.short_leg_attempted);
  CHECK(outcome_a.short_leg.placed);
  CHECK(outcome_a.short_leg.broker_id_assigned);
  CHECK(outcome_a.short_leg.present_in_orderbook);
  CHECK(outcome_a.short_leg.attributed_to_us);
  CHECK(outcome_a.orderbook_rows == 2);

  // The legs ended in the same lifecycle state and carried the prices the
  // strategy asked for, on BOTH brokers — the money path translated identically
  // through two completely different wire formats, with no float anywhere.
  CHECK(outcome_a.hedge.state == broker_exec::domain::OrderState::Filled);
  CHECK(outcome_a.short_leg.state == broker_exec::domain::OrderState::Filled);
  CHECK(outcome_a.hedge.filled == params.quantity);
  CHECK(outcome_a.short_leg.filled == params.quantity);
  CHECK(outcome_a.hedge.avg_price == params.hedge_price);
  CHECK(outcome_a.short_leg.avg_price == params.short_price);

  // THE POSITION BOOK IS NON-EMPTY, so comparing it means something. Both
  // recorded endpoints derive the net book from the orders they accepted — one
  // reports a signed net, the other reports buy/sell BUCKETS the adapter has to
  // net itself — and both must land on the same broker-neutral answer.
  CHECK(outcome_a.net_positions == 2);
  CHECK(outcome_a.short_position_found);
  CHECK(outcome_a.short_net_qty == broker_exec::domain::Quantity::of(-50));

  // THE EXIT PATH RAN, on both. An entry strategy whose stand-down is never
  // exercised is an entry strategy with an untested exit.
  CHECK(outcome_a.stand_down_attempted);
  CHECK(outcome_a.stand_down_accepted);

  // BOTH BROKERS REALLY WERE EXERCISED. Without this, a wiring bug that pointed
  // both runs at the same adapter would make the equality above trivially true.
  CHECK(substrate.kite_server.place_count() == 2);
  CHECK(substrate.kite_server.book_size() == 2);
  CHECK(substrate.kotak_server.place_count() == 2);
  CHECK(substrate.kotak_server.book_size() == 2);
}

TEST_CASE("portability: the composed assembly reports which broker it is, and the strategy cannot",
          "[portability][ac1]") {
  Substrate substrate;
  const comp::BrokerDeps deps = substrate.deps();

  comp::BrokerOptions options;
  options.required_capabilities = portable::required_capabilities();
  options.capability_override = fixture_certified_profile();

  auto a = comp::make_broker(config_for("kite"), deps, options);
  auto b = comp::make_broker(config_for("kotak"), deps, options);
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());

  // The composition root knows. That knowledge stops here — what the strategy
  // receives is a `ports::BrokerPort&`, which carries no broker identity at all.
  CHECK(a.value().choice() == comp::BrokerChoice::Kite);
  CHECK(b.value().choice() == comp::BrokerChoice::Kotak);
  CHECK(comp::to_string(a.value().choice()) == "kite");
  CHECK(comp::to_string(b.value().choice()) == "kotak");

  // And both assemblies loudly report that their capabilities are the test
  // fixture posture, not a certified profile.
  CHECK(a.value().capability_override_in_effect());
  CHECK(b.value().capability_override_in_effect());
}

TEST_CASE("portability: broker order-id FORMATS differ, which is exactly why the outcome omits them",
          "[portability][ac1]") {
  // The equality assertion in the AC-1 test is "modulo broker order-id formats".
  // This test makes that caveat concrete rather than leaving it as prose: the two
  // brokers really do mint differently-shaped ids for the same strategy action.
  Substrate substrate;
  const comp::BrokerDeps deps = substrate.deps();

  comp::BrokerOptions options;
  options.required_capabilities = portable::required_capabilities();
  options.capability_override = fixture_certified_profile();

  const portable::StrategyParams params = make_params();

  auto a = comp::make_broker(config_for("kite"), deps, options);
  REQUIRE(a.has_value());
  auto ack_a = a.value().broker().place(hedge_intent(params));
  REQUIRE(ack_a.has_value());

  auto b = comp::make_broker(config_for("kotak"), deps, options);
  REQUIRE(b.has_value());
  auto ack_b = b.value().broker().place(hedge_intent(params));
  REQUIRE(ack_b.has_value());

  // Both non-empty, both echoing our ref back — the portable properties.
  CHECK_FALSE(ack_a.value().broker_order_id.empty());
  CHECK_FALSE(ack_b.value().broker_order_id.empty());
  CHECK(ack_a.value().client_ref == params.hedge_ref);
  CHECK(ack_b.value().client_ref == params.hedge_ref);
  // ...and genuinely different formats, which is the thing a portable strategy
  // must never parse or compare.
  CHECK(ack_a.value().broker_order_id != ack_b.value().broker_order_id);
}

// ── AC-2: load-time rejection, on the DEFAULT capability posture ────────────

TEST_CASE("portability: a required capability is rejected AT LOAD for an uncertified broker",
          "[portability][ac2]") {
  Substrate substrate;
  const comp::BrokerDeps deps = substrate.deps();

  // The strategy's own declared requirements. No override: this is the
  // production posture.
  comp::BrokerOptions options;
  options.required_capabilities = portable::required_capabilities();
  REQUIRE_FALSE(options.required_capabilities.empty());

  SECTION("the certified broker composes") {
    auto made = comp::make_broker(config_for("kite"), deps, options);
    REQUIRE(made.has_value());
    CHECK_FALSE(made.value().capability_override_in_effect());
    CHECK(made.value().capability_set().supports(caps::Capability::PlaceOrder));
  }

  SECTION("the UNCERTIFIED broker is refused, and the error names the capability") {
    auto made = comp::make_broker(config_for("kotak"), deps, options);
    REQUIRE_FALSE(made.has_value());
    CHECK(made.error().category == broker_exec::errors::ErrorCategory::NotSupported);
    CHECK(made.error().action == broker_exec::errors::SuggestedAction::DoNotRetry);
    CHECK(made.error().message.find(caps::to_string(caps::Capability::PlaceOrder)) !=
          std::string::npos);
  }

  SECTION("REFUSED AT LOAD MEANS NOT MID-TRADE: nothing was placed, nothing was sent") {
    // This is the entire point of AC-2. A capability discovered missing while a
    // position is half on is a catastrophe; discovered at wiring time it is a
    // startup error.
    auto made = comp::make_broker(config_for("kotak"), deps, options);
    REQUIRE_FALSE(made.has_value());
    CHECK(substrate.kotak_server.place_count() == 0);
    CHECK(substrate.kotak_server.book_size() == 0);
    CHECK(substrate.kite_server.place_count() == 0);
  }

  SECTION("the refusal is the fail-closed tri-state, not a certified absence") {
    // Every capability on the uncertified broker is `Unknown`, and Unknown reads
    // as unsupported at the gate. Nothing here has been certified ABSENT either —
    // only the operator-run live smoke may move any of these.
    const caps::CapabilitySet profile = broker_exec::adapters::kotak::kotak_capabilities();
    CHECK(profile.support_of(caps::Capability::PlaceOrder) == caps::Support::Unknown);
    CHECK_FALSE(profile.supports(caps::Capability::PlaceOrder));
  }
}

TEST_CASE("portability: an unknown broker name is a fail-closed Validation error",
          "[portability][ac2]") {
  Substrate substrate;
  const comp::BrokerDeps deps = substrate.deps();

  comp::BrokerOptions options;
  options.required_capabilities = portable::required_capabilities();
  options.capability_override = fixture_certified_profile();

  for (const char* bad : {"Kite", "KOTAK", "zerodha", "kotak_neo", ""}) {
    UNSCOPED_INFO("broker.name = '" << bad << "'");
    auto made = comp::make_broker(config_for(bad), deps, options);
    REQUIRE_FALSE(made.has_value());
    CHECK(made.error().category == broker_exec::errors::ErrorCategory::Validation);
    CHECK(made.error().action == broker_exec::errors::SuggestedAction::DoNotRetry);
    CHECK(made.error().message.find("broker.name") != std::string::npos);
  }

  // Nothing reached either broker on any of those attempts.
  CHECK(substrate.kite_server.place_count() == 0);
  CHECK(substrate.kotak_server.place_count() == 0);
}

TEST_CASE("portability: the strategy declares capabilities as DATA, checkable without running it",
          "[portability][ac2]") {
  // The declaration is a value the loader can inspect — which is what allows the
  // check to happen at composition time instead of on the first order.
  const std::vector<caps::Capability> declared = portable::required_capabilities();
  REQUIRE_FALSE(declared.empty());

  // AND IT DECLARES THE EXIT, NOT JUST THE ENTRY. A strategy that lists only
  // PlaceOrder can be admitted onto a broker it cannot get out of.
  const auto declares = [&declared](caps::Capability capability) {
    return std::find(declared.begin(), declared.end(), capability) != declared.end();
  };
  CHECK(declares(caps::Capability::PlaceOrder));
  CHECK(declares(caps::Capability::CancelOrder));
  CHECK(declares(caps::Capability::SquareOff));

  Substrate substrate;
  comp::BrokerOptions options;
  options.required_capabilities = declared;
  auto made = comp::make_broker(config_for("kite"), substrate.deps(), options);
  REQUIRE(made.has_value());

  // Same rule, applied after composition by a second consumer. This assembly
  // carries the broker's REAL profile, so the answer is a genuine verdict.
  CHECK(comp::assert_production_posture(made.value()).has_value());
  CHECK(comp::require_capabilities(made.value(), declared).has_value());
}

TEST_CASE("portability: an EMPTY capability declaration cannot compose a broker",
          "[portability][ac2]") {
  // The gate is only as strong as the declaration feeding it, and `require_all`
  // over an empty list is vacuously ok(). Composing with a default-constructed
  // BrokerOptions must therefore be refused outright — otherwise "declare
  // nothing" would be the cheapest route to an ungated port on the very broker
  // AC-2 exists to keep orders away from.
  Substrate substrate;
  auto made = comp::make_broker(config_for("kotak"), substrate.deps(), comp::BrokerOptions{});
  REQUIRE_FALSE(made.has_value());
  CHECK(made.error().category == broker_exec::errors::ErrorCategory::Validation);
  CHECK(substrate.kotak_server.place_count() == 0);
}

TEST_CASE("portability: the proof run's fixture posture is quarantined from production checks",
          "[portability][ac2][testonly]") {
  // The AC-1 run is composed under a FixtureCertification. That posture must not
  // be able to answer a production question: `assert_production_posture()` fails
  // it, and `require_capabilities()` refuses rather than laundering a test's
  // assertion into a broker fact.
  Substrate substrate;
  comp::BrokerOptions options;
  options.required_capabilities = portable::required_capabilities();
  options.capability_override = fixture_certified_profile();

  auto made = comp::make_broker(config_for("kotak"), substrate.deps(), options);
  REQUIRE(made.has_value());

  CHECK_FALSE(comp::assert_production_posture(made.value()).has_value());
  CHECK_FALSE(
      comp::require_capabilities(made.value(), portable::required_capabilities()).has_value());
}
