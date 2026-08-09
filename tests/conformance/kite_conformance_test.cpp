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
#include "broker_exec/adapters/square_off_exit.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/idempotency/idempotency.hpp"
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

// ── IMP-13: the REAL flatten (AC-1), its duplicate guard (AC-2) and the
// instrument-master exchange resolver (AC-4) ─────────────────────────────────
//
// square_off is the EMERGENCY exit: hedge-first recovery (5.1), a panic kill
// (3.8) and the protective-stop supervisor (IMP-4) all reach for it, on exactly
// the paths where it gets invoked twice. Before IMP-13 the Kite impl CANCELLED
// and returned ok, so a FILLED position survived a "successful" square-off. These
// tests pin the flatten and, more importantly, pin that a second call cannot open
// a second opposite leg — which would be a brand-new naked position.

namespace {

// A parent SELL the recorded server reports as filled per its fill model.
broker_exec::domain::OrderIntent parent_sell() {
  broker_exec::domain::OrderIntent intent;
  intent.client_ref = "alpha-1a2b3c4d-00112233445566778899aabbccddeeff";
  intent.symbol = "NIFTY24JUN24000CE";
  intent.side = broker_exec::domain::Side::Sell;
  intent.quantity = broker_exec::domain::Quantity::of(50);
  intent.price = broker_exec::domain::Price::from_rupees(120);
  intent.order_type = broker_exec::domain::OrderType::Limit;
  intent.product = broker_exec::domain::Product::Intraday;
  intent.strategy = "alpha";
  return intent;
}

// The broker ids of a parent that has been flattened EXACTLY ONCE — the shared
// prologue for every "what does a SECOND square_off do?" test below.
struct FlattenedParent {
  std::string parent_id;
  std::string exit_id;
};

[[nodiscard]] FlattenedParent flatten_once(OwningKiteAdapter& owner) {
  auto placed = owner.adapter.place(parent_sell());
  REQUIRE(placed.has_value());
  REQUIRE(owner.adapter.square_off(placed.value().broker_order_id).has_value());
  const auto book = owner.server->placed_orders();
  REQUIRE(book.size() == 2);
  return FlattenedParent{placed.value().broker_order_id, book.back().order_id};
}

}  // namespace

TEST_CASE("[conformance][kite][IMP-13] square_off FLATTENS the filled quantity",
          "[conformance][kite][IMP-13][squareoff]") {
  SECTION("a partially filled parent is cancelled AND exited for exactly the fill") {
    broker_exec::clock::TestClock clock;
    OwningKiteAdapter owner(clock, FaultConfig{});
    owner.server->set_fill_model(30, "OPEN");  // 30 of 50 done, 20 still working

    auto placed = owner.adapter.place(parent_sell());
    REQUIRE(placed.has_value());
    const std::string parent_id = placed.value().broker_order_id;

    REQUIRE(owner.adapter.square_off(parent_id).has_value());

    const auto book = owner.server->placed_orders();
    // Exactly ONE new order beyond the parent: the exit.
    REQUIRE(book.size() == 2);
    const auto& exit_row = book.back();
    // Opposite side, sized off the CANONICAL fill (30) — not the order total (50),
    // which would leave a naked 20 long once the working remainder is cancelled.
    CHECK(exit_row.side == "BUY");
    CHECK(exit_row.qty == "30");
    CHECK(exit_row.symbol == "NIFTY24JUN24000CE");
    CHECK(exit_row.order_type == "MARKET");
  }

  SECTION("a parent with NO fill is cancel-only, and that is a COMPLETE square-off") {
    broker_exec::clock::TestClock clock;
    OwningKiteAdapter owner(clock, FaultConfig{});
    owner.server->set_fill_model(0, "OPEN");

    auto placed = owner.adapter.place(parent_sell());
    REQUIRE(placed.has_value());

    REQUIRE(owner.adapter.square_off(placed.value().broker_order_id).has_value());

    // Nothing was placed to close a position that was never opened.
    CHECK(owner.server->placed_orders().size() == 1);
  }

  SECTION("a cancel the broker REFUSES as already-terminal is tolerated (AC-1b)") {
    // THE BUG THIS SECTION USED TO HIDE: it drove a 50-of-50 COMPLETE parent, which
    // the adapter reads as terminal — so it SKIPPED the cancel entirely and the
    // refusal knob never fired. The section passed while proving nothing, and
    // AC-1b's tolerance had zero coverage on either broker.
    //
    // Driving it from a PARTIAL fill is what makes the cancel real: the broker
    // says COMPLETE while reporting 30 of 50, the adapter's quantity-first reading
    // calls that PartiallyFilled (a live remainder), so it issues the cancel — and
    // broker truth, which still holds a terminal row, refuses it. That refusal
    // means the remainder reached the state we were cancelling it into, so the
    // flatten must shrug it off rather than abandon the square-off with a filled
    // position still open.
    broker_exec::clock::TestClock clock;
    OwningKiteAdapter owner(clock, FaultConfig{});
    owner.server->set_fill_model(30, "COMPLETE");
    owner.server->set_cancel_rejects_terminal(true);

    auto placed = owner.adapter.place(parent_sell());
    REQUIRE(placed.has_value());

    REQUIRE(owner.adapter.square_off(placed.value().broker_order_id).has_value());

    // NON-VACUITY: a cancel really was issued and really was refused. Without this
    // the section silently reverts to "cancel skipped" the next time the fill model
    // changes.
    CHECK(owner.server->cancel_refusals() == 1);

    const auto book = owner.server->placed_orders();
    REQUIRE(book.size() == 2);
    CHECK(book.back().side == "BUY");
    CHECK(book.back().qty == "30");  // the fill, not the order total
  }

  SECTION("an UNKNOWN parent status is INDETERMINATE: no exit is ever placed") {
    broker_exec::clock::TestClock clock;
    OwningKiteAdapter owner(clock, FaultConfig{});

    auto placed = owner.adapter.place(parent_sell());
    REQUIRE(placed.has_value());
    // A status word we do not recognize: we do not know whether this order is
    // working, filled or dead, so we neither cancel it nor size an exit off it.
    owner.server->set_status_override("SOME NEW KITE STATE");

    auto squared = owner.adapter.square_off(placed.value().broker_order_id);
    REQUIRE_FALSE(squared.has_value());
    CHECK(squared.error().action == broker_exec::errors::SuggestedAction::ReconcileFirst);
    // The safety property: an indeterminate read placed NOTHING.
    CHECK(owner.server->placed_orders().size() == 1);
  }

  SECTION("an unknown parent order id refuses rather than guessing") {
    broker_exec::clock::TestClock clock;
    OwningKiteAdapter owner(clock, FaultConfig{});

    auto squared = owner.adapter.square_off("NOT-A-REAL-ORDER");
    REQUIRE_FALSE(squared.has_value());
    CHECK(squared.error().action == broker_exec::errors::SuggestedAction::ReconcileFirst);
    CHECK(owner.server->placed_orders().empty());
  }
}

TEST_CASE("[conformance][kite][IMP-13] a REPLAYED square_off never opens a second leg",
          "[conformance][kite][IMP-13][squareoff][duplicate]") {
  // AC-2, and the whole reason the exit ref is deterministic. A panic path that
  // fires twice, or a crash between the cancel and the place, must converge on ONE
  // exit — a second one is a fresh naked position in the opposite direction.
  broker_exec::clock::TestClock clock;
  OwningKiteAdapter owner(clock, FaultConfig{});
  owner.server->set_fill_model(50, "COMPLETE");

  auto placed = owner.adapter.place(parent_sell());
  REQUIRE(placed.has_value());
  const std::string parent_id = placed.value().broker_order_id;

  REQUIRE(owner.adapter.square_off(parent_id).has_value());
  const std::size_t after_first = owner.server->placed_orders().size();
  REQUIRE(after_first == 2);

  // Invoke it again — the operator hammering panic, or a supervisor restart.
  REQUIRE(owner.adapter.square_off(parent_id).has_value());
  CHECK(owner.server->placed_orders().size() == after_first);

  // And a THIRD time through a freshly constructed adapter over the SAME server:
  // this is the post-crash replay, where the adapter's in-memory correlation maps
  // are empty and only broker truth remains. The fetch-first guard is what has to
  // hold here, not the local maps.
  broker_exec::adapters::kite::KiteBrokerAdapter restarted(owner.rest);
  REQUIRE(restarted.square_off(parent_id).has_value());
  CHECK(owner.server->placed_orders().size() == after_first);

  // Exactly one exit on the book, for the full fill.
  const auto book = owner.server->placed_orders();
  REQUIRE(book.size() == 2);
  CHECK(book.back().side == "BUY");
  CHECK(book.back().qty == "50");
}

TEST_CASE("[conformance][kite][IMP-13] an EXISTING exit is judged by its STATE, not its existence",
          "[conformance][kite][IMP-13][squareoff][duplicate]") {
  // THE FAIL-OPEN THIS CLOSES: the duplicate guard used to special-case only
  // REJECTED and let EVERY other status fall into `return ports::ok()`. So an exit
  // the broker had CANCELLED — or one reporting a word from a vocabulary we do not
  // know — was read as "already flat". A square_off that LIES about being flat is
  // worse than one that fails: the caller believes the position is closed and
  // stops managing it, so the leg runs unattended until an operator finds it.
  broker_exec::clock::TestClock clock;
  OwningKiteAdapter owner(clock, FaultConfig{});
  owner.server->set_fill_model(50, "COMPLETE");
  const FlattenedParent ids = flatten_once(owner);

  SECTION("a WORKING exit answers ok, and still never a second order") {
    owner.server->set_order_status(ids.exit_id, "OPEN");
    REQUIRE(owner.adapter.square_off(ids.parent_id).has_value());
    CHECK(owner.server->placed_orders().size() == 2);
  }

  SECTION("a FILLED exit answers ok — the position really is closed") {
    owner.server->set_order_status(ids.exit_id, "COMPLETE");
    REQUIRE(owner.adapter.square_off(ids.parent_id).has_value());
    CHECK(owner.server->placed_orders().size() == 2);
  }

  SECTION("a REJECTED exit is an operator alert, never ok") {
    owner.server->set_order_status(ids.exit_id, "REJECTED");
    auto squared = owner.adapter.square_off(ids.parent_id);
    REQUIRE_FALSE(squared.has_value());
    CHECK(squared.error().broker_code == "KITE-SQUAREOFF-EXITREJECTED");
    CHECK(squared.error().action == broker_exec::errors::SuggestedAction::RaiseAlert);
    CHECK(owner.server->placed_orders().size() == 2);  // and no blind re-fire
  }

  SECTION("a CANCELLED exit is the SAME alert class: the position is still open") {
    owner.server->set_order_status(ids.exit_id, "CANCELLED");
    auto squared = owner.adapter.square_off(ids.parent_id);
    REQUIRE_FALSE(squared.has_value());
    CHECK(squared.error().broker_code == "KITE-SQUAREOFF-EXITCANCELLED");
    CHECK(squared.error().action == broker_exec::errors::SuggestedAction::RaiseAlert);
    CHECK(owner.server->placed_orders().size() == 2);
  }

  SECTION("an UNRECOGNIZED exit status is INDETERMINATE (AC-1e), never ok") {
    owner.server->set_order_status(ids.exit_id, "SOME NEW KITE STATE");
    auto squared = owner.adapter.square_off(ids.parent_id);
    REQUIRE_FALSE(squared.has_value());
    CHECK(squared.error().action == broker_exec::errors::SuggestedAction::ReconcileFirst);
    CHECK(owner.server->placed_orders().size() == 2);
  }
}

TEST_CASE("[conformance][kite][IMP-13] an exit SMALLER than the position never reports ok",
          "[conformance][kite][IMP-13][squareoff][duplicate]") {
  // THE RACE, in full: the parent is 30-of-50 filled when square_off measures it,
  // so the exit goes out for 30 — and the last 20 fill in the window before the
  // cancel lands. A replay now measures a 50-lot position, finds a 30-lot exit,
  // and (checking only EXISTENCE) used to answer ok with 20 lots naked and the
  // caller told it was flat.
  broker_exec::clock::TestClock clock;
  OwningKiteAdapter owner(clock, FaultConfig{});
  owner.server->set_fill_model(30, "OPEN");

  auto placed = owner.adapter.place(parent_sell());
  REQUIRE(placed.has_value());
  const std::string parent_id = placed.value().broker_order_id;

  REQUIRE(owner.adapter.square_off(parent_id).has_value());
  REQUIRE(owner.server->placed_orders().size() == 2);
  REQUIRE(owner.server->placed_orders().back().qty == "30");

  owner.server->grow_fill(parent_id, 50);  // the remainder filled underneath us

  auto again = owner.adapter.square_off(parent_id);
  REQUIRE_FALSE(again.has_value());
  CHECK(again.error().broker_code == "KITE-SQUAREOFF-EXITSHORT");
  CHECK(again.error().action == broker_exec::errors::SuggestedAction::RaiseAlert);
  // NEITHER failure mode: not a cheerful ok over a 20-lot naked leg, and not a
  // second FULL-SIZE exit (30 + 50 against 50 long is a 30-lot naked reversal).
  CHECK(owner.server->placed_orders().size() == 2);
}

TEST_CASE("[conformance][kite][IMP-13] square_off REFUSES to flatten its own exit",
          "[conformance][kite][IMP-13][squareoff]") {
  // A panic walker iterates the book and squares off every row. On its second
  // pass it reaches the exit the first pass placed — and flattening an exit
  // re-opens the position in the ORIGINAL direction, with nothing left to close
  // it. Every extra pass reverses the account again.
  broker_exec::clock::TestClock clock;
  OwningKiteAdapter owner(clock, FaultConfig{});
  owner.server->set_fill_model(50, "COMPLETE");
  const FlattenedParent ids = flatten_once(owner);

  auto squared = owner.adapter.square_off(ids.exit_id);
  REQUIRE_FALSE(squared.has_value());
  CHECK(squared.error().broker_code == "KITE-SQUAREOFF-SELFEXIT");
  CHECK(squared.error().action == broker_exec::errors::SuggestedAction::DoNotRetry);
  CHECK(owner.server->placed_orders().size() == 2);

  // AND IT SURVIVES A RESTART, because it reads the TAG the broker echoes back
  // rather than an in-memory map. (The Kotak twin cannot make this claim — it has
  // no verified tag echo — and its header says so.)
  broker_exec::adapters::kite::KiteBrokerAdapter restarted(owner.rest);
  CHECK_FALSE(restarted.square_off(ids.exit_id).has_value());
  CHECK(owner.server->placed_orders().size() == 2);
}

TEST_CASE("[conformance][kite][IMP-13] the exit echoes the parent's PRODUCT and EXCHANGE",
          "[conformance][kite][IMP-13][squareoff]") {
  // Exiting an NRML position with an MIS order does not close it: it opens a
  // SECOND position in a different margin bucket while the overnight carry leg
  // survives the "square-off" untouched. The product is therefore echoed from
  // broker truth rather than re-derived from the exit intent's own enum.
  broker_exec::clock::TestClock clock;
  OwningKiteAdapter owner(clock, FaultConfig{});
  owner.server->set_fill_model(50, "COMPLETE");

  broker_exec::domain::OrderIntent carry = parent_sell();
  carry.product = broker_exec::domain::Product::Normal;  // NRML, not the MIS default
  auto placed = owner.adapter.place(carry);
  REQUIRE(placed.has_value());
  REQUIRE(owner.adapter.square_off(placed.value().broker_order_id).has_value());

  const auto book = owner.server->placed_orders();
  REQUIRE(book.size() == 2);
  REQUIRE(book.front().product == "NRML");
  CHECK(book.back().product == "NRML");
  CHECK(book.back().exchange == book.front().exchange);
  CHECK(book.back().side == "BUY");
  CHECK(book.back().qty == "50");
}

TEST_CASE("[conformance][kite][IMP-13] the exit ref can never collide with a slice child",
          "[conformance][kite][IMP-13][squareoff]") {
  // The freeze-slicer owns `<parent>#<k>` with k >= 1 — every child suffix is a run
  // of DECIMAL DIGITS. The exit suffix is `#X`, so a slice and a flatten of the
  // same parent are always distinct orders. If the slicer ever admits a
  // non-numeric suffix, this fails BEFORE two orders can collide live.
  const std::string parent = "alpha-1a2b3c4d-00112233445566778899aabbccddeeff";
  const std::string exit_ref = broker_exec::adapters::exit_client_ref(parent);

  CHECK(broker_exec::adapters::is_exit_ref(exit_ref));
  for (int k = 1; k <= 64; ++k) {
    CHECK(exit_ref != broker_exec::idempotency::child_ref(parent, k));
  }
  // An empty anchor yields no name at all — a caller must never build an exit out
  // of nothing, and the adapters fail closed on the empty string.
  CHECK(broker_exec::adapters::exit_client_ref("").empty());
  CHECK_FALSE(broker_exec::adapters::is_exit_ref(parent));
}

TEST_CASE("[conformance][kite][IMP-13] the instrument master OUTRANKS the symbol heuristic",
          "[conformance][kite][IMP-13][exchange]") {
  // AC-4. The symbol-shape guess is a fallback for an unwired adapter; when a
  // resolver IS wired it is authoritative, because the instrument master actually
  // knows where the contract trades and a guess does not.
  SECTION("a wired resolver supplies the exchange the heuristic would have missed") {
    broker_exec::clock::TestClock clock;
    OwningKiteAdapter owner(clock, FaultConfig{});
    owner.adapter.set_exchange_resolver(
        [](const std::string&) -> broker_exec::Result<std::string> { return std::string{"BFO"}; });
    REQUIRE(owner.adapter.has_exchange_resolver());

    REQUIRE(owner.adapter.place(parent_sell()).has_value());

    const auto book = owner.server->placed_orders();
    REQUIRE(book.size() == 1);
    // NFO is what the shape heuristic yields for this symbol; the master said BFO.
    CHECK(book.front().exchange == "BFO");
  }

  SECTION("a resolver ERROR is fail-closed: no order goes out under a guessed exchange") {
    broker_exec::clock::TestClock clock;
    OwningKiteAdapter owner(clock, FaultConfig{});
    owner.adapter.set_exchange_resolver([](const std::string&) {
      return broker_exec::fail(broker_exec::errors::make_error(
          broker_exec::errors::ErrorCategory::DataStale, "instrument master is stale", "TEST"));
    });

    auto placed = owner.adapter.place(parent_sell());
    REQUIRE_FALSE(placed.has_value());
    // Silently falling back to the heuristic here is the fail-open this guards:
    // it would route a real order to an exchange nobody confirmed.
    CHECK(owner.server->placed_orders().empty());
  }

  SECTION("with NO resolver wired the documented heuristic still applies") {
    broker_exec::clock::TestClock clock;
    OwningKiteAdapter owner(clock, FaultConfig{});
    CHECK_FALSE(owner.adapter.has_exchange_resolver());

    REQUIRE(owner.adapter.place(parent_sell()).has_value());
    const auto book = owner.server->placed_orders();
    REQUIRE(book.size() == 1);
    // PIN THE VALUE, not merely its non-emptiness. "Some exchange was sent" passes
    // for an adapter that sends a constant, or the wrong constant; the heuristic's
    // documented answer for an option symbol is NFO and nothing else. It is also
    // what makes the resolver sections above meaningful — BFO is a value this
    // fallback provably cannot produce.
    CHECK(book.front().exchange == "NFO");
  }

  SECTION("a wired resolver drives the EXIT's exchange too (AC-4 covers the flatten)") {
    // AC-4 names the square-off exit explicitly, and it had no coverage: every
    // resolver assertion ran on the PLACE path. An exit routed by the heuristic
    // while entries are routed by the master is the mis-route that matters most —
    // it is the leg that is supposed to CLOSE something.
    broker_exec::clock::TestClock clock;
    OwningKiteAdapter owner(clock, FaultConfig{});
    owner.adapter.set_exchange_resolver(
        [](const std::string&) -> broker_exec::Result<std::string> { return std::string{"BFO"}; });
    owner.server->set_fill_model(50, "COMPLETE");

    auto placed = owner.adapter.place(parent_sell());
    REQUIRE(placed.has_value());
    REQUIRE(owner.adapter.square_off(placed.value().broker_order_id).has_value());

    const auto book = owner.server->placed_orders();
    REQUIRE(book.size() == 2);
    // BFO on BOTH legs. The heuristic yields NFO for this symbol, so an exit that
    // had quietly fallen back to it would read "NFO" here.
    CHECK(book.front().exchange == "BFO");
    CHECK(book.back().exchange == "BFO");
  }

  SECTION("on the SQUARE-OFF path a resolver ERROR refuses BEFORE anything is cancelled") {
    // THE ORDERING BUG THIS PINS: the resolver used to run immediately before the
    // place, i.e. AFTER the cancel. A resolver error therefore left the working
    // remainder CANCELLED and the already-filled quantity NAKED, and then returned
    // an error — a square_off that made the position strictly harder to manage
    // than never calling it. Refusals must be decided from the READ, before any
    // side effect. (place() has always resolved first; the flatten now matches.)
    broker_exec::clock::TestClock clock;
    OwningKiteAdapter owner(clock, FaultConfig{});
    owner.server->set_fill_model(30, "OPEN");  // 30 done, 20 STILL WORKING — the thing to lose

    auto placed = owner.adapter.place(parent_sell());  // no resolver yet: heuristic -> NFO
    REQUIRE(placed.has_value());

    owner.adapter.set_exchange_resolver([](const std::string&) {
      return broker_exec::fail(broker_exec::errors::make_error(
          broker_exec::errors::ErrorCategory::DataStale, "instrument master is stale", "TEST"));
    });

    auto squared = owner.adapter.square_off(placed.value().broker_order_id);
    REQUIRE_FALSE(squared.has_value());

    const auto book = owner.server->placed_orders();
    REQUIRE(book.size() == 1);             // no exit, as before
    CHECK(book.front().status == "OPEN");  // AND the remainder was never cancelled
  }

  SECTION("a resolver that DISAGREES with the parent row refuses, and cancels nothing") {
    // A FLATTEN MUST ROUTE WHERE THE POSITION ACTUALLY IS. The parent filled on
    // NFO; the master now says BFO. Either the master is stale or the parent went
    // somewhere unexpected — and on both readings sending the "exit" to BFO closes
    // nothing and OPENS a fresh naked leg on a second exchange. Preferring the
    // parent row would be safer than that, but it would also silently ignore the
    // authority we were told to trust on the call where being wrong costs most. So
    // neither side wins: refuse, touch nothing, and hand an operator both names.
    broker_exec::clock::TestClock clock;
    OwningKiteAdapter owner(clock, FaultConfig{});
    owner.server->set_fill_model(30, "OPEN");

    auto placed = owner.adapter.place(parent_sell());  // routed NFO by the heuristic
    REQUIRE(placed.has_value());
    REQUIRE(owner.server->placed_orders().front().exchange == "NFO");

    owner.adapter.set_exchange_resolver(
        [](const std::string&) -> broker_exec::Result<std::string> { return std::string{"BFO"}; });

    auto squared = owner.adapter.square_off(placed.value().broker_order_id);
    REQUIRE_FALSE(squared.has_value());
    CHECK(squared.error().broker_code == "KITE-SQUAREOFF-EXCHANGEMISMATCH");
    CHECK(squared.error().action == broker_exec::errors::SuggestedAction::RaiseAlert);

    const auto book = owner.server->placed_orders();
    REQUIRE(book.size() == 1);
    CHECK(book.front().status == "OPEN");  // nothing cancelled, nothing sent
  }
}
