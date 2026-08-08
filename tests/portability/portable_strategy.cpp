#include "portable_strategy.hpp"

#include <string>
#include <vector>

#include "broker_exec/capabilities/capabilities.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/result.hpp"

// NOTE TO A FUTURE EDITOR: this translation unit is scanned by an automated test
// (strategy_scan_test.cpp) that fails if any broker's name appears in it. If you
// need broker-specific behaviour here, you have found a gap in `ports::BrokerPort`
// or in the capability model — widen one of those instead. That is the whole
// bargain: the port absorbs the difference so this file never has to.

namespace portable {

namespace {

using broker_exec::domain::Order;
using broker_exec::domain::OrderIntent;
using broker_exec::domain::OrderState;
using broker_exec::domain::Side;

// Build one leg's intent. Both legs are LIMIT orders on purpose: a market order
// carries no price, and a decision path that never states a price would not
// exercise the money translation each adapter has to get right.
[[nodiscard]] OrderIntent make_intent(const StrategyParams& params, const std::string& client_ref,
                                      const std::string& symbol, Side side,
                                      broker_exec::domain::Price price) {
  OrderIntent intent;
  intent.client_ref = client_ref;
  intent.symbol = symbol;
  intent.side = side;
  intent.quantity = params.quantity;
  intent.price = price;
  intent.order_type = broker_exec::domain::OrderType::Limit;
  intent.product = broker_exec::domain::Product::Intraday;
  intent.strategy = params.strategy_id;
  return intent;
}

// Find our row in broker truth. The broker order id is the strong key; the
// client_ref is the fallback for the case where the acknowledgement was lost and
// the adapter recovered the attribution some other way. Which mechanism the
// adapter used is not this file's business.
[[nodiscard]] const Order* find_row(const std::vector<Order>& orders,
                                    const std::string& broker_order_id,
                                    const std::string& client_ref) {
  if (!broker_order_id.empty()) {
    for (const Order& order : orders) {
      if (order.broker_order_id == broker_order_id) {
        return &order;
      }
    }
  }
  if (!client_ref.empty()) {
    for (const Order& order : orders) {
      if (order.intent.client_ref == client_ref) {
        return &order;
      }
    }
  }
  return nullptr;
}

// Copy what broker truth says about a leg into the outcome.
void absorb_row(LegOutcome& leg, const Order* row, const std::string& client_ref) {
  if (row == nullptr) {
    return;
  }
  leg.present_in_orderbook = true;
  leg.attributed_to_us = (row->intent.client_ref == client_ref);
  leg.state = row->state;
  leg.filled = row->filled_qty;
  leg.avg_price = row->avg_price;
}

// Is the protective leg safe to build a short on top of?
//
// FAIL-CLOSED: only a state that positively says the order is live or done
// qualifies. `Unknown` explicitly does NOT — an unresolved hedge is exactly the
// case where selling the risk leg could leave the account naked, and "we could
// not tell" must never read as "yes".
[[nodiscard]] bool hedge_is_confirmed(OrderState state) noexcept {
  switch (state) {
    case OrderState::Acknowledged:
    case OrderState::Sent:
    case OrderState::PartiallyFilled:
    case OrderState::Filled:
    case OrderState::Reconciled:
      return true;
    case OrderState::Created:
    case OrderState::Validated:
    case OrderState::PendingSend:
    case OrderState::Rejected:
    case OrderState::Cancelled:
    case OrderState::Unknown:
    case OrderState::PartiallyPlaced:
    case OrderState::ManualInterventionRequired:
      return false;
  }
  return false;
}

// Normalize a broker failure into the shared, broker-neutral vocabulary. This is
// the ONLY thing the strategy ever learns about a failure — never raw broker
// text, never a broker error code.
[[nodiscard]] std::string halt_reason_of(const broker_exec::errors::Error& error) {
  return std::string(broker_exec::errors::to_string(error.category));
}

}  // namespace

std::vector<broker_exec::capabilities::Capability> required_capabilities() {
  using Capability = broker_exec::capabilities::Capability;
  // DECLARE THE EXIT, NOT JUST THE ENTRY. Anyone can list the capabilities the
  // happy path calls; the ones that matter are the ones needed to GET OUT of a
  // position this strategy is about to open. A broker that can place but cannot
  // cancel or flatten is a broker on which a short option leg becomes
  // unmanageable, and that has to be a refusal at wiring time — while there is
  // still nothing to manage.
  //
  //   PlaceOrder  — both legs.
  //   CancelOrder — the stand-down at the end of the entry window (exercised).
  //   SquareOff   — the emergency flatten. NOT exercised on the happy path, and
  //                 declared anyway: needing it is exactly the moment you cannot
  //                 afford to discover it is missing.
  return {Capability::PlaceOrder, Capability::CancelOrder, Capability::SquareOff};
}

StrategyOutcome run(broker_exec::ports::BrokerPort& broker, const StrategyParams& params) {
  StrategyOutcome outcome;

  // ── 1. The protective leg goes on FIRST, always ───────────────────────────
  const OrderIntent hedge =
      make_intent(params, params.hedge_ref, params.hedge_symbol, Side::Buy, params.hedge_price);

  auto hedge_ack = broker.place(hedge);
  if (!hedge_ack.has_value()) {
    outcome.halt_reason = halt_reason_of(hedge_ack.error());
    return outcome;  // no protection, no short. Ever.
  }
  outcome.hedge.placed = true;
  outcome.hedge.broker_id_assigned = !hedge_ack.value().broker_order_id.empty();
  const std::string hedge_id = hedge_ack.value().broker_order_id;

  // ── 2. Read broker truth back BEFORE taking on risk ───────────────────────
  // An acknowledgement is a promise, not evidence. The decision to sell the risk
  // leg is made off what the broker says it actually holds.
  auto first_read = broker.fetch_orders();
  if (!first_read.has_value()) {
    outcome.halt_reason = halt_reason_of(first_read.error());
    return outcome;
  }
  const Order* hedge_row = find_row(first_read.value(), hedge_id, params.hedge_ref);
  absorb_row(outcome.hedge, hedge_row, params.hedge_ref);

  if (hedge_row == nullptr || !hedge_is_confirmed(hedge_row->state)) {
    outcome.halt_reason = kHaltHedgeNotConfirmed;
    return outcome;
  }

  // ── 3. Only now does the risk leg go on ───────────────────────────────────
  outcome.short_leg_attempted = true;
  const OrderIntent short_leg =
      make_intent(params, params.short_ref, params.short_symbol, Side::Sell, params.short_price);

  auto short_ack = broker.place(short_leg);
  if (!short_ack.has_value()) {
    outcome.halt_reason = halt_reason_of(short_ack.error());
    return outcome;
  }
  outcome.short_leg.placed = true;
  outcome.short_leg.broker_id_assigned = !short_ack.value().broker_order_id.empty();
  const std::string short_id = short_ack.value().broker_order_id;

  // ── 4. Final reconciliation read: both legs, from broker truth ────────────
  auto final_read = broker.fetch_orders();
  if (!final_read.has_value()) {
    outcome.halt_reason = halt_reason_of(final_read.error());
    return outcome;
  }
  outcome.orderbook_rows = final_read.value().size();
  absorb_row(outcome.hedge, find_row(final_read.value(), hedge_id, params.hedge_ref),
             params.hedge_ref);
  absorb_row(outcome.short_leg, find_row(final_read.value(), short_id, params.short_ref),
             params.short_ref);

  // ── 5. And the net position book, which is what any exit would be sized off ─
  auto positions = broker.fetch_positions();
  if (!positions.has_value()) {
    outcome.halt_reason = halt_reason_of(positions.error());
    return outcome;
  }
  outcome.net_positions = positions.value().size();
  for (const broker_exec::domain::Position& pos : positions.value()) {
    if (pos.symbol == params.short_symbol) {
      outcome.short_position_found = true;
      outcome.short_net_qty = pos.net_qty;
      break;
    }
  }

  // ── 6. STAND DOWN: the exit path, through the same port ───────────────────
  // The entry window is over; withdraw any residual working quantity on the risk
  // leg. Exercising this on the happy path is deliberate — an exit that is only
  // ever called in an emergency is an exit nobody has tested.
  //
  // WHY CANCEL AND NOT A FLATTEN: the two are different capabilities, and only
  // one of them behaves identically everywhere today. A flatten is a broker-side
  // position operation whose support genuinely differs between brokers at this
  // tier (one adapter implements it, another returns a typed refusal rather than
  // pretending), so putting it on the compared path would measure an adapter
  // maturity gap, not portability. Withdrawing an order is the portable exit, so
  // that is the one the shared decision path takes. The flatten capability is
  // still DECLARED (see required_capabilities) — it must exist before the
  // position is opened, whether or not the happy path calls it.
  outcome.stand_down_attempted = true;
  auto stood_down = broker.cancel(short_id);
  if (!stood_down.has_value()) {
    outcome.halt_reason = halt_reason_of(stood_down.error());
    return outcome;
  }
  outcome.stand_down_accepted = true;

  outcome.completed = true;
  return outcome;
}

}  // namespace portable
