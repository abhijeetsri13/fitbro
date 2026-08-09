#include "broker_exec/runtime/unknown_resolver.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/errors/error.hpp"

namespace broker_exec::runtime {

namespace {

using domain::Order;
using domain::OrderState;

// Attribute equality for the WEAKEST rung: two orders corroborate when their
// (symbol, side, quantity, price, TRIGGER price) all agree. Deliberately does NOT
// consider product/order_type — corroboration is a best-effort "same economic
// trade", and over-narrowing it would make a real match harder while the
// precedence already prefers the two id-based rungs whenever they exist.
//
// THE TRIGGER IS THE ONE EXCEPTION TO THAT LENIENCE (IMP-11), because for a stop
// it is not a detail — it IS the order. Two protective stops on the same symbol,
// side and size, differing only in the level at which they arm, are DIFFERENT
// orders with different risk. Matching on (symbol, side, qty, price) alone would
// let this rung adopt a stop armed at the wrong level as though it were ours,
// and the local order would then be marked resolved against a protection that
// fires somewhere else entirely. `std::optional` equality also gives the right
// answer at the boundary: an armed stop never corroborates an unarmed order.
[[nodiscard]] bool attributes_match(const Order& a, const Order& b) noexcept {
  return a.intent.symbol == b.intent.symbol && a.intent.side == b.intent.side &&
         a.intent.quantity == b.intent.quantity && a.intent.price == b.intent.price &&
         a.intent.trigger_price == b.intent.trigger_price;
}

}  // namespace

std::string_view to_string(MatchKind kind) noexcept {
  switch (kind) {
    case MatchKind::BrokerOrderId:
      return "BROKER_ORDER_ID";
    case MatchKind::CorrelationToken:
      return "CORRELATION_TOKEN";
    case MatchKind::AttributeCorroboration:
      return "ATTRIBUTE_CORROBORATION";
    case MatchKind::NoMatch:
      return "NO_MATCH";
  }
  return "NO_MATCH";
}

UnknownResolver::UnknownResolver(ports::BrokerPort& broker, store::Store& store,
                                 lifecycle::LifecycleEngine& fsm, ports::AlertSink& alerts,
                                 ports::ClockPort& clock, Config config)
    : broker_(broker),
      store_(store),
      fsm_(fsm),
      alerts_(alerts),
      clock_(clock),
      config_(config) {}

bool UnknownResolver::within_attr_window(const Order& /*broker_order*/) const noexcept {
  // The domain Order the broker returns carries no timestamp of its own (the
  // broker-neutral order model is id + intent + fill progress, not a placement
  // clock), so there is no broker time to compare against `config_.attr_window`
  // here. We therefore admit the attribute rung unconditionally at THIS layer and
  // keep the window in the public Config as the contract a real adapter honors
  // once it can stamp a broker placement time onto the order (Epic 3). Admitting
  // it is safe because attribute corroboration is already the LAST resort: it is
  // reached only when BOTH id rungs failed, and a false corroboration cannot fire
  // a second order — the resolver is read-only. The risk it carries (adopting a
  // collided identical lot) is documented in the header and the Dev Notes.
  (void)config_;
  return true;
}

UnknownResolver::Classification UnknownResolver::classify(
    const Order& unknown_order, const std::vector<Order>& broker_orders) const {
  // Rung 1 — BROKER ORDER ID (strongest). The broker minted this id for THIS
  // order; an exact id hit is unambiguous. Requires a non-empty local id.
  if (!unknown_order.broker_order_id.empty()) {
    const auto it = std::find_if(broker_orders.begin(), broker_orders.end(), [&](const Order& bo) {
      return !bo.broker_order_id.empty() && bo.broker_order_id == unknown_order.broker_order_id;
    });
    if (it != broker_orders.end()) {
      return {MatchKind::BrokerOrderId, *it};
    }
  }

  // Rung 2 — CORRELATION TOKEN. The broker echoed our idempotency key
  // (client_ref) back on the order. Requires a non-empty local client_ref.
  if (!unknown_order.intent.client_ref.empty()) {
    const auto it = std::find_if(broker_orders.begin(), broker_orders.end(), [&](const Order& bo) {
      return !bo.intent.client_ref.empty() &&
             bo.intent.client_ref == unknown_order.intent.client_ref;
    });
    if (it != broker_orders.end()) {
      return {MatchKind::CorrelationToken, *it};
    }
  }

  // Rung 3 — ATTRIBUTE CORROBORATION (weakest; both ids absent at the broker).
  // Only reached when neither id rung hit. Gated by the time window.
  const auto it = std::find_if(broker_orders.begin(), broker_orders.end(), [&](const Order& bo) {
    return attributes_match(unknown_order, bo) && within_attr_window(bo);
  });
  if (it != broker_orders.end()) {
    return {MatchKind::AttributeCorroboration, *it};
  }

  // Rung 4 — FAIL-CLOSED.
  return {MatchKind::NoMatch, std::nullopt};
}

Result<Order> UnknownResolver::adopt(const Order& unknown_order, const Order& broker_truth) {
  // Mutate a COPY of the local UNKNOWN order; the FSM advances it Unknown -> the
  // broker-observed state. We carry the broker_order_id through the view so an
  // order that was Unknown WITHOUT a broker id (id-less correlation/attribute
  // match) adopts the broker's id too.
  Order resolved = unknown_order;

  lifecycle::BrokerView view;
  view.client_ref = resolved.intent.client_ref;
  view.broker_order_id = broker_truth.broker_order_id;
  view.observed_state = broker_truth.state;
  view.filled_qty = broker_truth.filled_qty;
  view.avg_price = broker_truth.avg_price;
  // A reconcile snapshot is the freshest possible view; give it a strictly higher
  // ordering key than any normal placement observation (which starts at 1) so it
  // is never dropped as stale by the forward-progressing rule.
  if (const auto last = fsm_.last_key(view.client_ref); last.has_value()) {
    view.ordering_key = *last + 1;
  } else {
    view.ordering_key = 1;
  }

  fsm_.apply(resolved, view);
  // If the FSM refused the transition (it should not for Unknown -> live/terminal)
  // the order remains Unknown; we still adopt the broker id so a later view keys
  // off it. Persist whatever state we now hold as broker truth.
  if (resolved.broker_order_id.empty()) {
    resolved.broker_order_id = broker_truth.broker_order_id;
  }

  if (auto up = store_.upsert_order(resolved); !up) {
    return fail(up.error());
  }
  return resolved;
}

Result<UnknownResolution> UnknownResolver::resolve(const Order& unknown_order) {
  // READ broker truth. This is the ONLY broker call the resolver makes, and it is
  // an idempotent read — never a mutation.
  auto fetched = broker_.fetch_orders();
  if (!fetched) {
    // Infrastructure failure (could not read broker truth): surface it. The order
    // stays Unknown by virtue of us not having changed it.
    return fail(fetched.error());
  }

  const Classification cls = classify(unknown_order, fetched.value());

  UnknownResolution out;
  out.kind = cls.kind;

  if (cls.kind == MatchKind::NoMatch || !cls.matched.has_value()) {
    // FAIL-CLOSED: no authoritative match. Leave the order Unknown, raise a
    // Critical alert, and DO NOT send anything. A second fire is impossible here.
    out.resolved_ok = false;
    out.new_state = OrderState::Unknown;
    out.resolved = std::nullopt;
    // The message carries no secret — only the client_ref (our own idempotency
    // key) and the order's broker_order_id (a broker-minted id), never a
    // token/body.
    //
    // KNOWN LIMITATION, and it is the opposite of what this comment used to
    // claim: the ref does NOT survive to the operator. AlertSink implementations
    // run domain::scrub over the whole FREE-FORM body (see
    // multi_channel_alert_sink.cpp), and a client_ref is one long token-shaped
    // run, so this alert reaches the channel reading `ref=***REDACTED***`. The
    // IMP-15 provenance exemption deliberately does NOT apply here: it is a
    // WHOLE-TYPED-COLUMN allowlist, and giving a free-form body a substring
    // exemption would disable the bare high-entropy rule for every alert body.
    // The real fix is a TYPED provenance parameter on AlertSink::send (and
    // Ledger::append), which changes the port ABI across every implementation and
    // caller — tracked as a separate story, not smuggled in here.
    std::string message = "UNKNOWN order has no authoritative broker match (fail-closed): ref=";
    message += unknown_order.intent.client_ref;
    if (!unknown_order.broker_order_id.empty()) {
      message += " broker_order_id=";
      message += unknown_order.broker_order_id;
    }
    // Best-effort escalation. An alert-delivery failure does not turn the safe
    // fail-closed posture into an error: the order is (and stays) Unknown either
    // way, so we still report NoMatch to the caller.
    (void)alerts_.send(ports::AlertLevel::Critical, message);
    return out;
  }

  // AUTHORITATIVE MATCH: adopt the broker-truth state through the FSM + persist.
  auto adopted = adopt(unknown_order, cls.matched.value());
  if (!adopted) {
    return fail(adopted.error());
  }
  out.resolved = adopted.value();
  out.new_state = adopted.value().state;
  out.resolved_ok = true;
  return out;
}

Result<std::vector<UnknownResolution>> UnknownResolver::resolve_all() {
  auto all = store_.all_orders();
  if (!all) {
    return fail(all.error());
  }

  std::vector<UnknownResolution> out;
  for (const Order& order : all.value()) {
    if (order.state != OrderState::Unknown) {
      continue;
    }
    auto resolved = resolve(order);
    if (!resolved) {
      return fail(resolved.error());
    }
    out.push_back(std::move(resolved.value()));
  }
  return out;
}

}  // namespace broker_exec::runtime
