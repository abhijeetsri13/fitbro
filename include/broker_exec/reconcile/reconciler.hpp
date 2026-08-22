#pragma once

// broker_exec::reconcile — continuous reconciliation (Story 3.1, FR-11).
//
// THE FETCH-OFF-LOOP / APPLY-ON-LOOP SPLIT. Broker truth is reconciled against
// local state by two structurally separated halves:
//
//   * FETCH side (`Reconciler`) — READS ONLY. It performs the four broker reads
//     (orders/trades/positions/funds) off the main loop and packages them into
//     an immutable `ReconcileResult`. It holds NO Store and NO LifecycleEngine,
//     so it *cannot* write: the fetch-off-loop guarantee is enforced by the type,
//     not by discipline (COH-1 / CC-4).
//   * APPLY side (`ReconcileApplier`) — THE SOLE WRITER. Running on the main
//     loop, it folds a `ReconcileResult` into local order state by reusing the
//     lifecycle FSM (`LifecycleEngine::apply`, forward-progressing +
//     terminal-absorbing — never re-implemented here). It is the ONLY function
//     that mutates `local_orders` / the engine, and it raises an alert + blocks
//     new orders on any mismatch (AC-3).
//
// Adaptive cadence (AC-2): poll tight (~1-2s) while anything is in-flight or a
// position is open, loose (~15-30s) when flat. Cadence is integer milliseconds
// (no float, per conventions).
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`, no
// floating point. No throw across the boundary (fetch returns Result<T>).

#include <chrono>
#include <cstdint>
#include <vector>

#include "broker_exec/domain/types.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/clock_port.hpp"

namespace broker_exec::lifecycle {
// Forward-declared: the engine is held by reference and only its apply() is
// called from reconciler.cpp, so the public header does not depend on the
// lifecycle header (keeps the lifecycle link PRIVATE).
class LifecycleEngine;
}  // namespace broker_exec::lifecycle

namespace broker_exec::reconcile {

// The immutable snapshot a single fetch produces: the four broker reads plus the
// apply-ordering discriminator and a wall stamp. IMMUTABLE after construction —
// the apply side only reads it. Members are public values but no code mutates a
// ReconcileResult once `Reconciler::fetch` has built it (it is passed by const
// reference everywhere downstream).
struct ReconcileResult {
  std::vector<domain::Order> orders;
  std::vector<domain::Trade> trades;
  std::vector<domain::Position> positions;
  ports::FundsSnapshot funds{};
  // Monotonic snapshot sequence — the apply-ordering key. A full reconciler
  // snapshot is authoritative over a single WS push, so callers set this high
  // (CC-7). Threaded straight into every BrokerView the applier builds.
  std::int64_t ordering_key = 0;
  std::chrono::system_clock::time_point fetched_at{};
};

// The FETCH side. READS ONLY — holds ONLY the clock (NO Store, NO
// LifecycleEngine): the fetch side structurally cannot write (fetch-off-loop
// guarantee, AC-1 / COH-1).
class Reconciler {
 public:
  explicit Reconciler(const ports::ClockPort& clock) noexcept : clock_(clock) {}

  // Perform the four idempotent broker reads and package them into an immutable
  // ReconcileResult stamped with `snapshot_seq` as the ordering_key and the
  // clock's wall time. If ANY read fails, return that typed Error (the trigger
  // handler decides the policy, e.g. reconcile-first / retry-safe). Does NOTHING
  // else — no writes, no state.
  [[nodiscard]] Result<ReconcileResult> fetch(ports::BrokerPort& broker,
                                              std::int64_t snapshot_seq) const;

 private:
  const ports::ClockPort& clock_;
};

// What a single apply() produced, for logging / metrics and the new-order gate.
struct ReconcileOutcome {
  int applied = 0;                // Broker orders matched to a local order and fed to the FSM.
  int advanced = 0;               // Of those, how many the FSM advanced (ApplyOutcome::Applied).
  int dropped_stale = 0;          // Of those, how many were stale/duplicate (DroppedStale).
  int mismatches = 0;             // Phantom broker orders + vanished local orders.
  bool block_new_orders = false;  // Set on any mismatch (AC-3).
};

// The APPLY side: the SOLE writer. Reuses the lifecycle FSM (never reimplements
// transitions) and raises an alert + blocks new orders on any mismatch.
class ReconcileApplier {
 public:
  ReconcileApplier(lifecycle::LifecycleEngine& engine, ports::AlertSink& alerts) noexcept
      : engine_(engine), alerts_(alerts) {}

  // Fold `result` into `local_orders` (the ONLY mutation point). For each broker
  // order: match a local order by intent.client_ref (fallback broker_order_id),
  // build a BrokerView (observed_state = broker order's state, ordering_key =
  // result.ordering_key) and call engine.apply(). A broker order with no local
  // match is a phantom mismatch; a non-terminal local order the broker has
  // ALREADY acknowledged (non-empty broker_order_id) yet that is absent from the
  // snapshot is a vanished mismatch (a non-terminal local order with an EMPTY
  // broker_order_id is pre-ack — a normal place->reconcile race, NOT a mismatch).
  // Any mismatch -> Warning alert (redaction safe: client_ref only, never a
  // token) + block_new_orders. No throw: a failing alert send still counts the
  // mismatch.
  //
  // STALE-SNAPSHOT GUARD (apply-ordering): a snapshot whose `ordering_key` is not
  // newer than one already applied is stale/out-of-order. Its matched views are
  // still fed to the FSM (harmless — the engine drops stale per-order keys) but
  // NO mismatch is escalated from it (no phantom/vanished alert, no block) and
  // the returned outcome has block_new_orders=false. NON-const: the applier is
  // the sole stateful writer and tracks the high-water snapshot key.
  ReconcileOutcome apply(const ReconcileResult& result, std::vector<domain::Order>& local_orders);

 private:
  lifecycle::LifecycleEngine& engine_;
  ports::AlertSink& alerts_;
  // High-water mark of the newest snapshot ordering_key whose mismatch logic has
  // run. A snapshot with ordering_key <= this is stale (see apply()). -1 means no
  // snapshot has been applied yet.
  std::int64_t last_snapshot_key_ = -1;
};

// ── Adaptive cadence (AC-2) ────────────────────────────────────────────────

// The two facts that drive cadence: is anything working at the broker, and is
// any position open. Either one keeps us polling tight.
struct ReconcileState {
  bool any_inflight = false;
  bool any_open_position = false;
};

// Tight while anything is in-flight or a position is open, loose when flat.
// Integer milliseconds only (no float). Defaults: tight 1500ms, loose 20000ms.
[[nodiscard]] std::chrono::milliseconds next_cadence(
    const ReconcileState& state, std::chrono::milliseconds tight = std::chrono::milliseconds(1500),
    std::chrono::milliseconds loose = std::chrono::milliseconds(20000));

// Derive the cadence inputs from the current orders + positions.
//   any_inflight       = any order in a working/uncertain state at the broker:
//                        PendingSend, Sent, Acknowledged, PartiallyFilled,
//                        Unknown, or PartiallyPlaced (terminal and pre-send
//                        Created/Validated states are NOT in-flight). This is
//                        inclusive of the SENT/UNKNOWN states the AC calls out.
//   any_open_position  = any position with a non-zero net_qty.
[[nodiscard]] ReconcileState derive_state(const std::vector<domain::Order>& orders,
                                          const std::vector<domain::Position>& positions);

}  // namespace broker_exec::reconcile
