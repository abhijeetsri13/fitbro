#include "broker_exec/reconcile/manual_intervention.hpp"

#include <string>
#include <unordered_set>
#include <utility>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/lifecycle/lifecycle.hpp"

namespace broker_exec::reconcile {

namespace {

// Absolute value of a signed quantity, as a plain integer (no float, no <cmath>).
[[nodiscard]] std::int64_t abs_qty(std::int64_t value) noexcept {
  return value < 0 ? -value : value;
}

// The broker's net quantity for `symbol` in this snapshot, or 0 if the broker
// no longer reports the symbol (absent == flat).
[[nodiscard]] std::int64_t broker_net_for(const ReconcileResult& truth,
                                          const std::string& symbol) noexcept {
  for (const domain::Position& pos : truth.positions) {
    if (pos.symbol == symbol) {
      return pos.net_qty.value();
    }
  }
  return 0;
}

// Two orders are the same order if their (non-empty) client_refs match (primary
// key) or, failing that, their (non-empty) broker_order_ids match (fallback).
// Mirrors reconciler.cpp's same_order so order identity is consistent.
[[nodiscard]] bool same_order(const domain::Order& a, const domain::Order& b) noexcept {
  if (!a.intent.client_ref.empty() && a.intent.client_ref == b.intent.client_ref) {
    return true;
  }
  if (!a.broker_order_id.empty() && a.broker_order_id == b.broker_order_id) {
    return true;
  }
  return false;
}

// A redaction-safe quantity tag for messages, e.g. "believed=50 broker=0". The
// signed integer net quantities are NOT secrets.
[[nodiscard]] std::string qty_tag(std::int64_t believed, std::int64_t broker) {
  return "believed=" + std::to_string(believed) + " broker=" + std::to_string(broker);
}

// The typed provenance for a position-level event (IMP-16). Only the INSTRUMENT
// is known here — there is no order and therefore no client_ref/broker_order_id.
//
// WHY THE SYMBOL LEFT THE BODY: a sink scrubs the whole free-form body, and
// scrub()'s bare high-entropy rule redacts any >=20-char run mixing letters and
// digits. "PositionClosedManually BANKNIFTY24JUN52000CE believed=50 broker=0"
// therefore reached the operator naming NO INSTRUMENT (NIFTY24JUN24000CE at 17
// chars survived; FINNIFTY/BANKNIFTY/MIDCPNIFTY option symbols at 20-22 did not).
// ports::AlertContext::symbol is rendered through the whole-column symbol
// allowlist and appended after the scrub, so it arrives intact.
[[nodiscard]] ports::AlertContext symbol_provenance(const std::string& symbol) {
  ports::AlertContext provenance;
  provenance.symbol = symbol;
  return provenance;
}

}  // namespace

std::string_view to_string(ManualInterventionEvent::Kind kind) noexcept {
  switch (kind) {
    case ManualInterventionEvent::Kind::PositionClosedManually:
      return "PositionClosedManually";
    case ManualInterventionEvent::Kind::PositionReducedManually:
      return "PositionReducedManually";
    case ManualInterventionEvent::Kind::OrderCancelledManually:
      return "OrderCancelledManually";
  }
  return "Unknown";
}

std::vector<ManualInterventionEvent> ManualInterventionDetector::detect(
    const std::vector<domain::Position>& believed_positions,
    const std::vector<domain::Order>& local_orders, const ReconcileResult& truth) const {
  std::vector<ManualInterventionEvent> events;

  // ── The "bot has live intent on this symbol" set ──
  // SIMPLIFICATION (documented): a non-terminal OR Filled bot order on the symbol
  // suppresses a manual-position flag (safety bias: suppress over false-page). A
  // non-terminal order is live bot intent; a Filled order is a completed bot order
  // that plausibly closed/changed the position, so a flat/reduced broker state is
  // explained by the bot, not a human. Rejected/Cancelled are excluded — they did
  // not change the position. This is intentionally coarse — it does not match side
  // or quantity. A precise side/qty match (only a SELL on a long, only for the
  // closed quantity, counts as an explanation) is a future refinement.
  // The trade-off is a deliberate false-NEGATIVE: a manual close that happens
  // while a bot order is also live/filled on the same symbol is treated as
  // explained and missed. We accept that here because the safety-critical
  // direction is to NOT send a duplicate exit — over-suppressing a flag is safer
  // than double-exiting. The main loop is expected to fold its own fills into the
  // believed-position book BEFORE calling detect(); this Filled backstop covers
  // the residual (the believed book here is the pre-reconcile, stale-open book, so
  // without it a normal exit fill would be mislabelled a manual close).
  std::unordered_set<std::string> bot_intent_symbols;
  for (const domain::Order& order : local_orders) {
    if (order.intent.symbol.empty()) {
      continue;
    }
    if (!lifecycle::is_terminal(order.state) || order.state == domain::OrderState::Filled) {
      bot_intent_symbols.insert(order.intent.symbol);
    }
  }

  // ── Position-level manual close / reduce ──
  for (const domain::Position& believed : believed_positions) {
    const std::int64_t believed_qty = believed.net_qty.value();
    if (believed_qty == 0) {
      continue;  // A flat belief cannot be manually closed/reduced.
    }

    // A close/reduce explained by a live bot order on this symbol is normal — skip.
    if (bot_intent_symbols.count(believed.symbol) != 0) {
      continue;
    }

    const std::int64_t broker_qty = broker_net_for(truth, believed.symbol);

    if (broker_qty == 0) {
      // Broker flat, bot believed open, no bot order explains it -> manual close.
      ManualInterventionEvent event;
      event.kind = ManualInterventionEvent::Kind::PositionClosedManually;
      event.symbol = believed.symbol;
      event.believed_qty = believed_qty;
      event.broker_qty = 0;
      // `event.detail` is an IN-PROCESS typed record handed to callers/tests
      // verbatim and never run through a scrubbing sink, so it keeps naming the
      // symbol inline. The ALERT body deliberately does not — see
      // symbol_provenance above.
      event.detail = std::string(to_string(event.kind)) + " " + believed.symbol + " " +
                     qty_tag(believed_qty, 0);
      const std::string alert_body =
          std::string(to_string(event.kind)) + " " + qty_tag(believed_qty, 0);
      (void)alerts_.send_with_context(ports::AlertLevel::Warning, alert_body,
                                      symbol_provenance(believed.symbol));
      events.push_back(std::move(event));
      continue;
    }

    // Reduced: same-sign smaller magnitude, OR the sign flipped (a manual
    // reverse). A broker magnitude >= believed with the same sign is NOT a
    // reduce (matched or grew — nothing the bot must react to here).
    const bool sign_flipped = (broker_qty < 0) != (believed_qty < 0);
    const bool same_sign_reduced = !sign_flipped && abs_qty(broker_qty) < abs_qty(believed_qty);
    if (sign_flipped || same_sign_reduced) {
      ManualInterventionEvent event;
      event.kind = ManualInterventionEvent::Kind::PositionReducedManually;
      event.symbol = believed.symbol;
      event.believed_qty = believed_qty;
      event.broker_qty = broker_qty;
      event.detail = std::string(to_string(event.kind)) + " " + believed.symbol + " " +
                     qty_tag(believed_qty, broker_qty);
      const std::string alert_body =
          std::string(to_string(event.kind)) + " " + qty_tag(believed_qty, broker_qty);
      (void)alerts_.send_with_context(ports::AlertLevel::Warning, alert_body,
                                      symbol_provenance(believed.symbol));
      events.push_back(std::move(event));
    }
  }

  // ── Order-level manual cancel ──
  // A local order the bot believes live (NOT terminal) that the broker has
  // ALREADY acked (non-empty broker_order_id) but the snapshot now shows absent
  // OR present-with-a-CANCELLED-state, with no bot-initiated cancel, is an
  // UNEXPLAINED (manual) cancel. ASSUMPTION (documented): this detector cannot
  // cheaply tell whether the bot requested the cancel, so it treats any
  // unexplained broker-side cancel as manual — the dispatcher owns bot-initiated
  // cancels and would not have a live local order here for one it requested.
  for (const domain::Order& local : local_orders) {
    if (lifecycle::is_terminal(local.state) || local.broker_order_id.empty()) {
      continue;  // pre-ack or already terminal -> not a "vanished live order".
    }

    bool cancelled_or_absent = true;
    for (const domain::Order& broker_order : truth.orders) {
      if (same_order(local, broker_order)) {
        // Present at the broker: only a CANCELLED state counts as a manual cancel;
        // any other live state means the order is fine.
        cancelled_or_absent = broker_order.state == domain::OrderState::Cancelled;
        break;
      }
    }

    if (cancelled_or_absent) {
      ManualInterventionEvent event;
      event.kind = ManualInterventionEvent::Kind::OrderCancelledManually;
      event.symbol = local.intent.symbol;
      event.client_ref = local.intent.client_ref;
      // `event.detail` stays as it was: it is an IN-PROCESS typed record, handed
      // to callers/tests verbatim and never run through a scrubbing sink, so it
      // keeps naming the ref inline.
      event.detail = std::string(to_string(event.kind)) + " ref=" + local.intent.client_ref +
                     " symbol=" + local.intent.symbol;
      // THE ALERT NAMES THE ORDER (IMP-16). Its body is deliberately NOT
      // `event.detail`: a sink scrubs the whole free-form body
      // (multi_channel_alert_sink.cpp) and a client_ref is one long token-shaped
      // run, so `ref=<client_ref>` used to reach the operator as
      // `ref=***REDACTED***`. The ref now travels in the TYPED ports::AlertContext,
      // which the sink renders through the whole-column allowlist and appends as
      // ` [client_ref=... broker_order_id=... strategy=...]` — the body's own
      // redaction is untouched, and a column that is not id-shaped is still
      // redacted.
      //
      // THE SYMBOL LEFT THE BODY TOO. `symbol=<...>` was still interpolated here,
      // and it has the SAME defect the ids had: an option symbol of >=20 chars
      // (BANKNIFTY24JUN52000CE) is a token-shaped run to scrub(), so it shipped as
      // `symbol=***REDACTED***`. It now rides in the typed context, measured
      // against the instrument-symbol shape rather than the id one.
      const std::string alert_body = std::string(to_string(event.kind));
      ports::AlertContext provenance;
      provenance.client_ref = local.intent.client_ref;
      provenance.broker_order_id = local.broker_order_id;
      provenance.strategy = local.intent.strategy;
      provenance.symbol = local.intent.symbol;
      (void)alerts_.send_with_context(ports::AlertLevel::Warning, alert_body, provenance);
      events.push_back(std::move(event));
    }
  }

  return events;
}

void ManualInterventionDetector::reconcile_positions(
    const ReconcileResult& truth, std::vector<domain::Position>& local_positions) const {
  // For each local position adopt broker truth: the matching broker net_qty +
  // avg_price, or flat (Quantity::of(0) + a zero/flat avg_price) if the broker no
  // longer reports it. This is the state update that makes the no-duplicate-exit
  // guarantee structural — a manually-closed position reads flat and needs_exit()
  // is then false. ONLY positions the bot already tracks locally are updated.
  //
  // DEFERRED: detection/handling of a manually-OPENED position (one the broker
  // reports but the bot has NO local entry for) is deliberately not done here —
  // the bot must not adopt+auto-exit a position it never opened. Such broker-only
  // positions are intentionally NOT appended to the local book.
  for (domain::Position& local : local_positions) {
    const domain::Position* match = nullptr;
    for (const domain::Position& broker_pos : truth.positions) {
      if (broker_pos.symbol == local.symbol) {
        match = &broker_pos;
        break;
      }
    }
    if (match != nullptr) {
      local.net_qty = match->net_qty;
      local.avg_price = match->avg_price;
    } else {
      local.net_qty = domain::Quantity::of(0);         // broker no longer reports it -> flat.
      local.avg_price = domain::Price::from_paise(0);  // flatten avg_price alongside net_qty.
    }
  }
}

}  // namespace broker_exec::reconcile
