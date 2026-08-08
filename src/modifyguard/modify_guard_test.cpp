#include "broker_exec/modifyguard/modify_guard.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"

using broker_exec::domain::OrderState;
using broker_exec::modifyguard::evaluate_modify;
using broker_exec::modifyguard::ModifyRequest;
using broker_exec::modifyguard::ModifyResult;
using broker_exec::modifyguard::ModifyVerdict;
using broker_exec::modifyguard::OrderModifyState;
using broker_exec::modifyguard::to_string;

namespace {

// A clean working order: Acknowledged, total 100, filled 0 — the baseline a safe
// modify is allowed against.
OrderModifyState working_clean() {
  OrderModifyState s;
  s.state = OrderState::Acknowledged;
  s.filled_qty = 0;
  s.total_qty = 100;
  return s;
}

// A price-only modify (the safe modify shape — never re-states quantity).
ModifyRequest price_only() {
  ModifyRequest r;
  r.changes_price = true;
  r.changes_quantity = false;
  return r;
}

// A quantity modify to a given new TOTAL quantity.
ModifyRequest qty_to(std::int64_t new_total) {
  ModifyRequest r;
  r.changes_quantity = true;
  r.new_total_qty = new_total;
  return r;
}

}  // namespace

// ── 1. TERMINAL ─────────────────────────────────────────────────────────────
TEST_CASE("terminal states reject: nothing to modify", "[modifyguard]") {
  for (const OrderState terminal :
       {OrderState::Filled, OrderState::Cancelled, OrderState::Rejected}) {
    OrderModifyState cur = working_clean();
    cur.state = terminal;
    cur.filled_qty = 100;
    // Even an otherwise-valid price-only modify with no race is rejected.
    const ModifyResult res = evaluate_modify(cur, price_only(), 100);
    CHECK(res.verdict == ModifyVerdict::RejectTerminal);
    CHECK_FALSE(res.allowed);
  }
}

// ── 2. NOT-MODIFIABLE base ──────────────────────────────────────────────────
TEST_CASE("ambiguous / awaiting-reconcile states reject", "[modifyguard]") {
  for (const OrderState ambiguous :
       {OrderState::Unknown, OrderState::ManualInterventionRequired, OrderState::Reconciled,
        OrderState::PartiallyPlaced}) {
    OrderModifyState cur = working_clean();
    cur.state = ambiguous;
    const ModifyResult res = evaluate_modify(cur, price_only(), 0);
    CHECK(res.verdict == ModifyVerdict::RejectNotModifiable);
    CHECK_FALSE(res.allowed);
  }
}

// ── 3. RACED FILL ───────────────────────────────────────────────────────────
TEST_CASE("a fill that raced in since the decision rejects, even price-only",
          "[modifyguard]") {
  OrderModifyState cur = working_clean();
  cur.state = OrderState::PartiallyFilled;
  cur.filled_qty = 30;  // current truth
  // Caller decided when it had only observed 20 filled -> a fill raced in.
  const ModifyResult res = evaluate_modify(cur, price_only(), 20);
  CHECK(res.verdict == ModifyVerdict::RejectRacedFill);
  CHECK_FALSE(res.allowed);
}

// ── 4. QTY MODIFY ON A PARTIAL ──────────────────────────────────────────────
TEST_CASE("a quantity modify on a partial rejects (would cancel the remainder)",
          "[modifyguard]") {
  OrderModifyState cur = working_clean();
  cur.state = OrderState::PartiallyFilled;
  cur.filled_qty = 30;
  cur.total_qty = 100;
  // Grow the total well above filled — still rejected purely for being a qty
  // modify on a partial (Kite re-states the TOTAL and can cancel the remainder).
  const ModifyResult res = evaluate_modify(cur, qty_to(200), 30);
  CHECK(res.verdict == ModifyVerdict::RejectQtyOnPartial);
  CHECK_FALSE(res.allowed);
}

TEST_CASE("a price-only modify on a partial is the safe path -> allow", "[modifyguard]") {
  OrderModifyState cur = working_clean();
  cur.state = OrderState::PartiallyFilled;
  cur.filled_qty = 30;
  cur.total_qty = 100;
  const ModifyResult res = evaluate_modify(cur, price_only(), 30);
  CHECK(res.verdict == ModifyVerdict::Allow);
  CHECK(res.allowed);
}

// ── 5. SHRINK BELOW FILLED ──────────────────────────────────────────────────
TEST_CASE("a qty modify whose new total is below filled rejects", "[modifyguard]") {
  OrderModifyState cur = working_clean();
  cur.state = OrderState::Acknowledged;  // non-partial working order
  cur.filled_qty = 20;
  cur.total_qty = 100;
  const ModifyResult res = evaluate_modify(cur, qty_to(15), 20);
  CHECK(res.verdict == ModifyVerdict::RejectShrinkBelowFilled);
  CHECK_FALSE(res.allowed);
}

TEST_CASE("a qty modify whose new total equals filled rejects (<=)", "[modifyguard]") {
  OrderModifyState cur = working_clean();
  cur.state = OrderState::Acknowledged;
  cur.filled_qty = 20;
  cur.total_qty = 100;
  const ModifyResult res = evaluate_modify(cur, qty_to(20), 20);
  CHECK(res.verdict == ModifyVerdict::RejectShrinkBelowFilled);
  CHECK_FALSE(res.allowed);
}

TEST_CASE("a qty modify that grows the total above filled on a non-partial allows",
          "[modifyguard]") {
  OrderModifyState cur = working_clean();
  cur.state = OrderState::Acknowledged;
  cur.filled_qty = 20;
  cur.total_qty = 100;
  const ModifyResult res = evaluate_modify(cur, qty_to(50), 20);
  CHECK(res.verdict == ModifyVerdict::Allow);
  CHECK(res.allowed);
}

// ── 6. ALLOW (price-only on a clean working order) ──────────────────────────
TEST_CASE("a price-only modify on a clean working order allows", "[modifyguard]") {
  const ModifyResult res = evaluate_modify(working_clean(), price_only(), 0);
  CHECK(res.verdict == ModifyVerdict::Allow);
  CHECK(res.allowed);
}

// ── Pre-ack / in-flight states are NOT modifiable (review fix) ───────────────
TEST_CASE("pre-ack/in-flight states reject: a qty modify on an unconfirmed order is unsafe",
          "[modifyguard]") {
  // Created/Validated/PendingSend/Sent are not broker-confirmed: the local
  // filled_qty is stale, so a qty modify could set a TOTAL below the true fill and
  // cancel the working remainder. NEVER modify until Acknowledged/PartiallyFilled.
  for (const OrderState st : {OrderState::Created, OrderState::Validated,
                              OrderState::PendingSend, OrderState::Sent}) {
    OrderModifyState cur = working_clean();
    cur.state = st;
    cur.filled_qty = 0;  // stale: the broker may already have fills on a Sent order
    // Even a "growing" qty modify (would Allow on a confirmed order) must be denied.
    const ModifyResult res = evaluate_modify(cur, qty_to(50), 0);
    CHECK(res.verdict == ModifyVerdict::RejectNotModifiable);
    CHECK_FALSE(res.allowed);
  }
}

// ── Boundaries (review fix) ─────────────────────────────────────────────────
TEST_CASE("boundary: new total exactly one above filled allows; a negative new total rejects",
          "[modifyguard]") {
  OrderModifyState cur = working_clean();
  cur.state = OrderState::Acknowledged;
  cur.filled_qty = 20;
  // 21 == filled+1 (one pending lot remains) -> allowed.
  CHECK(evaluate_modify(cur, qty_to(21), 20).verdict == ModifyVerdict::Allow);
  // A negative new total is <= filled -> shrink-below-filled reject (no UB).
  CHECK(evaluate_modify(cur, qty_to(-5), 20).verdict == ModifyVerdict::RejectShrinkBelowFilled);
}

// ── Fail-closed default ─────────────────────────────────────────────────────
TEST_CASE("a default-constructed ModifyResult is fail-closed", "[modifyguard]") {
  const ModifyResult res;
  CHECK(res.verdict == ModifyVerdict::RejectNotModifiable);
  CHECK_FALSE(res.allowed);
}

// ── Detail is redaction-safe and names verdict + state ──────────────────────
TEST_CASE("detail names the verdict and the order state", "[modifyguard]") {
  OrderModifyState cur = working_clean();
  cur.state = OrderState::PartiallyFilled;
  cur.filled_qty = 30;
  const ModifyResult res = evaluate_modify(cur, qty_to(200), 30);
  CHECK(res.detail.find("reject_qty_on_partial") != std::string::npos);
  CHECK(res.detail.find("PARTIALLY_FILLED") != std::string::npos);
}

// ── to_string stability ─────────────────────────────────────────────────────
TEST_CASE("to_string yields the stable verdict names", "[modifyguard]") {
  CHECK(to_string(ModifyVerdict::Allow) == "allow");
  CHECK(to_string(ModifyVerdict::RejectQtyOnPartial) == "reject_qty_on_partial");
  CHECK(to_string(ModifyVerdict::RejectShrinkBelowFilled) == "reject_shrink_below_filled");
  CHECK(to_string(ModifyVerdict::RejectRacedFill) == "reject_raced_fill");
  CHECK(to_string(ModifyVerdict::RejectTerminal) == "reject_terminal");
  CHECK(to_string(ModifyVerdict::RejectNotModifiable) == "reject_not_modifiable");
}

// ── IMP-11 AC-3: the request is DIFFED off the intents, not asserted by hand ──
//
// `changes_price` used to be a caller-set boolean. A caller that moved a stop's
// TRIGGER while leaving the limit alone could easily set neither flag, and the
// guard would then evaluate a "modify that changes nothing". Now that the trigger
// is a real field, make_modify_request() computes the flags from the two intents.

namespace {

[[nodiscard]] broker_exec::domain::OrderIntent stop_intent() {
  broker_exec::domain::OrderIntent intent;
  intent.client_ref = "alpha-1a2b3c4d-uuid";
  intent.symbol = "NIFTY26JUL24000CE";
  intent.side = broker_exec::domain::Side::Sell;
  intent.quantity = broker_exec::domain::Quantity::of(50);
  intent.price = broker_exec::domain::Price::from_rupees(99);
  intent.trigger_price = broker_exec::domain::Price::from_rupees(100);
  intent.order_type = broker_exec::domain::OrderType::StopLoss;
  intent.strategy = "alpha";
  return intent;
}

}  // namespace

TEST_CASE("make_modify_request: moving ONLY the trigger is a price change",
          "[modifyguard][IMP-11]") {
  const broker_exec::domain::OrderIntent current = stop_intent();
  broker_exec::domain::OrderIntent amended = current;
  amended.trigger_price = broker_exec::domain::Price::from_rupees(101);  // trail the stop

  const ModifyRequest req = broker_exec::modifyguard::make_modify_request(current, amended);
  CHECK(req.changes_price);          // the hazard: this used to be missable
  CHECK_FALSE(req.changes_quantity);  // and it is NOT a quantity modify
  CHECK(req.new_total_qty == 50);

  // A price-only modify stays safe even on a PARTIAL — the whole reason the
  // distinction matters (a qty modify there could cancel the working remainder).
  OrderModifyState partial = working_clean();
  partial.state = OrderState::PartiallyFilled;
  partial.filled_qty = 20;
  CHECK(evaluate_modify(partial, req, 20).allowed);
}

TEST_CASE("make_modify_request: arming and disarming a stop both count as price changes",
          "[modifyguard][IMP-11]") {
  broker_exec::domain::OrderIntent unarmed = stop_intent();
  unarmed.trigger_price.reset();
  const broker_exec::domain::OrderIntent armed = stop_intent();

  // nullopt -> value (arming) and value -> nullopt (disarming) are both changes:
  // std::optional's operator== treats engaged-vs-absent as different.
  CHECK(broker_exec::modifyguard::make_modify_request(unarmed, armed).changes_price);
  CHECK(broker_exec::modifyguard::make_modify_request(armed, unarmed).changes_price);
}

TEST_CASE("make_modify_request: an identical intent changes nothing", "[modifyguard][IMP-11]") {
  const broker_exec::domain::OrderIntent current = stop_intent();
  const ModifyRequest req = broker_exec::modifyguard::make_modify_request(current, current);
  CHECK_FALSE(req.changes_price);
  CHECK_FALSE(req.changes_quantity);
}

TEST_CASE("make_modify_request: a quantity change carries the amended TOTAL",
          "[modifyguard][IMP-11]") {
  const broker_exec::domain::OrderIntent current = stop_intent();
  broker_exec::domain::OrderIntent amended = current;
  amended.quantity = broker_exec::domain::Quantity::of(30);  // Kite: the new TOTAL

  const ModifyRequest req = broker_exec::modifyguard::make_modify_request(current, amended);
  CHECK(req.changes_quantity);
  CHECK_FALSE(req.changes_price);
  CHECK(req.new_total_qty == 30);

  // And the guard still refuses to shrink a total to at-or-below what is filled.
  OrderModifyState cur = working_clean();
  cur.filled_qty = 30;
  CHECK(evaluate_modify(cur, req, 30).verdict == ModifyVerdict::RejectShrinkBelowFilled);
}

TEST_CASE("make_modify_request: changing order_type is a price change too",
          "[modifyguard][IMP-11]") {
  // SL-M -> SL re-points the broker at a limit it previously ignored, so the
  // order works at a different price even though no number moved.
  broker_exec::domain::OrderIntent current = stop_intent();
  current.order_type = broker_exec::domain::OrderType::StopLossMarket;
  broker_exec::domain::OrderIntent amended = current;
  amended.order_type = broker_exec::domain::OrderType::StopLoss;

  CHECK(broker_exec::modifyguard::make_modify_request(current, amended).changes_price);
}
