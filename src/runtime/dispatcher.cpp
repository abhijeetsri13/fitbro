#include "broker_exec/runtime/dispatcher.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include "broker_exec/domain/enums.hpp"

namespace broker_exec::runtime {

namespace {

using domain::Order;
using domain::OrderIntent;
using domain::OrderState;
using errors::Error;
using errors::ErrorCategory;
using errors::SuggestedAction;

// ── Tiny, dependency-free JSON-string helpers ──────────────────────────────
// The runtime module is C++20-stdlib-only (no nlohmann_json), and the Result
// payloads we write to the intent log are small, fixed-shape objects over data
// WE control (state names, a broker order id). We escape defensively anyway so a
// surprising broker_order_id can never produce a malformed log line.

// Append `value` as a JSON-escaped string body (no surrounding quotes) to `out`.
void append_escaped(std::string& out, std::string_view value) {
  for (const char c : value) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        // Control chars -> \u00XX; everything else verbatim (UTF-8 passes through).
        if (static_cast<unsigned char>(c) < 0x20) {
          static constexpr char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
          out += kHex[static_cast<unsigned char>(c) & 0xF];
        } else {
          out += c;
        }
        break;
    }
  }
}

// A one-line outcome object: {"state":"<STATE>","wall_ts_ns":<n>} (+ optional
// broker_order_id). Used as the Result record payload. `wall_ts_ns` is provenance
// stamped from the dispatcher's injected ClockPort.
std::string outcome_json(OrderState state, std::string_view broker_order_id,
                         std::int64_t wall_ts_ns) {
  std::string out = R"({"state":")";
  append_escaped(out, domain::to_string(state));
  out += '"';
  if (!broker_order_id.empty()) {
    out += R"(,"broker_order_id":")";
    append_escaped(out, broker_order_id);
    out += '"';
  }
  out += R"(,"wall_ts_ns":)";
  out += std::to_string(wall_ts_ns);
  out += '}';
  return out;
}

// A one-line outcome object for an op that has no order body (cancel/square-off):
// {"op":"<OP>","outcome":"<OUTCOME>","wall_ts_ns":<n>} (+ optional broker_order_id).
std::string op_outcome_json(std::string_view op, std::string_view result,
                            std::string_view broker_order_id, std::int64_t wall_ts_ns) {
  std::string out = R"({"op":")";
  append_escaped(out, op);
  out += R"(","outcome":")";
  append_escaped(out, result);
  out += '"';
  if (!broker_order_id.empty()) {
    out += R"(,"broker_order_id":")";
    append_escaped(out, broker_order_id);
    out += '"';
  }
  out += R"(,"wall_ts_ns":)";
  out += std::to_string(wall_ts_ns);
  out += '}';
  return out;
}

// Build a fresh Order from an intent in a given state (dispatch OWNS creation of a
// new order; the FSM applies broker VIEWS to an existing order). The intent must
// already carry its reserved client_ref.
Order make_order(const OrderIntent& intent, OrderState state) {
  Order order;
  order.intent = intent;
  order.state = state;
  return order;
}

}  // namespace

Dispatcher::Dispatcher(ports::BrokerPort& broker, intentlog::IntentLog& log, store::Store& store,
                       idempotency::IdempotencyIndex& index, idempotency::UuidGenerator& uuids,
                       lifecycle::LifecycleEngine& fsm, ports::ClockPort& clock)
    : broker_(broker),
      log_(log),
      store_(store),
      index_(index),
      uuids_(uuids),
      fsm_(fsm),
      clock_(clock) {}

void Dispatcher::set_pre_send_barrier(std::function<void()> barrier) {
  pre_send_barrier_ = std::move(barrier);
}

void Dispatcher::run_pre_send_barrier() {
  if (pre_send_barrier_) {
    pre_send_barrier_();
  }
}

std::int64_t Dispatcher::wall_ns() const noexcept {
  const auto since_epoch = clock_.now_wall().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count();
}

bool Dispatcher::is_reconcile_first(const Error& error) noexcept {
  // The dangerous, "reconcile-don't-retry" outcomes: a timeout/network failure
  // carries ReconcileFirst, and an unparseable/ambiguous error is conservatively
  // treated the same (mark Unknown, never blindly repeat). A DoNotRetry error is a
  // clean rejection (the broker gave a verdict) and does NOT become Unknown.
  if (error.action == SuggestedAction::ReconcileFirst) {
    return true;
  }
  return error.category == ErrorCategory::Timeout || error.category == ErrorCategory::Network ||
         error.category == ErrorCategory::Unknown;
}

Result<intentlog::IntentRecord> Dispatcher::append_result(std::string_view client_ref,
                                                          std::string_view outcome) {
  return log_.append(intentlog::IntentOp::Result, std::string(client_ref), std::string(outcome));
}

// ── PLACE ───────────────────────────────────────────────────────────────────
Result<Order> Dispatcher::place(std::string_view strategy, const OrderIntent& intent) {
  // (1) Idempotent reserve. A duplicate signal returns the existing order and
  //     performs ZERO broker sends — the duplicate-submit guard.
  auto reserved = idempotency::reserve(index_, store_, uuids_, strategy, intent);
  if (!reserved) {
    return fail(reserved.error());
  }
  const idempotency::Reservation& reservation = reserved.value();

  if (!reservation.is_new) {
    // Already submitted. Return the stored Order if the projection has it; else
    // synthesize a minimal Unknown order carrying the prior ref (it was reserved
    // but not yet projected — the safe, reconcile-able posture). NO send.
    if (reservation.existing.has_value()) {
      return reservation.existing.value();
    }
    OrderIntent prior = intent;
    prior.client_ref = reservation.client_ref;
    return make_order(prior, OrderState::Unknown);
  }

  // A fresh order. Stamp the reserved client_ref onto our intent copy so the
  // intent-log payload, the broker call, and the persisted Order all agree (this
  // is what idempotency::intent_payload_json keys restart-dedup on).
  OrderIntent placed = intent;
  placed.client_ref = reservation.client_ref;

  // (2) record-intent -> fsync. The canonical PlaceOrder payload is REQUIRED so a
  //     restart can rebuild the idempotency index from the log.
  auto recorded = log_.append(intentlog::IntentOp::PlaceOrder, placed.client_ref,
                              idempotency::intent_payload_json(placed));
  if (!recorded) {
    return fail(recorded.error());
  }

  // (3) barrier (fsync'd intent is now durable) -> send. SINGLE-THREADED: no
  //     hand-off between the fsync above and the send below.
  run_pre_send_barrier();
  auto ack = broker_.place(placed);

  if (ack) {
    // SUCCESS. Build the order as Sent, then apply the broker ack via the FSM
    // (Sent -> Acknowledged) so the lifecycle engine records the ordering key and
    // the broker_order_id exactly as a real broker view would.
    Order order = make_order(placed, OrderState::Sent);
    lifecycle::BrokerView view;
    view.client_ref = placed.client_ref;
    view.broker_order_id = ack.value().broker_order_id;
    view.observed_state = OrderState::Acknowledged;
    view.ordering_key = 1;  // first observation for this fresh order
    fsm_.apply(order, view);

    if (auto ins = store_.insert_order(order); !ins) {
      return fail(ins.error());
    }
    if (auto res = append_result(order.intent.client_ref,
                                 outcome_json(order.state, order.broker_order_id, wall_ns()));
        !res) {
      return fail(res.error());
    }
    return order;
  }

  // FAILURE. Decide UNKNOWN (reconcile-first / ambiguous) vs Rejected (clean
  // do-not-retry verdict). EITHER WAY: never an immediate repeat (FR-10).
  const Error& err = ack.error();
  const OrderState resolved =
      is_reconcile_first(err) ? OrderState::Unknown : OrderState::Rejected;

  Order order = make_order(placed, resolved);
  if (auto ins = store_.insert_order(order); !ins) {
    return fail(ins.error());
  }
  if (auto res = append_result(order.intent.client_ref, outcome_json(order.state, {}, wall_ns()));
      !res) {
    return fail(res.error());
  }
  // Return the persisted order (Unknown/Rejected). We deliberately return the
  // ORDER, not an Error: the order is durably recorded and the caller / reconciler
  // resolves the Unknown against broker truth. A blind retry is impossible here —
  // there is no retry path.
  return order;
}

// ── MODIFY ────────────────────────────────────────────────────────────────────
Result<Order> Dispatcher::modify(const std::string& broker_order_id, const OrderIntent& intent) {
  // record-intent (ModifyOrder) -> fsync. The payload is the canonical intent so
  // the modify is enumerable on replay; the client_ref keys it to the local order.
  auto recorded = log_.append(intentlog::IntentOp::ModifyOrder, intent.client_ref,
                              idempotency::intent_payload_json(intent));
  if (!recorded) {
    return fail(recorded.error());
  }

  run_pre_send_barrier();
  auto ack = broker_.modify(broker_order_id, intent);

  // Load the existing local order (if projected) so we mutate the SAME row.
  auto found = store_.find_order(intent.client_ref);
  if (!found) {
    return fail(found.error());
  }
  Order order = found.value().has_value() ? found.value().value()
                                          : make_order(intent, OrderState::Acknowledged);
  order.broker_order_id = broker_order_id;

  if (ack) {
    // Success: the modify was acknowledged. Keep the order Acknowledged (its
    // fill/terminal progression continues to arrive via reconcile/push views).
    if (order.state == OrderState::Unknown || order.state == OrderState::Created ||
        order.state == OrderState::Validated || order.state == OrderState::PendingSend ||
        order.state == OrderState::Sent) {
      order.state = OrderState::Acknowledged;
    }
    order.broker_order_id = ack.value().broker_order_id;
    if (auto up = store_.upsert_order(order); !up) {
      return fail(up.error());
    }
    if (auto res = append_result(order.intent.client_ref,
                                 outcome_json(order.state, order.broker_order_id, wall_ns()));
        !res) {
      return fail(res.error());
    }
    return order;
  }

  const Error& err = ack.error();
  if (is_reconcile_first(err)) {
    order.state = OrderState::Unknown;
    if (auto up = store_.upsert_order(order); !up) {
      return fail(up.error());
    }
    if (auto res = append_result(order.intent.client_ref,
                                 outcome_json(order.state, broker_order_id, wall_ns()));
        !res) {
      return fail(res.error());
    }
    return order;
  }

  // A clean rejection of the modify (DoNotRetry) leaves the underlying order as it
  // was — we surface the typed Error to the caller (the order itself is unchanged
  // and still valid at the broker).
  if (auto res = append_result(intent.client_ref,
                               op_outcome_json("modify", "rejected", broker_order_id, wall_ns()));
      !res) {
    return fail(res.error());
  }
  return fail(err);
}

// ── CANCEL ────────────────────────────────────────────────────────────────────
Result<ports::Ok> Dispatcher::cancel(const std::string& broker_order_id,
                                     std::string_view client_ref) {
  // record-intent (CancelOrder) -> fsync.
  auto recorded = log_.append(intentlog::IntentOp::CancelOrder, std::string(client_ref),
                              op_outcome_json("cancel", "intent", broker_order_id, wall_ns()));
  if (!recorded) {
    return fail(recorded.error());
  }

  run_pre_send_barrier();
  auto outcome = broker_.cancel(broker_order_id);

  if (outcome) {
    if (auto res =
            append_result(client_ref, op_outcome_json("cancel", "ok", broker_order_id, wall_ns()));
        !res) {
      return fail(res.error());
    }
    return ports::ok();
  }

  const Error& err = outcome.error();
  if (is_reconcile_first(err)) {
    // Dangerous: the cancel may have taken effect (ack-lost-but-cancelled). Mark
    // the local order Unknown so the reconciler resolves it — NEVER retry.
    if (auto found = store_.find_order(client_ref); found && found.value().has_value()) {
      Order order = found.value().value();
      if (!lifecycle::is_terminal(order.state)) {
        order.state = OrderState::Unknown;
        if (auto up = store_.upsert_order(order); !up) {
          return fail(up.error());
        }
      }
    }
    if (auto res = append_result(client_ref,
                                 op_outcome_json("cancel", "unknown", broker_order_id, wall_ns()));
        !res) {
      return fail(res.error());
    }
    return fail(err);
  }

  // Clean rejection (e.g. OrderNotFound / already terminal): surface it.
  if (auto res = append_result(client_ref,
                               op_outcome_json("cancel", "rejected", broker_order_id, wall_ns()));
      !res) {
    return fail(res.error());
  }
  return fail(err);
}

// ── SQUARE-OFF ──────────────────────────────────────────────────────────────
Result<ports::Ok> Dispatcher::square_off(const std::string& broker_order_id,
                                         std::string_view client_ref) {
  // record-intent (SquareOff) -> fsync.
  auto recorded = log_.append(intentlog::IntentOp::SquareOff, std::string(client_ref),
                              op_outcome_json("square_off", "intent", broker_order_id, wall_ns()));
  if (!recorded) {
    return fail(recorded.error());
  }

  run_pre_send_barrier();
  auto outcome = broker_.square_off(broker_order_id);

  if (outcome) {
    if (auto res = append_result(client_ref,
                                 op_outcome_json("square_off", "ok", broker_order_id, wall_ns()));
        !res) {
      return fail(res.error());
    }
    return ports::ok();
  }

  const Error& err = outcome.error();
  if (is_reconcile_first(err)) {
    if (auto found = store_.find_order(client_ref); found && found.value().has_value()) {
      Order order = found.value().value();
      if (!lifecycle::is_terminal(order.state)) {
        order.state = OrderState::Unknown;
        if (auto up = store_.upsert_order(order); !up) {
          return fail(up.error());
        }
      }
    }
    if (auto res = append_result(
            client_ref, op_outcome_json("square_off", "unknown", broker_order_id, wall_ns()));
        !res) {
      return fail(res.error());
    }
    return fail(err);
  }

  if (auto res = append_result(
          client_ref, op_outcome_json("square_off", "rejected", broker_order_id, wall_ns()));
      !res) {
    return fail(res.error());
  }
  return fail(err);
}

}  // namespace broker_exec::runtime
