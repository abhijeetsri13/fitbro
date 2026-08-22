#pragma once

// broker_exec::reconcile — manual-intervention detection (Story 3.2, FR-12).
//
// WHAT THIS IS: the CLASSIFIER that turns broker truth (a 3.1 `ReconcileResult`)
// into named manual-intervention events and, crucially, guarantees the bot never
// sends a duplicate exit after a human has already closed/reduced a position in
// the broker app.
//
// The 3.1 `ReconcileApplier` flags a vanished order as a GENERIC mismatch (block
// new orders + alert). 3.2 goes one level finer: it CLASSIFIES POSITION-level
// manual closes / reduces and ORDER-level manual cancels, and — the AC-2 crux —
// reconciles the local position book to broker truth so a manually-closed
// position reads FLAT before any exit decision. A flat position needs no exit,
// so the no-duplicate-exit guarantee is STRUCTURAL (a property of the state), not
// a discipline the caller must remember.
//
// MANUAL vs LEGIT FILL: a close/reduce that is EXPLAINED by a bot order on that
// symbol is a normal bot-initiated exit (a legit fill), NOT a manual intervention
// — it is not flagged. "Explained" is, deliberately, kept SIMPLE: ANY local
// non-terminal OR Filled order for the symbol counts (live bot intent, or a
// completed bot order that plausibly changed the position). A precise
// side/quantity match is a documented future refinement (see
// manual_intervention.cpp). Likewise an order the broker has
// acked but the snapshot now shows absent/cancelled, with no bot-initiated
// cancel, is treated as an UNEXPLAINED (manual) cancel — the dispatcher owns
// bot-initiated cancels.
//
// ALERT + AUDIT: every event raises a redaction-safe Warning on the AlertSink
// (symbol / client_ref / quantities are NOT secrets) and is RETURNED. The
// returned vector IS the audit record the main loop persists (the audit store is
// Epic 4); this detector's contract is "classify + alert + return for audit".
//
// CROSS-PLATFORM: C++20 standard library only. No OS APIs, no `#ifdef`, no
// floating point (quantities are integer domain::Quantity). No throw: a failing
// AlertSink send is swallowed via `(void)` and the event is still returned.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "broker_exec/domain/types.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/reconcile/reconciler.hpp"

namespace broker_exec::reconcile {

// A single classified manual intervention. A plain value: the main loop persists
// the returned list as the audit record (Epic 4). Quantities are signed integer
// net quantities (positive long, negative short); 0 = flat.
struct ManualInterventionEvent {
  enum class Kind {
    PositionClosedManually,   // believed open -> broker flat, unexplained by a bot order.
    PositionReducedManually,  // believed magnitude > broker magnitude (or sign flipped),
                              // unexplained.
    OrderCancelledManually    // a broker-acked, non-terminal local order absent/cancelled at the
                              // broker.
  };

  Kind kind{Kind::PositionClosedManually};
  std::string symbol;             // The instrument symbol (redaction-safe).
  std::string client_ref;         // For an order cancel: the order's client_ref (redaction-safe).
  std::int64_t believed_qty = 0;  // The bot's believed net_qty before the change.
  std::int64_t broker_qty = 0;    // The broker's authoritative net_qty now.
  std::string detail;             // Free-form, redaction-safe context for logs/audit.
};

// Stable, log/serialization-friendly name for an event Kind (observability /
// audit contract). Used to build the alert + detail messages.
[[nodiscard]] std::string_view to_string(ManualInterventionEvent::Kind kind) noexcept;

// The detector: classify manual broker-app changes against local belief, alert,
// and reconcile the local position book to broker truth so no duplicate exit is
// ever generated. Holds ONLY the AlertSink (no Store, no engine) — it classifies
// and alerts; the main loop owns persistence and any resulting decision.
class ManualInterventionDetector {
 public:
  explicit ManualInterventionDetector(ports::AlertSink& alerts) noexcept : alerts_(alerts) {}

  // Compare the bot's believed positions + live orders against broker truth and
  // return every manual-intervention event found. For each event a redaction-safe
  // Warning is sent on the AlertSink (the send Result is swallowed via `(void)`;
  // a failing send never throws and never drops the event). See the .cpp for the
  // exact close/reduce/cancel rules and the documented "explained by a bot order"
  // simplification. const: detection reads only; reconcile_positions does the
  // state update.
  [[nodiscard]] std::vector<ManualInterventionEvent> detect(
      const std::vector<domain::Position>& believed_positions,
      const std::vector<domain::Order>& local_orders, const ReconcileResult& truth) const;

  // Fold broker truth into the local position book (the AC-1 state update that
  // makes AC-2 structural). Each local position is set to the matching broker
  // position's net_qty + avg_price; a symbol the broker no longer reports becomes
  // net_qty == Quantity::of(0) (flat) with a flat avg_price. ONLY positions the
  // bot already tracks locally are updated. A broker position with NO local entry
  // (a manually-OPENED position) is deliberately NOT adopted — the bot must not
  // auto-exit a position it never opened; that case is deferred. After this call
  // a manually-closed position reads flat, so needs_exit() is false.
  void reconcile_positions(const ReconcileResult& truth,
                           std::vector<domain::Position>& local_positions) const;

  // The AC-2 proof helper: a flat position needs no exit. A pure decision over
  // the position's net quantity — after reconcile_positions a manually-closed
  // position is flat, so this returns false and the bot generates NO second exit.
  [[nodiscard]] static bool needs_exit(const domain::Position& position) noexcept {
    return position.net_qty.value() != 0;
  }

 private:
  ports::AlertSink& alerts_;
};

}  // namespace broker_exec::reconcile
