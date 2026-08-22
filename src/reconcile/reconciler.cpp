#include "broker_exec/reconcile/reconciler.hpp"

#include <string>
#include <utility>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/lifecycle/lifecycle.hpp"

namespace broker_exec::reconcile {

namespace {

// Two orders are the same order if their client_refs match (the primary key) or,
// failing that, their broker_order_ids match (the fallback). Empty fields never
// match — an unacknowledged local order with no broker id only matches by ref.
[[nodiscard]] bool same_order(const domain::Order& local, const domain::Order& broker) noexcept {
  if (!local.intent.client_ref.empty() && local.intent.client_ref == broker.intent.client_ref) {
    return true;
  }
  if (!local.broker_order_id.empty() && local.broker_order_id == broker.broker_order_id) {
    return true;
  }
  return false;
}

// TYPED PROVENANCE for an order's alert (IMP-16). This REPLACED a `ref_of()`
// helper that returned "the client_ref, else the broker_order_id" as a bare
// string to be interpolated into the alert body. That was silently useless: a
// sink scrubs the whole free-form body and a client_ref is one long token-shaped
// run, so the operator got `unmatched broker order ***REDACTED***`. Handing the
// ids over as TYPED COLUMNS lets the sink render them through the whole-column
// allowlist, and it is strictly more informative than the old either/or — both
// ids are carried when both exist, each in its own column, and empty ones are
// omitted by the renderer.
[[nodiscard]] ports::AlertContext provenance_of(const domain::Order& order) {
  ports::AlertContext ctx;
  ctx.client_ref = order.intent.client_ref;
  ctx.broker_order_id = order.broker_order_id;
  ctx.strategy = order.intent.strategy;
  // ...and the INSTRUMENT, which has the same defect the ids had: an option symbol
  // of >=20 chars (BANKNIFTY24JUN52000CE) is a token-shaped run to scrub(), so it
  // could never have survived the body either. It is measured against the SYMBOL
  // shape rule (uppercase alnum), not the id one.
  ctx.symbol = order.intent.symbol;
  return ctx;
}

}  // namespace

Result<ReconcileResult> Reconciler::fetch(ports::BrokerPort& broker,
                                          std::int64_t snapshot_seq) const {
  // READS ONLY: the four idempotent broker queries. Any failure short-circuits
  // to that typed Error. No writes, no FSM, no Store — this side cannot mutate.
  auto orders = broker.fetch_orders();
  if (!orders) {
    return fail(orders.error());
  }
  auto trades = broker.fetch_trades();
  if (!trades) {
    return fail(trades.error());
  }
  auto positions = broker.fetch_positions();
  if (!positions) {
    return fail(positions.error());
  }
  auto funds = broker.fetch_funds();
  if (!funds) {
    return fail(funds.error());
  }

  ReconcileResult result;
  result.orders = std::move(orders.value());
  result.trades = std::move(trades.value());
  result.positions = std::move(positions.value());
  result.funds = funds.value();
  result.ordering_key = snapshot_seq;
  result.fetched_at = clock_.now_wall();
  return result;
}

ReconcileOutcome ReconcileApplier::apply(const ReconcileResult& result,
                                         std::vector<domain::Order>& local_orders) {
  ReconcileOutcome outcome;

  // ── Stale / out-of-order snapshot guard (apply-ordering) ──
  // A snapshot whose ordering_key is not newer than one we have already applied
  // is stale/duplicate. The per-order FSM stale-guards its WRITES, so we still
  // feed each matched view through it below (harmless: the engine drops stale
  // per-order keys), but we must NOT escalate mismatches off a stale snapshot —
  // doing so would false-set block_new_orders during routine out-of-order
  // delivery. We therefore skip the phantom + vanished alert/block logic and
  // leave block_new_orders=false for a stale snapshot (still counting
  // applied/dropped). A fresh snapshot advances the high-water mark.
  const bool stale_snapshot = result.ordering_key <= last_snapshot_key_;
  if (!stale_snapshot) {
    last_snapshot_key_ = result.ordering_key;
  }

  // ── Broker orders -> local state (via the FSM), phantom detection ──
  for (const domain::Order& broker_order : result.orders) {
    domain::Order* match = nullptr;
    for (domain::Order& local : local_orders) {
      if (same_order(local, broker_order)) {
        match = &local;
        break;
      }
    }

    if (match == nullptr) {
      // A broker order the bot did not create (phantom / manually placed). A
      // stale snapshot never escalates (see the guard above).
      if (!stale_snapshot) {
        ++outcome.mismatches;
        outcome.block_new_orders = true;
        (void)alerts_.send_with_context(ports::AlertLevel::Warning,
                                        "reconcile: unmatched broker order",
                                        provenance_of(broker_order));
      }
      continue;
    }

    // Build the broker view and let the lifecycle FSM decide (forward
    // progressing + terminal-absorbing). Key the view on a STABLE, non-empty
    // identity: the LOCAL client_ref when present, else the broker_order_id, so
    // the engine's per-order high-water mark stays stable AND distinct even when
    // the match came via the broker_order_id fallback with an empty client_ref
    // (an empty key would collide all such orders into one bucket).
    lifecycle::BrokerView view;
    view.client_ref =
        !match->intent.client_ref.empty() ? match->intent.client_ref : match->broker_order_id;
    view.broker_order_id = broker_order.broker_order_id;
    view.observed_state = broker_order.state;
    view.filled_qty = broker_order.filled_qty;
    view.avg_price = broker_order.avg_price;
    view.ordering_key = result.ordering_key;

    const lifecycle::ApplyOutcome applied = engine_.apply(*match, view);
    ++outcome.applied;
    switch (applied) {
      case lifecycle::ApplyOutcome::Applied:
        ++outcome.advanced;
        break;
      case lifecycle::ApplyOutcome::DroppedStale:
        ++outcome.dropped_stale;
        break;
      case lifecycle::ApplyOutcome::DroppedTerminal:
      case lifecycle::ApplyOutcome::NoChange:
        break;
    }
  }

  // ── Vanished local orders: a non-terminal order the bot believes live that
  // the broker snapshot does NOT contain (a candidate manual intervention; full
  // handling is Story 3.2, flagged here). A stale snapshot never escalates. ──
  if (!stale_snapshot) {
    for (const domain::Order& local : local_orders) {
      // Only a non-terminal order the broker has ALREADY acknowledged (non-empty
      // broker_order_id — we previously got an ack, so the broker definitely
      // knows it) can "vanish". A terminal order legitimately drops off the
      // snapshot; a non-terminal order with an EMPTY broker_order_id is pre-ack
      // (a normal place->reconcile race), NOT a mismatch.
      if (lifecycle::is_terminal(local.state) || local.broker_order_id.empty()) {
        continue;
      }
      bool present = false;
      for (const domain::Order& broker_order : result.orders) {
        if (same_order(local, broker_order)) {
          present = true;
          break;
        }
      }
      if (!present) {
        ++outcome.mismatches;
        outcome.block_new_orders = true;
        (void)alerts_.send_with_context(ports::AlertLevel::Warning,
                                        "reconcile: local order missing from broker snapshot",
                                        provenance_of(local));
      }
    }
  }

  return outcome;
}

std::chrono::milliseconds next_cadence(const ReconcileState& state, std::chrono::milliseconds tight,
                                       std::chrono::milliseconds loose) {
  return (state.any_inflight || state.any_open_position) ? tight : loose;
}

ReconcileState derive_state(const std::vector<domain::Order>& orders,
                            const std::vector<domain::Position>& positions) {
  ReconcileState state;

  for (const domain::Order& order : orders) {
    switch (order.state) {
      // Working / uncertain at the broker -> poll tight (inclusive of the
      // SENT/UNKNOWN states the AC calls out).
      case domain::OrderState::PendingSend:
      case domain::OrderState::Sent:
      case domain::OrderState::Acknowledged:
      case domain::OrderState::PartiallyFilled:
      case domain::OrderState::Unknown:
      case domain::OrderState::PartiallyPlaced:
        state.any_inflight = true;
        break;
      // Pre-send (Created/Validated), settled (Reconciled), stuck-for-human
      // (ManualInterventionRequired) and terminal states are not in-flight.
      case domain::OrderState::Created:
      case domain::OrderState::Validated:
      case domain::OrderState::Reconciled:
      case domain::OrderState::ManualInterventionRequired:
      case domain::OrderState::Filled:
      case domain::OrderState::Rejected:
      case domain::OrderState::Cancelled:
        break;
    }
    if (state.any_inflight) {
      break;
    }
  }

  for (const domain::Position& position : positions) {
    if (position.net_qty != domain::Quantity::of(0)) {
      state.any_open_position = true;
      break;
    }
  }

  return state;
}

}  // namespace broker_exec::reconcile
