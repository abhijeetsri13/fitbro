// Kotak Neo adapter conformance (Story 6.2, AC-1; FR-1/FR-37, NFR-3, TO-6). The
// twin of tests/conformance/kite_conformance_test.cpp: it runs the SAME
// broker-agnostic conformance kit that certifies the FakeBroker (Epic 1) and the
// Kite adapter (Epic 2) against the REAL KotakBrokerAdapter, driven by a stateful,
// fault-injecting recorded Kotak HTTP endpoint. The kit is reused VERBATIM — only
// the BrokerFactory differs — proving the zero-duplicate invariant holds for Kotak.
//
// TIER-1 ONLY, AND THAT IS THE POINT: this runs in CI with a synthetic session
// bundle + the in-memory RecordedKotakServer (NO network, NO live credentials).
// Passing here does NOT flip any entry in `kotak_capabilities()` — every one of
// them stays `Unknown`, because a fixture we authored ourselves cannot certify an
// endpoint we have never contacted. The tier-2 gate that DOES flip them is the
// operator-run live min-qty smoke in docs/kotak-min-qty-smoke.md (architecture TO-6).
//
// WHY THE NON-KIT ASSERTIONS AT THE BOTTOM MATTER: Kotak has no verified client-tag
// echo, so the adapter recovers an ack-lost order by ATTRIBUTE CORROBORATION. Two
// hazards follow, and both are asserted explicitly rather than assumed:
//   * VACUITY — the kit counts duplicates via `o.intent.client_ref == client_ref`,
//     so an adapter that recovered an EMPTY client_ref would report zero duplicates
//     for free. We assert the exact minted ref comes back, AND that it never went
//     out on the wire (so the recovery is genuinely corroboration, not an echo).
//   * FALSE POSITIVES — attribute corroboration must claim NOTHING when the
//     pairing is ambiguous. A colliding manual order proves the fail-closed side.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`, no float.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "broker_exec/adapters/fake/fake_broker.hpp"  // FaultConfig (the fault selector)
#include "broker_exec/adapters/kotak/kotak_broker_adapter.hpp"
#include "broker_exec/adapters/kotak/kotak_capabilities.hpp"
#include "broker_exec/adapters/kotak/kotak_rest_client.hpp"
#include "broker_exec/adapters/kotak/kotak_session.hpp"
#include "broker_exec/adapters/kotak/kotak_transport.hpp"
#include "broker_exec/capabilities/capabilities.hpp"
#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/domain/decimal_paise.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/lifecycle/lifecycle.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/clock_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/runtime/unknown_resolver.hpp"
#include "broker_exec/store/store.hpp"
#include "conformance_kit.hpp"
#include "recorded_kotak_server.hpp"

namespace conf = broker_exec::conformance;

namespace {

using broker_exec::adapters::fake::FaultConfig;

// THE RECORDED KOTAK SERVER, THE OWNING ADAPTER STACK AND THE BROKER-TRUTH TALLY
// NOW LIVE IN tests/conformance/recorded_kotak_server.hpp. They moved there
// VERBATIM in Story 6.3 so the broker-portability proof (tests/portability/)
// drives the SAME fixture instead of a copy that would quietly drift from this
// one — a portability claim proven against a stale fixture is worth nothing.
// Nothing about this suite's behavior changed with the move; the class comments
// explaining the fault model, the read-throttle deviation and the fixture
// provenance travelled with the code.
using broker_exec::conformance::kotak_fixture::ConformanceTally;
using broker_exec::conformance::kotak_fixture::OwningKotakAdapter;

// The Kotak BrokerFactory: per scenario, a fresh recorded server for the
// FaultConfig, a KotakRestClient over it + a synthetic session bundle, and a
// KotakBrokerAdapter — all owned by the returned BrokerPort. `tally` (optional)
// receives each scenario's broker-truth counters at teardown.
conf::BrokerFactory kotak_factory(ConformanceTally* tally = nullptr) {
  return [tally](broker_exec::ports::ClockPort& clock,
                 FaultConfig fault) -> std::unique_ptr<broker_exec::ports::BrokerPort> {
    return std::make_unique<OwningKotakAdapter>(clock, fault, tally);
  };
}

// The canonical intent used by the direct (non-kit) tests, with a KNOWN,
// non-empty client_ref in the "<strategy>-<sig8>-<uuid>" shape the dispatcher
// mints (Story 1.7). Recovery must hand this EXACT ref back.
constexpr const char* kClientRef = "alpha-1a2b3c4d-deadbeefcafebabe0123456789abcdef";

[[nodiscard]] broker_exec::domain::OrderIntent make_intent() {
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
}

// A LEGITIMATE opposite-side order of the same shape, belonging to a DIFFERENT
// signal: the second leg of a hedge pair, a reversal, or another strategy's
// position under 6-4 isolation. Nothing about it is anomalous, and that is the
// point — it must never be mistaken for a square-off exit.
[[nodiscard]] broker_exec::domain::OrderIntent hedge_buy() {
  broker_exec::domain::OrderIntent intent = make_intent();
  intent.client_ref = "hedge-9f8e7d6c-00112233445566778899aabbccddeeff";
  intent.side = broker_exec::domain::Side::Buy;
  intent.strategy = "hedge";
  return intent;
}

}  // namespace

// ── AC-1: the kit, verbatim, across the whole fault matrix ───────────────────

TEST_CASE("conformance: the Kotak adapter passes the full fault matrix with zero duplicates",
          "[conformance][kotak]") {
  ConformanceTally tally;
  const conf::ConformanceReport report = conf::run_conformance(kotak_factory(&tally));

  // Scoped, so the reason survives to whichever assertion below actually fires.
  INFO("kotak conformance failures:" << conf::failure_digest(report.failures));
  CHECK(report.failures.empty());

  CHECK(report.scenarios_run > 0);
  CHECK(report.duplicate_orders == 0);
  CHECK(report.scenarios_passed == report.scenarios_run);
  CHECK(report.ok());

  // ── The duplicate check that actually measures duplicates ────────────────
  // Everything above is the kit's view, which counts rows BY CLIENT_REF. For an
  // adapter without a broker tag echo that metric can read zero simply because
  // correlation failed — see the ConformanceTally comment. These assertions read
  // the recorded broker's own state instead, so a real duplicate cannot hide
  // behind an ambiguous attribute key.
  REQUIRE(tally.book_sizes.size() == static_cast<std::size_t>(report.scenarios_run));
  REQUIRE(tally.place_counts.size() == static_cast<std::size_t>(report.scenarios_run));

  std::size_t total_places = 0;
  for (std::size_t i = 0; i < tally.book_sizes.size(); ++i) {
    // INFO, not UNSCOPED_INFO: two CHECKs follow inside this iteration, and the
    // first would clear an unscoped message before the second could print it.
    INFO("scenario #" << i << ": wire places=" << tally.place_counts[i]
                      << " broker book=" << tally.book_sizes[i]);
    // ZERO DUPLICATES, measured from broker truth: at most ONE order exists at the
    // broker per scenario, whatever the caller managed (or failed) to correlate.
    CHECK(tally.book_sizes[i] <= 1);
    // NO BLIND RETRY, measured at the wire: at most one place crossed it.
    CHECK(tally.place_counts[i] <= 1);
    total_places += tally.place_counts[i];
  }
  // Every scenario placed EXACTLY once — no scenario silently skipped its place
  // (which would make the zero-duplicate result vacuous) and none placed twice.
  CHECK(total_places == static_cast<std::size_t>(report.scenarios_run));
}

TEST_CASE("conformance: every scenario in the matrix is exercised against Kotak",
          "[conformance][kotak]") {
  const conf::ConformanceReport report = conf::run_conformance(kotak_factory());
  CHECK(report.scenarios_run == 7);
}

// ── Explicit non-vacuity: recovery + the no-blind-retry place count ──────────
// These exercise the adapter directly (NOT via run_conformance) to prove two
// things the kit cannot prove for a real adapter by itself. Story 2.14's review
// closed exactly this hole for Kite; it is not reopened here.

TEST_CASE("[conformance][kotak][recovery] an ack-lost order is recovered with its client_ref",
          "[conformance][kotak][recovery]") {
  SECTION("ack-lost place is recovered by ATTRIBUTE CORROBORATION, not a tag echo") {
    // ack_lost_but_placed: the order IS live at the broker but the caller sees a
    // transport failure — the headline duplicate-risk the safety core must survive.
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{.ack_lost_but_placed = true});

    const broker_exec::domain::OrderIntent intent = make_intent();

    // place() returns an Error (ack lost). The dispatcher would mark this UNKNOWN
    // and reconcile — never blindly retry.
    auto placed = owner.adapter.place(intent);
    REQUIRE_FALSE(placed.has_value());

    // NO BLIND RETRY: exactly ONE wire place per place() call. The only legitimate
    // re-fire is the dispatcher's, after reserve().
    CHECK(owner.server->place_count() == 1);
    // ...and the order really is live at the broker despite the failed ack.
    CHECK(owner.server->book_size() == 1);

    // NON-VACUITY, PART 1: the client_ref never went out on the wire, so anything
    // that comes back cannot be a broker echo — it has to be corroboration.
    REQUIRE(owner.server->place_bodies().size() == 1);
    CHECK(owner.server->place_bodies().front().find(kClientRef) == std::string::npos);

    // NON-VACUITY, PART 2: fetch_orders() recovers the live order AND its exact
    // originating client_ref. An adapter returning an empty ref would pass the
    // kit's duplicate count vacuously; this closes that hole.
    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    const broker_exec::domain::Order& recovered = orders.value().front();
    CHECK_FALSE(recovered.intent.client_ref.empty());
    CHECK(recovered.intent.client_ref == kClientRef);
    CHECK(recovered.state == broker_exec::domain::OrderState::Filled);

    // ANCHOR-ON-CORROBORATION: the weak rung ran once and promoted itself to the
    // strong id rung, so the trade book now correlates by broker order id.
    auto trades = owner.adapter.fetch_trades();
    REQUIRE(trades.has_value());
    REQUIRE(trades.value().size() == 1);
    CHECK(trades.value().front().client_ref == kClientRef);
    CHECK(trades.value().front().broker_order_id == recovered.broker_order_id);
  }

  SECTION("clean place is recovered from the broker-order-id map (the strong rung)") {
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});

    const broker_exec::domain::OrderIntent intent = make_intent();

    auto placed = owner.adapter.place(intent);
    REQUIRE(placed.has_value());
    CHECK_FALSE(placed.value().broker_order_id.empty());
    CHECK(placed.value().client_ref == kClientRef);
    CHECK(owner.server->place_count() == 1);  // still exactly one wire place

    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    const broker_exec::domain::Order& recovered = orders.value().front();
    CHECK(recovered.intent.client_ref == kClientRef);
    CHECK(recovered.broker_order_id == placed.value().broker_order_id);
  }
}

TEST_CASE("[conformance][kotak][recovery] a colliding manual order fails CLOSED, never a guess",
          "[conformance][kotak][recovery]") {
  // An operator (or another process) already has an order at the broker with the
  // SAME symbol/side/quantity as ours, and OUR ack is lost. The attribute rung is
  // now ambiguous, and the only safe answer is to claim NOTHING: an order attached
  // to the WRONG signal is worse than an unresolved one, which merely stays UNKNOWN
  // under an operator alert.
  broker_exec::clock::TestClock clock;
  OwningKotakAdapter owner(clock, FaultConfig{.ack_lost_but_placed = true});
  owner.server->seed_manual_order("NIFTY24JUN24000CE", "S", 50, "123.50");

  auto placed = owner.adapter.place(make_intent());
  REQUIRE_FALSE(placed.has_value());
  CHECK(owner.server->place_count() == 1);

  auto orders = owner.adapter.fetch_orders();
  REQUIRE(orders.has_value());
  REQUIRE(orders.value().size() == 2);  // the manual order plus our ack-lost one

  int claimed = 0;
  for (const broker_exec::domain::Order& o : orders.value()) {
    if (o.intent.client_ref == kClientRef) {
      ++claimed;
    }
  }
  // ZERO, not one and emphatically not two: an ambiguous pairing resolves to
  // nothing at all. (Two would be the shape that registers as a duplicate against
  // a single signal — the invariant NFR-3 forbids.)
  CHECK(claimed == 0);

  // The order is still enumerable at the broker; it is simply UNATTRIBUTED, which
  // is what drives the UnknownResolver to its fail-closed operator alert.
  CHECK(owner.server->book_size() == 2);

  // And the correlation tuple is WITHHELD on both uncorrelated rows, so the
  // resolver's own first-match-wins attribute rung cannot overturn this refusal
  // one layer up (pinned end-to-end by the [stack] test below).
  for (const broker_exec::domain::Order& o : orders.value()) {
    CHECK(o.intent.quantity == broker_exec::domain::Quantity::of(0));
    CHECK(o.intent.price == broker_exec::domain::Price::from_paise(0));
  }
}

TEST_CASE("[conformance][kotak][recovery] a DEFINITIVELY rejected intent stops attracting matches",
          "[conformance][kotak][recovery]") {
  // THE BUG THIS PINS: registration happens before the wire call, so a place that
  // the broker DEFINITIVELY refused (HTTP-200 `Not_Ok`, nothing live, book empty)
  // used to leave its shape registered forever. An operator then placing an
  // identical lot by hand would have that order silently adopted as ours — we
  // would be "managing" a position we never opened, and the real one would go
  // unmanaged. A definitive verdict must retire the registration.
  broker_exec::clock::TestClock clock;
  OwningKotakAdapter owner(clock, FaultConfig{});
  owner.server->set_hard_reject(true);

  const broker_exec::domain::OrderIntent intent = make_intent();
  auto placed = owner.adapter.place(intent);
  REQUIRE_FALSE(placed.has_value());
  // A definitive verdict, NOT an ambiguous one: nothing can be live.
  CHECK(placed.error().action == broker_exec::errors::SuggestedAction::DoNotRetry);
  CHECK(owner.server->book_size() == 0);

  // Now the operator places an identical lot by hand.
  owner.server->set_hard_reject(false);
  owner.server->seed_manual_order("NIFTY24JUN24000CE", "S", 50, "123.50");

  auto orders = owner.adapter.fetch_orders();
  REQUIRE(orders.has_value());
  REQUIRE(orders.value().size() == 1);
  const broker_exec::domain::Order& manual = orders.value().front();

  // It is REPORTED (never hide a broker order) but NOT CLAIMED.
  CHECK(manual.intent.client_ref.empty());
  CHECK_FALSE(manual.broker_order_id.empty());
  // ...and without the correlation tuple, so the resolver cannot claim it either.
  CHECK(manual.intent.quantity == broker_exec::domain::Quantity::of(0));
}

TEST_CASE("[conformance][kotak][stack] the colliding-manual refusal holds at the STACK level",
          "[conformance][kotak][stack]") {
  // The adapter refusing to name a row is only half the story: `UnknownResolver`
  // runs its OWN attribute corroboration (rung 3) on (symbol, side, quantity,
  // price), first-match-wins and with no ambiguity check. This test runs the two
  // together and pins what the STACK does — because "the adapter returns an empty
  // ref" is not by itself a safety guarantee.
  namespace fs = std::filesystem;
  namespace rt = broker_exec::runtime;

  std::error_code ec;
  const fs::path datadir = fs::temp_directory_path() / "broker_exec_kotak_stack_collide";
  fs::remove_all(datadir, ec);
  fs::create_directories(datadir, ec);

  const broker_exec::domain::OrderIntent intent = make_intent();

  // The local UNKNOWN order the dispatcher would have recorded for an ack-lost
  // place: full intent, no broker_order_id (the ack never came back).
  const auto make_unknown_order = [&intent] {
    broker_exec::domain::Order order;
    order.intent = intent;
    order.state = broker_exec::domain::OrderState::Unknown;
    return order;
  };

  SECTION("with a colliding manual order the stack stays UNKNOWN and alerts") {
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{.ack_lost_but_placed = true});
    owner.server->seed_manual_order("NIFTY24JUN24000CE", "S", 50, "123.50");

    auto store_opened = broker_exec::store::Store::open((datadir / "collide.db").string());
    REQUIRE(store_opened.has_value());
    broker_exec::store::Store store = std::move(store_opened.value());
    broker_exec::lifecycle::LifecycleEngine fsm;
    conf::detail::CountingAlertSink alerts;
    rt::UnknownResolver resolver(owner, store, fsm, alerts, clock);

    REQUIRE_FALSE(owner.adapter.place(intent).has_value());
    const broker_exec::domain::Order unknown_order = make_unknown_order();
    REQUIRE(store.insert_order(unknown_order).has_value());

    auto resolution = resolver.resolve(unknown_order);
    REQUIRE(resolution.has_value());

    // FAIL-CLOSED, end to end: no rung matched, the order stays UNKNOWN, and the
    // operator is alerted. Without the adapter withholding the correlation tuple
    // the resolver would have adopted whichever colliding row it saw first.
    CHECK(resolution.value().kind == rt::MatchKind::NoMatch);
    CHECK_FALSE(resolution.value().resolved_ok);
    CHECK(resolution.value().new_state == broker_exec::domain::OrderState::Unknown);
    CHECK(alerts.count() > 0);
  }

  SECTION("without a collision the stack DOES resolve (the refusal is not blanket)") {
    // The control that keeps the section above from being vacuous: suppression
    // must cost us nothing when the pairing is unambiguous.
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{.ack_lost_but_placed = true});

    auto store_opened = broker_exec::store::Store::open((datadir / "clean.db").string());
    REQUIRE(store_opened.has_value());
    broker_exec::store::Store store = std::move(store_opened.value());
    broker_exec::lifecycle::LifecycleEngine fsm;
    conf::detail::CountingAlertSink alerts;
    rt::UnknownResolver resolver(owner, store, fsm, alerts, clock);

    REQUIRE_FALSE(owner.adapter.place(intent).has_value());
    const broker_exec::domain::Order unknown_order = make_unknown_order();
    REQUIRE(store.insert_order(unknown_order).has_value());

    auto resolution = resolver.resolve(unknown_order);
    REQUIRE(resolution.has_value());
    CHECK(resolution.value().resolved_ok);
    // HONEST-LABELLING CAVEAT (documented, not encoded): the resolver reports
    // CorrelationToken because the adapter stamped the recovered ref onto the row —
    // but NO broker echoed anything. The evidence was attribute corroboration. On
    // Kotak, read CORRELATION_TOKEN as "order id OR corroborated attributes" until
    // TagCarry is resolved live. There is no field on domain::Order to say
    // "weak match" without rippling through ports/store/lifecycle.
    CHECK(resolution.value().kind == rt::MatchKind::CorrelationToken);
  }

  fs::remove_all(datadir, ec);
}

TEST_CASE("[conformance][kotak][state] Kotak status mapping is fail-closed and quantity-driven",
          "[conformance][kotak][state]") {
  const broker_exec::domain::OrderIntent intent = make_intent();

  SECTION("a partial fill on a working order is PartiallyFilled") {
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->set_fill_model(20, "open");

    REQUIRE(owner.adapter.place(intent).has_value());
    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    CHECK(orders.value().front().state == broker_exec::domain::OrderState::PartiallyFilled);
    CHECK(orders.value().front().filled_qty == broker_exec::domain::Quantity::of(20));
  }

  SECTION("a broker that SAYS complete while reporting a short fill is still PartiallyFilled") {
    // The fillnorm rule: drive off the filled QUANTITY, never the event/status
    // label. Believing the label here would under-report the live remainder.
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->set_fill_model(20, "complete");

    REQUIRE(owner.adapter.place(intent).has_value());
    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    CHECK(orders.value().front().state == broker_exec::domain::OrderState::PartiallyFilled);
  }

  SECTION("'not cancelled' is a WORKING order, not a terminal cancel") {
    // Kotak's vocabulary contains "not cancelled" / "cancel pending": a cancel
    // request that did NOT take effect. A naive substring match on "cancel" would
    // report the engine flat while the order is live at the exchange.
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->set_fill_model(0, "Not Cancelled");

    REQUIRE(owner.adapter.place(intent).has_value());
    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    CHECK(orders.value().front().state == broker_exec::domain::OrderState::Acknowledged);
  }

  SECTION("an UNRECOGNISED status maps to Unknown (fail-closed), never a guess") {
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->set_fill_model(50, "SOME_FUTURE_KOTAK_STATE");

    REQUIRE(owner.adapter.place(intent).has_value());
    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    // Note it is Unknown DESPITE a full filled quantity: an unrecognised status is
    // not evidence we may reason from, so the engine is forced to reconcile.
    CHECK(orders.value().front().state == broker_exec::domain::OrderState::Unknown);
  }

  SECTION("a WORKING order whose total arrives under an unexpected key is NOT Filled") {
    // THE BUG THIS PINS: `qty` absent used to default to 0, so fillnorm computed
    // pending = max(0, 0 - 30) = 0 and read "filled>0 && pending==0" as FILLED —
    // a terminal, ABSORBING state in the lifecycle FSM. A live 30-of-50 working
    // order would have been permanently marked done because one field name
    // differed. Absent is not zero: with no total, a working status may never
    // produce a terminal state.
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->set_total_field("quantity");  // a spelling the adapter does not know
    owner.server->set_fill_model(30, "open");

    REQUIRE(owner.adapter.place(intent).has_value());
    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    const broker_exec::domain::Order& row = orders.value().front();
    CHECK(row.state != broker_exec::domain::OrderState::Filled);
    CHECK(row.state == broker_exec::domain::OrderState::PartiallyFilled);
    CHECK(row.filled_qty == broker_exec::domain::Quantity::of(30));
  }

  SECTION("a WORKING order with an unknown total and no fill is Acknowledged, not Filled") {
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->set_total_field("quantity");
    owner.server->set_fill_model(0, "open");

    REQUIRE(owner.adapter.place(intent).has_value());
    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    CHECK(orders.value().front().state == broker_exec::domain::OrderState::Acknowledged);
  }

  SECTION("an impossible over-fill is clamped to the ordered quantity") {
    // A broker reporting fldQty=500 against qty=50 is reporting garbage, and the
    // dangerous direction is obvious: sizing an exit off 500 sells ten times the
    // position. Cap the fill at what was actually ordered.
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->set_fill_model(500, "complete");

    REQUIRE(owner.adapter.place(intent).has_value());
    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    CHECK(orders.value().front().filled_qty == broker_exec::domain::Quantity::of(50));
    CHECK(orders.value().front().state == broker_exec::domain::OrderState::Filled);
  }

  SECTION("a row carrying UNPARSEABLE money fails closed to Unknown") {
    // Money is never read best-effort: a truncating parse would have read
    // "12.3.4.5" as 1230 paise and reported a confident wrong price.
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->seed_manual_order("INFY-EQ", "B", 1, "12.3.4.5");

    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    // Despite a perfectly recognisable "open" status, the row we cannot read is
    // Unknown — the engine must reconcile rather than trust our parsing.
    CHECK(orders.value().front().state == broker_exec::domain::OrderState::Unknown);
    CHECK(orders.value().front().intent.price == broker_exec::domain::Price::from_paise(0));
  }

  SECTION("a 20-digit money field is a parse failure, not an overflow") {
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->seed_manual_order("INFY-EQ", "B", 1, "99999999999999999999.99");

    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    CHECK(orders.value().front().state == broker_exec::domain::OrderState::Unknown);
  }

  SECTION("observed casings and separators all map to the same state") {
    for (const char* status : {"COMPLETE", "Complete", "complete"}) {
      broker_exec::clock::TestClock clock;
      OwningKotakAdapter owner(clock, FaultConfig{});
      owner.server->set_fill_model(-1, status);

      REQUIRE(owner.adapter.place(intent).has_value());
      auto orders = owner.adapter.fetch_orders();
      REQUIRE(orders.has_value());
      REQUIRE(orders.value().size() == 1);
      CHECK(orders.value().front().state == broker_exec::domain::OrderState::Filled);
    }
    for (const char* status : {"TRIGGER PENDING", "trigger_pending", "Trigger-Pending"}) {
      broker_exec::clock::TestClock clock;
      OwningKotakAdapter owner(clock, FaultConfig{});
      owner.server->set_fill_model(0, status);

      REQUIRE(owner.adapter.place(intent).has_value());
      auto orders = owner.adapter.fetch_orders();
      REQUIRE(orders.has_value());
      REQUIRE(orders.value().size() == 1);
      CHECK(orders.value().front().state == broker_exec::domain::OrderState::Acknowledged);
    }
  }
}

TEST_CASE("[conformance][kotak][reject] an HTTP-200 Not_Ok is a rejection, and records nothing",
          "[conformance][kotak][reject]") {
  // The Kotak trap: a status-code-only client reads this as success and believes
  // it has a live order. It must surface as a typed failure with an EMPTY book.
  broker_exec::clock::TestClock clock;
  OwningKotakAdapter owner(clock, FaultConfig{});
  owner.server->set_hard_reject(true);

  auto placed = owner.adapter.place(make_intent());
  REQUIRE_FALSE(placed.has_value());
  CHECK(placed.error().category == broker_exec::errors::ErrorCategory::InsufficientFunds);
  CHECK(owner.server->place_count() == 1);  // still no retry on a hard reject
  CHECK(owner.server->book_size() == 0);    // nothing reached the book

  auto orders = owner.adapter.fetch_orders();
  REQUIRE(orders.has_value());
  CHECK(orders.value().empty());
}

TEST_CASE("[conformance][kotak][money] prices round-trip as integer paise, never a float",
          "[conformance][kotak][money]") {
  broker_exec::clock::TestClock clock;
  OwningKotakAdapter owner(clock, FaultConfig{});

  broker_exec::domain::OrderIntent intent = make_intent();
  intent.price = broker_exec::domain::Price::from_rupees(1450, 5);  // 1450.05 -> 145005 paise

  REQUIRE(owner.adapter.place(intent).has_value());
  // The wire body carries the decimal TEXT form, produced without any float.
  REQUIRE(owner.server->place_bodies().size() == 1);
  CHECK(owner.server->place_bodies().front().find("1450.05") != std::string::npos);

  auto orders = owner.adapter.fetch_orders();
  REQUIRE(orders.has_value());
  REQUIRE(orders.value().size() == 1);
  CHECK(orders.value().front().avg_price == broker_exec::domain::Price::from_paise(145005));
  CHECK(orders.value().front().intent.price == broker_exec::domain::Price::from_paise(145005));

  auto funds = owner.adapter.fetch_funds();
  REQUIRE(funds.has_value());
  CHECK(funds.value().available_margin == broker_exec::domain::Money::from_paise(10000000));
  CHECK(funds.value().used_margin == broker_exec::domain::Money::from_paise(0));
}

TEST_CASE("[conformance][kotak][money] the decimal->paise parser is fail-closed and overflow-safe",
          "[conformance][kotak][money]") {
  using broker_exec::domain::paise_to_decimal;
  using broker_exec::domain::parse_decimal_paise;
  using broker_exec::domain::parse_int64;

  // Exact values.
  CHECK(parse_decimal_paise("1450.05") == std::optional<std::int64_t>{145005});
  CHECK(parse_decimal_paise("1450.5") == std::optional<std::int64_t>{145050});  // zero-padded
  CHECK(parse_decimal_paise("1450") == std::optional<std::int64_t>{145000});
  CHECK(parse_decimal_paise("-7.25") == std::optional<std::int64_t>{-725});
  CHECK(parse_decimal_paise("  12.34  ") == std::optional<std::int64_t>{1234});
  CHECK(parse_decimal_paise("1.239") == std::optional<std::int64_t>{123});  // sub-paise truncated

  // FAIL-CLOSED: anything we cannot read exactly is nullopt, never a number. A
  // truncating parser read each of these as a confident wrong value.
  CHECK_FALSE(parse_decimal_paise("12.3.4.5").has_value());
  CHECK_FALSE(parse_decimal_paise("abc").has_value());
  CHECK_FALSE(parse_decimal_paise("12abc").has_value());
  CHECK_FALSE(parse_decimal_paise("1.2x9").has_value());
  CHECK_FALSE(parse_decimal_paise("").has_value());
  CHECK_FALSE(parse_decimal_paise(".").has_value());
  CHECK_FALSE(parse_decimal_paise("-").has_value());

  // OVERFLOW is a parse failure, not undefined behaviour. A 20-digit field is a
  // thing a broker can send, and wrapping it silently produces a plausible-looking
  // negative price.
  CHECK_FALSE(parse_decimal_paise("99999999999999999999.99").has_value());
  CHECK_FALSE(parse_decimal_paise("92233720368547758.08").has_value());  // just past the scaling
  CHECK_FALSE(parse_int64("99999999999999999999").has_value());
  CHECK(parse_int64("50") == std::optional<std::int64_t>{50});
  CHECK_FALSE(parse_int64("50 lots").has_value());  // trailing garbage FAILS, never truncates

  // Round-trip, including the INT64_MIN edge the naive negate would overflow on.
  CHECK(paise_to_decimal(145005) == "1450.05");
  CHECK(paise_to_decimal(-725) == "-7.25");
  CHECK(paise_to_decimal(5) == "0.05");
  CHECK_FALSE(paise_to_decimal(std::numeric_limits<std::int64_t>::min()).empty());
}

TEST_CASE("[conformance][kotak][IMP-13] square_off FLATTENS, at parity with Kite",
          "[conformance][kotak][IMP-13][squareoff]") {
  // HISTORY, because the contract changed twice: this first issued a cancel and
  // returned ok (against a FILLED position a cancel is a no-op, so the caller was
  // told "you are flat" while the position was still on), then a typed
  // NotSupported refusal, and now — IMP-13 — a real flatten. AC-3 is parity: the
  // same protocol as Kite, over the Kotak wire.
  //
  // NOTE the composition that still guards this: `SquareOff` remains `Unknown` in
  // kotak_capabilities() until the tier-2 live runbook, and the per-call
  // capability gate (Story 6.3) refuses an uncertified mutation. The adapter
  // having an implementation does NOT by itself let a live square-off through.
  SECTION("a partially filled parent is cancelled AND exited for exactly the fill") {
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->set_fill_model(30, "open");  // 30 of 50 done, 20 still working

    auto placed = owner.adapter.place(make_intent());
    REQUIRE(placed.has_value());

    REQUIRE(owner.adapter.square_off(placed.value().broker_order_id).has_value());

    const auto book = owner.server->placed_orders();
    REQUIRE(book.size() == 2);
    // Opposite side, sized off the CANONICAL fill — not the order total, which
    // would leave a naked remainder once the working part is cancelled.
    CHECK(book.back().side == "B");
    CHECK(book.back().symbol == "NIFTY24JUN24000CE");
    // THE SIZE, ASSERTED. Without this the section passes for an exit sized off
    // the ORDER TOTAL (50) — the exact bug the section's own comment describes —
    // because side and symbol are identical either way. 30 is what executed.
    CHECK(book.back().qty == 30);
    CHECK(book.back().price_type == "MKT");
  }

  SECTION("a cancel the broker REFUSES as already-terminal is tolerated (AC-1b)") {
    // The Kite twin, over the Kotak wire — and it had no Kotak counterpart at all.
    // A PARTIAL fill is what makes the cancel real: the broker says `complete`
    // while reporting 30 of 50, the adapter's quantity-first reading calls that
    // PartiallyFilled (a live remainder), so it issues the cancel — and broker
    // truth, which still holds a terminal row, refuses it. That refusal IS the
    // state we were cancelling into, so the flatten must shrug it off.
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->set_fill_model(30, "complete");
    owner.server->set_cancel_rejects_terminal(true);

    auto placed = owner.adapter.place(make_intent());
    REQUIRE(placed.has_value());

    REQUIRE(owner.adapter.square_off(placed.value().broker_order_id).has_value());

    // NON-VACUITY: a cancel really was issued and really was refused.
    CHECK(owner.server->cancel_refusals() == 1);

    const auto book = owner.server->placed_orders();
    REQUIRE(book.size() == 2);
    CHECK(book.back().side == "B");
    CHECK(book.back().qty == 30);
  }

  SECTION("the exit echoes the parent's PRODUCT and SEGMENT from broker truth") {
    // Exiting an NRML position with an MIS order opens a SECOND position in a
    // different margin bucket and leaves the overnight carry leg untouched; a
    // re-inferred segment would additionally re-run the very symbol heuristic this
    // story is retiring. Both are echoed verbatim from the parent row instead.
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});

    broker_exec::domain::OrderIntent carry = make_intent();
    carry.product = broker_exec::domain::Product::Normal;  // NRML, not the MIS default
    auto placed = owner.adapter.place(carry);
    REQUIRE(placed.has_value());
    REQUIRE(owner.adapter.square_off(placed.value().broker_order_id).has_value());

    const auto book = owner.server->placed_orders();
    REQUIRE(book.size() == 2);
    REQUIRE(book.front().product == "NRML");
    CHECK(book.back().product == "NRML");
    CHECK(book.back().segment == book.front().segment);
    CHECK(book.back().side == "B");
    CHECK(book.back().qty == 50);
  }

  SECTION("a post-restart replay finds the exit by ATTRIBUTE corroboration (rung 2)") {
    // The rung that actually carries the restart case, and it had no test. A fresh
    // adapter over the SAME server has an EMPTY id map, so rung 1 cannot fire and
    // only broker truth remains — this is the crash-replay shape, and on Kotak
    // (no verified tag echo) rung 2 is the only thing standing between a panic
    // restart and a second, opposite naked leg.
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->set_fill_model(50, "complete");

    auto placed = owner.adapter.place(make_intent());
    REQUIRE(placed.has_value());
    REQUIRE(owner.adapter.square_off(placed.value().broker_order_id).has_value());
    REQUIRE(owner.server->placed_orders().size() == 2);

    broker_exec::adapters::kotak::KotakBrokerAdapter restarted(owner.rest);
    REQUIRE(restarted.square_off(placed.value().broker_order_id).has_value());
    CHECK(owner.server->placed_orders().size() == 2);
  }

  SECTION("more than one candidate exit is INDETERMINATE, never a third order") {
    // Rung 2's bound: two plausible exits is not a coin flip. Both seeded rows are
    // FOREIGN (nothing this adapter placed, so bound to no client_ref), which is
    // exactly the population rung 2 is allowed to consider — and it must refuse
    // rather than pick one and report the position flat.
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->set_fill_model(50, "complete");

    auto placed = owner.adapter.place(make_intent());
    REQUIRE(placed.has_value());
    owner.server->seed_manual_order("NIFTY24JUN24000CE", "B", 50, "123.50");
    owner.server->seed_manual_order("NIFTY24JUN24000CE", "B", 20, "123.50");

    auto squared = owner.adapter.square_off(placed.value().broker_order_id);
    REQUIRE_FALSE(squared.has_value());
    CHECK(squared.error().broker_code == "KOTAK-SQUAREOFF-AMBIGUOUS");
    CHECK(squared.error().action == broker_exec::errors::SuggestedAction::ReconcileFirst);
    CHECK(owner.server->placed_orders().size() == 3);  // the parent + the two seeds
  }

  SECTION("a parent with NO fill is cancel-only, and that is a COMPLETE square-off") {
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->set_fill_model(0, "open");

    auto placed = owner.adapter.place(make_intent());
    REQUIRE(placed.has_value());

    REQUIRE(owner.adapter.square_off(placed.value().broker_order_id).has_value());
    CHECK(owner.server->placed_orders().size() == 1);
  }

  SECTION("an unknown parent order id refuses rather than guessing") {
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});

    auto squared = owner.adapter.square_off("NOT-A-REAL-ORDER");
    REQUIRE_FALSE(squared.has_value());
    CHECK(squared.error().action == broker_exec::errors::SuggestedAction::ReconcileFirst);
    CHECK(owner.server->placed_orders().empty());
  }

  SECTION("a REPLAYED square_off never opens a second leg") {
    // AC-2 on the Kotak side. A panic path that fires twice, or a crash between
    // the cancel and the place, must converge on ONE exit — a second one is a
    // fresh naked position in the opposite direction.
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});
    owner.server->set_fill_model(50, "complete");

    auto placed = owner.adapter.place(make_intent());
    REQUIRE(placed.has_value());
    const std::string parent_id = placed.value().broker_order_id;

    REQUIRE(owner.adapter.square_off(parent_id).has_value());
    const std::size_t after_first = owner.server->placed_orders().size();
    REQUIRE(after_first == 2);

    REQUIRE(owner.adapter.square_off(parent_id).has_value());
    CHECK(owner.server->placed_orders().size() == after_first);
  }
}

TEST_CASE("[conformance][kotak][IMP-13] a legitimate opposite order is NOT adopted as our exit",
          "[conformance][kotak][IMP-13][squareoff]") {
  // THE FAIL-OPEN THIS CLOSES, and it needs no exotic broker behaviour at all —
  // just two ordinary orders. A SELL 50 parent sitting next to a genuine BUY 50
  // (a hedge leg, a reversal, another strategy under 6-4 isolation) made attribute
  // corroboration "find" an exit that was never ours: square_off placed NOTHING
  // and returned ok while the 50-lot short stayed fully on, and the caller — a
  // panic kill or the protective-stop supervisor — believed it was flat.
  //
  // The fix costs nothing: rung 1 already knows that BUY belongs to a DIFFERENT
  // client_ref, so it was never a corroboration candidate in the first place.
  broker_exec::clock::TestClock clock;
  OwningKotakAdapter owner(clock, FaultConfig{});  // default model: fills everything

  auto parent = owner.adapter.place(make_intent());  // SELL 50
  REQUIRE(parent.has_value());
  auto hedge = owner.adapter.place(hedge_buy());  // BUY 50 — same symbol, same size
  REQUIRE(hedge.has_value());
  REQUIRE(owner.server->book_size() == 2);

  REQUIRE(owner.adapter.square_off(parent.value().broker_order_id).has_value());

  // AN EXIT WAS ACTUALLY PLACED — three orders, not two.
  const auto book = owner.server->placed_orders();
  REQUIRE(book.size() == 3);
  CHECK(book.back().side == "B");
  CHECK(book.back().qty == 50);
  // ...and the hedge leg was left strictly alone.
  CHECK(book[1].order_id == hedge.value().broker_order_id);
  CHECK(book[1].qty == 50);
}

TEST_CASE("[conformance][kotak][IMP-13] an EXISTING exit is judged by its STATE, not its existence",
          "[conformance][kotak][IMP-13][squareoff][duplicate]") {
  // HIGH-2's twin of the Kite guard. Rung 1 — the STRONG id match — applied no
  // status filter at all, directly contradicting the comment three lines above it
  // ("Only WORKING or COMPLETE rows count"). A prior exit the broker had REJECTED
  // was matched by id and answered with a cheerful `ok`: the position fully on,
  // the broker having explicitly refused to close it, and the caller told it was
  // flat. Because a strong-id match PROVES the row is ours (exactly as Kite's tag
  // does), a dead one is escalated to an operator rather than merely refused.
  broker_exec::clock::TestClock clock;
  OwningKotakAdapter owner(clock, FaultConfig{});
  owner.server->set_fill_model(50, "complete");

  auto placed = owner.adapter.place(make_intent());
  REQUIRE(placed.has_value());
  const std::string parent_id = placed.value().broker_order_id;
  REQUIRE(owner.adapter.square_off(parent_id).has_value());
  REQUIRE(owner.server->placed_orders().size() == 2);
  const std::string exit_id = owner.server->placed_orders().back().order_id;

  SECTION("a REJECTED exit is an operator alert, never ok") {
    owner.server->set_order_status(exit_id, "rejected");
    auto again = owner.adapter.square_off(parent_id);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().broker_code == "KOTAK-SQUAREOFF-EXITREJECTED");
    CHECK(again.error().action == broker_exec::errors::SuggestedAction::RaiseAlert);
    CHECK(owner.server->placed_orders().size() == 2);  // and no blind re-fire
  }

  SECTION("a CANCELLED exit is the SAME alert class: the position is still open") {
    owner.server->set_order_status(exit_id, "cancelled");
    auto again = owner.adapter.square_off(parent_id);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().broker_code == "KOTAK-SQUAREOFF-EXITCANCELLED");
    CHECK(again.error().action == broker_exec::errors::SuggestedAction::RaiseAlert);
    CHECK(owner.server->placed_orders().size() == 2);
  }

  SECTION("an UNRECOGNIZED exit status is INDETERMINATE (AC-1e), never ok") {
    owner.server->set_order_status(exit_id, "SOME_FUTURE_KOTAK_STATE");
    auto again = owner.adapter.square_off(parent_id);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().broker_code == "KOTAK-SQUAREOFF-EXITUNKNOWNSTATE");
    CHECK(again.error().action == broker_exec::errors::SuggestedAction::ReconcileFirst);
    CHECK(owner.server->placed_orders().size() == 2);
  }

  SECTION("a still-WORKING exit answers ok, so the guard is not blanket") {
    owner.server->set_order_status(exit_id, "open");
    REQUIRE(owner.adapter.square_off(parent_id).has_value());
    CHECK(owner.server->placed_orders().size() == 2);
  }
}

TEST_CASE("[conformance][kotak][IMP-13] an exit SMALLER than the position never reports ok",
          "[conformance][kotak][IMP-13][squareoff][duplicate]") {
  // The Kotak half of the same race — and it failed WORSE than Kite's. Rung 2
  // demanded an EXACT quantity match, so a legitimately-short prior exit (30) was
  // invisible once the parent had filled to 50: the flatten concluded "no exit
  // exists" and placed a SECOND, FULL-SIZE one. 30 + 50 against a 50-lot long is a
  // 30-lot NAKED REVERSAL — the single worst outcome this call can produce, from a
  // function whose entire purpose is to close a position.
  broker_exec::clock::TestClock clock;
  OwningKotakAdapter owner(clock, FaultConfig{});
  owner.server->set_fill_model(30, "open");  // 30 of 50 done, 20 still working

  auto placed = owner.adapter.place(make_intent());
  REQUIRE(placed.has_value());
  const std::string parent_id = placed.value().broker_order_id;

  REQUIRE(owner.adapter.square_off(parent_id).has_value());
  REQUIRE(owner.server->placed_orders().size() == 2);
  REQUIRE(owner.server->placed_orders().back().qty == 30);

  owner.server->grow_fill(parent_id, 50);  // the remainder filled underneath us

  auto again = owner.adapter.square_off(parent_id);
  REQUIRE_FALSE(again.has_value());
  CHECK(again.error().broker_code == "KOTAK-SQUAREOFF-EXITSHORT");
  CHECK(again.error().action == broker_exec::errors::SuggestedAction::RaiseAlert);
  CHECK(owner.server->placed_orders().size() == 2);  // no second order, and no lie
}

TEST_CASE("[conformance][kotak][IMP-13] square_off REFUSES to flatten its own exit",
          "[conformance][kotak][IMP-13][squareoff]") {
  // A panic walker iterates the book and squares off every row; on its second pass
  // it reaches the exit the first pass placed. Flattening an exit re-opens the
  // position in the ORIGINAL direction with nothing left to close it, so every
  // extra pass reverses the account again.
  broker_exec::clock::TestClock clock;
  OwningKotakAdapter owner(clock, FaultConfig{});
  owner.server->set_fill_model(50, "complete");

  auto placed = owner.adapter.place(make_intent());
  REQUIRE(placed.has_value());
  REQUIRE(owner.adapter.square_off(placed.value().broker_order_id).has_value());
  REQUIRE(owner.server->placed_orders().size() == 2);
  const std::string exit_id = owner.server->placed_orders().back().order_id;

  auto squared = owner.adapter.square_off(exit_id);
  REQUIRE_FALSE(squared.has_value());
  CHECK(squared.error().broker_code == "KOTAK-SQUAREOFF-SELFEXIT");
  CHECK(squared.error().action == broker_exec::errors::SuggestedAction::DoNotRetry);
  CHECK(owner.server->placed_orders().size() == 2);

  // THE HONEST LIMIT, pinned so nobody reads the guard as stronger than it is:
  // it rests on the IN-MEMORY id map, so a RESTARTED process cannot recognize its
  // own exit and the named refusal does NOT fire. (Kite's twin keys on the tag the
  // broker echoes back and does survive a restart — see its suite.) What still
  // holds here is weaker and accidental: attribute corroboration finds the original
  // parent as a plausible opposite order, so nothing is placed. Good enough to
  // avoid a reversal in this shape, but it is corroboration luck rather than a
  // guarantee — the real fix is a verified Kotak tag echo (TagCarry, smoke step 3a)
  // or IntentLog rehydration of the correlation maps.
  broker_exec::adapters::kotak::KotakBrokerAdapter restarted(owner.rest);
  const std::size_t before_replay = owner.server->placed_orders().size();
  auto after_restart = restarted.square_off(exit_id);
  // Whatever it answers, it must not have opened anything.
  CHECK(owner.server->placed_orders().size() == before_replay);
  if (!after_restart.has_value()) {
    // ...and if it refused, it was for some other reason: this guard is blind here.
    CHECK(after_restart.error().broker_code != "KOTAK-SQUAREOFF-SELFEXIT");
  }
}

// ── AC-2: tier-1 green must NOT promote a single capability ──────────────────

TEST_CASE("[conformance][kotak][capabilities] passing the kit does NOT flip any capability",
          "[conformance][kotak][capabilities]") {
  // THE GUARD THIS TEST EXISTS FOR: it is very tempting, once the conformance kit
  // is green, to "finish the job" by marking PlaceOrder Supported. That would be a
  // lie with real money behind it — the kit ran against OUR OWN recorded endpoint,
  // not against Kotak. Only the operator-run live smoke
  // (docs/kotak-min-qty-smoke.md) may promote an entry, so this test fails loudly
  // if anyone promotes one without it.
  namespace caps = broker_exec::capabilities;
  const caps::CapabilitySet set = broker_exec::adapters::kotak::kotak_capabilities();

  for (std::size_t i = 0; i < caps::kCapabilityCount; ++i) {
    const auto capability = static_cast<caps::Capability>(i);
    UNSCOPED_INFO("capability: " << caps::to_string(capability));
    CHECK(set.support_of(capability) == caps::Support::Unknown);
  }

  // And Unknown must be REJECTED at the gate — early, not mid-trade (AC-2). The
  // capability deltas the story calls out are checked by name.
  CHECK_FALSE(set.supports(caps::Capability::PlaceOrder));
  CHECK_FALSE(set.require(caps::Capability::PlaceOrder).has_value());
  CHECK_FALSE(set.require(caps::Capability::BasketMargin).has_value());
  CHECK_FALSE(set.require(caps::Capability::OrderUpdateWebsocket).has_value());
  CHECK_FALSE(set.require(caps::Capability::HeadlessSessionRefresh).has_value());
  CHECK_FALSE(set.require(caps::Capability::TagCarry).has_value());
}

// ── IMP-11 AC-2: the stop TRIGGER reaches the wire, and comes back ───────────
//
// Kotak is NOT symmetric about this field's name: the quick-place REQUEST carries
// the trigger as jData `tp`, while the order REPORT spells the same datum
// `trgPrc`. The recorded server reproduces exactly that asymmetry — it records the
// `tp` it was sent and echoes it back under `trgPrc` — so an adapter that knew
// only one spelling would fail here instead of passing on a fixture that flattered
// it. Before OrderIntent carried a distinct trigger the adapter sent `price` as
// BOTH `pr` and `tp`, so a live SL armed at its own limit.
TEST_CASE("[conformance][kotak][IMP-11] SL sends a DISTINCT trigger and parses it back",
          "[conformance][kotak][IMP-11]") {
  const auto stop_intent = [](broker_exec::domain::OrderType type) {
    broker_exec::domain::OrderIntent intent = make_intent();
    intent.order_type = type;
    intent.price = broker_exec::domain::Price::from_rupees(119);              // the LIMIT
    intent.trigger_price = broker_exec::domain::Price::from_rupees(120, 50);  // the TRIGGER
    return intent;
  };

  SECTION("stop-loss LIMIT (SL): jData carries pr AND tp, and they DIFFER") {
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});

    REQUIRE(owner.adapter.place(stop_intent(broker_exec::domain::OrderType::StopLoss)).has_value());

    // Decode the jData frame and assert the FIELD NAMES, not just that the digits
    // appear somewhere in the body: a substring hit would pass even if the trigger
    // had gone out under the wrong key.
    REQUIRE(owner.server->place_bodies().size() == 1);
    const nlohmann::json sent =
        broker_exec::conformance::kotak_fixture::parse_jdata(owner.server->place_bodies().front());
    CHECK(broker_exec::conformance::kotak_fixture::jstr(sent, "pr") == "119.00");  // the limit
    CHECK(broker_exec::conformance::kotak_fixture::jstr(sent, "tp") == "120.50");  // the trigger
    CHECK(broker_exec::conformance::kotak_fixture::jstr(sent, "pt") == "SL");

    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    const broker_exec::domain::Order& back = orders.value().front();
    REQUIRE(back.intent.client_ref == kClientRef);  // correlated, so the tuple is published
    // The ORDER TYPE round-trips (report spelling `prcTp`) — without it the row
    // would come back as a Market carrying a trigger, a shape the gate refuses.
    CHECK(back.intent.order_type == broker_exec::domain::OrderType::StopLoss);
    REQUIRE(back.intent.trigger_price.has_value());
    // 120.50, NOT the 119.00 limit a duplicated `price` would have produced.
    CHECK(*back.intent.trigger_price == broker_exec::domain::Price::from_paise(12050));
    CHECK(back.intent.price == broker_exec::domain::Price::from_paise(11900));
  }

  SECTION("a plain LIMIT order sends tp=0 and reports NO trigger") {
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});

    REQUIRE(owner.adapter.place(make_intent()).has_value());  // Limit, no trigger

    REQUIRE(owner.server->place_bodies().size() == 1);
    const nlohmann::json sent =
        broker_exec::conformance::kotak_fixture::parse_jdata(owner.server->place_bodies().front());
    CHECK(broker_exec::conformance::kotak_fixture::jstr(sent, "tp") == "0");
    CHECK(broker_exec::conformance::kotak_fixture::jstr(sent, "pt") == "L");

    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    CHECK(orders.value().front().intent.order_type == broker_exec::domain::OrderType::Limit);
    // Kotak's "no trigger" encoding is 0, not an absent key. Reading it as an
    // ENGAGED trigger of zero would make every reconciled limit order look like a
    // stop (and fail the gate's shape check on the next touch).
    CHECK_FALSE(orders.value().front().intent.trigger_price.has_value());
  }

  SECTION("a stop with NO trigger sends tp=0 rather than falling back to the limit") {
    // The adapter's own fail-closed backstop: Kotak rejects a stop with no
    // trigger, which is a definitive verdict. Arming a real stop at the limit
    // price — the pre-IMP-11 behavior — would be silently wrong instead.
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});

    broker_exec::domain::OrderIntent unarmed =
        stop_intent(broker_exec::domain::OrderType::StopLoss);
    unarmed.trigger_price.reset();
    REQUIRE(owner.adapter.place(unarmed).has_value());

    REQUIRE(owner.server->place_bodies().size() == 1);
    const nlohmann::json sent =
        broker_exec::conformance::kotak_fixture::parse_jdata(owner.server->place_bodies().front());
    CHECK(broker_exec::conformance::kotak_fixture::jstr(sent, "tp") == "0");
    CHECK(broker_exec::conformance::kotak_fixture::jstr(sent, "pr") == "119.00");  // NOT copied

    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    CHECK_FALSE(orders.value().front().intent.trigger_price.has_value());
  }

  SECTION("an UNRECOGNIZED prcTp falls closed to Market AND drops the trigger") {
    // The fail-safe pairing, Kotak side. Its field spellings are an unverified
    // tier-2 assumption, so "the report names a price type we do not know" is a
    // first-class hazard here — and the answer must be BOTH halves at once:
    // Market, and no trigger. Publishing a Market that carries a trigger is a
    // shape the validation gate refuses outright.
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});

    REQUIRE(owner.adapter.place(stop_intent(broker_exec::domain::OrderType::StopLoss)).has_value());
    owner.server->set_price_type_override("BO-SL");

    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    const broker_exec::domain::Order& back = orders.value().front();
    REQUIRE(back.intent.client_ref == kClientRef);  // still correlated by broker id
    CHECK(back.intent.order_type == broker_exec::domain::OrderType::Market);
    CHECK_FALSE(back.intent.trigger_price.has_value());
    // And crucially the row is NOT failed to Unknown: an unknown price type is a
    // vocabulary gap, not a corrupt row, so reconciliation still proceeds.
    CHECK(back.state != broker_exec::domain::OrderState::Unknown);
  }

  SECTION("a non-numeric trgPrc fails the row closed, but `tp` is never read") {
    // MEDIUM-6: `tp` is the REQUEST spelling and is deliberately NOT a read
    // candidate. first_paise() flags a key that is PRESENT but unparseable as
    // malformed, which fails the whole row closed to Unknown — so if we read `tp`
    // on a report, a broker sending a non-numeric `tp` would turn EVERY row
    // Unknown and the resulting all-Unknown book would freeze entries via the
    // UNKNOWN-pause. Reading a field we were never promised is not worth a
    // self-inflicted trading halt.
    broker_exec::clock::TestClock clock;
    OwningKotakAdapter owner(clock, FaultConfig{});

    REQUIRE(owner.adapter.place(make_intent()).has_value());
    owner.server->set_report_tp("N/A");  // garbage under the REQUEST spelling

    auto orders = owner.adapter.fetch_orders();
    REQUIRE(orders.has_value());
    REQUIRE(orders.value().size() == 1);
    // Untouched: the row parsed cleanly because `tp` was never consulted.
    CHECK(orders.value().front().state != broker_exec::domain::OrderState::Unknown);
    CHECK(orders.value().front().intent.client_ref == kClientRef);
    CHECK_FALSE(orders.value().front().intent.trigger_price.has_value());
  }
}
