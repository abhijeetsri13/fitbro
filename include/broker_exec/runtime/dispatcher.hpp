#pragma once

// broker_exec::runtime::Dispatcher — the single broker-mutation chokepoint
// (Story 1.9, FR-8/FR-10, NFR-1/NFR-2/NFR-3). THE SAFETY CENTERPIECE.
//
// WHAT THIS IS: the one synchronous path through which EVERY broker mutation
// (place / modify / cancel / square-off) flows. Each mutation performs, in this
// exact order on a single thread with NO hand-off between the durability and the
// send:
//
//     record-intent  ->  fsync  ->  [pre_send_barrier]  ->  send  ->  record-result|UNKNOWN
//
// The intent is durably on disk (intentlog::IntentLog::append fsyncs BEFORE it
// returns) before `broker.*` is ever called. That is what makes "a crash never
// produces a duplicate or an un-enumerable order" structurally true: on replay
// the intent is enumerable and the reconciler (Epic 3) resolves it — we never
// have to guess whether a send happened.
//
// NO BLIND RETRY (FR-10): a Timeout / Network failure on a mutation carries
// SuggestedAction::ReconcileFirst. Such a result is DANGEROUS — the order may
// have reached the exchange (see FakeBroker::ack_lost_but_placed). The dispatcher
// NEVER repeats the call. It marks the order domain::OrderState::Unknown,
// persists it, appends a Result(unknown) intent record, and returns the Unknown
// order so the reconcile/replay machinery (Stories 1.10 / Epic 3) can resolve it
// against broker truth. A blind retry here is the exact failure mode that creates
// a duplicate; it is forbidden by construction.
//
// SINGLE SEND PATH (the "chokepoint" invariant): the dispatcher holds the ONLY
// `ports::BrokerPort&` in the runtime. No other runtime code is given the broker
// reference, so no mutation can bypass record->fsync->send->record. This is
// enforced structurally (the broker is a private member reached only through the
// four public mutation methods) and by review/convention (conventions.md: "The
// single broker-mutation path is dispatch()"). The behavioral test in
// dispatcher_test.cpp proves the dispatcher is the sole mutation path by counting
// FakeBroker::request_count().
//
// SINGLE-THREADED / SYNCHRONOUS (NFR-2): there is no async, no threading, no
// thread hand-off anywhere between the fsync and the send. The decision core is
// synchronous; correctness over speed.
//
// CROSS-PLATFORM: C++20 standard library only (<functional>, <string>,
// <string_view>, <optional>). No OS APIs, no `#ifdef`, no floating point. All
// durability/time is delegated to injected ports (IntentLog/ClockPort), all
// money/price is exact integer paise via the domain types.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "broker_exec/domain/types.hpp"
#include "broker_exec/idempotency/idempotency.hpp"
#include "broker_exec/intentlog/intent_log.hpp"
#include "broker_exec/lifecycle/lifecycle.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/clock_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/store/store.hpp"

namespace broker_exec::runtime {

// The single synchronous broker-mutation chokepoint. Constructed once at
// composition time with references to every collaborator it coordinates; all
// references must outlive the Dispatcher (reference-injection, matching the rest
// of the codebase). Not thread-safe by design — it is touched only on the main
// loop (NFR-2).
class Dispatcher {
 public:
  // Collaborators (all by reference, must outlive this object):
  //   broker  — the ONLY BrokerPort in the runtime; the chokepoint owns it.
  //   log     — the write-ahead intent log; append() fsyncs before returning.
  //   store   — the SQLite projection (persisted Order rows; UNIQUE(client_ref)).
  //   index   — the in-memory idempotency index (signature -> client_ref).
  //   uuids   — the UUID source for minting new client-refs.
  //   fsm     — the lifecycle engine (applies the success broker view).
  //   clock   — the injected time source (wall stamp on records).
  Dispatcher(ports::BrokerPort& broker, intentlog::IntentLog& log, store::Store& store,
             idempotency::IdempotencyIndex& index, idempotency::UuidGenerator& uuids,
             lifecycle::LifecycleEngine& fsm, ports::ClockPort& clock);

  Dispatcher(const Dispatcher&) = delete;
  Dispatcher& operator=(const Dispatcher&) = delete;
  Dispatcher(Dispatcher&&) = delete;
  Dispatcher& operator=(Dispatcher&&) = delete;
  ~Dispatcher() = default;

  // PLACE a new order idempotently.
  //   1. reserve(): if the signal was already submitted -> return the EXISTING
  //      order and perform ZERO broker sends (the duplicate-submit guard).
  //   2. else: append a PlaceOrder intent (using the canonical
  //      idempotency::intent_payload_json so restart-dedup works), which FSYNCs,
  //      then run pre_send_barrier(), then broker.place():
  //        * success            -> persist the Order (Sent->Acknowledged via the
  //                                 FSM), append a Result record, return the Order.
  //        * Timeout/Network     -> mark the Order Unknown, persist, append a
  //          (ReconcileFirst)       Result(unknown) record, return the Unknown
  //                                 order. NEVER retried (FR-10).
  //        * clean rejection     -> mark the Order Rejected, persist, append a
  //          (DoNotRetry)           Result record, return the Rejected order.
  //        * other error         -> treated conservatively as Unknown (reconcile).
  // `strategy` is the owning strategy id embedded in the minted client-ref
  // (normally intent.strategy). The returned Order always carries the reserved
  // client_ref.
  [[nodiscard]] Result<domain::Order> place(std::string_view strategy,
                                            const domain::OrderIntent& intent);

  // MODIFY an acknowledged order at the broker. Records a ModifyOrder intent
  // (fsync) -> barrier -> broker.modify(); a Timeout/Network result marks the
  // order Unknown (reconcile, never retry). `intent` is the new order parameters;
  // its client_ref identifies the local order being modified.
  [[nodiscard]] Result<domain::Order> modify(const std::string& broker_order_id,
                                             const domain::OrderIntent& intent);

  // CANCEL an order at the broker. Records a CancelOrder intent (fsync) ->
  // barrier -> broker.cancel(); a Timeout/Network result marks the local order
  // Unknown (reconcile, never retry). `client_ref` identifies the local order.
  [[nodiscard]] Result<ports::Ok> cancel(const std::string& broker_order_id,
                                         std::string_view client_ref);

  // SQUARE-OFF (flatten) the position behind an order. Records a SquareOff intent
  // (fsync) -> barrier -> broker.square_off(); a Timeout/Network result marks the
  // local order Unknown (reconcile, never retry). `client_ref` identifies the
  // local order.
  [[nodiscard]] Result<ports::Ok> square_off(const std::string& broker_order_id,
                                             std::string_view client_ref);

  // Install a barrier invoked AFTER the intent record is fsync'd but BEFORE the
  // broker send, on every mutation. Production leaves it empty (the default is a
  // no-op). Story 1.12's SIGKILL durability harness installs a hook here that
  // kills the process at exactly this point to prove durability precedes the
  // send; the fsync-before-send test in dispatcher_test.cpp installs a hook that
  // reads the on-disk intent log and asserts the record is already present.
  void set_pre_send_barrier(std::function<void()> barrier);

 private:
  // Append a result/outcome record for `client_ref` to the intent log. The
  // payload is a small, redaction-safe JSON object describing the outcome (state
  // + optional broker_order_id). Best-effort: a log-append failure is propagated
  // as the call's Error (durability of the OUTCOME record is on the same hot
  // path as the intent).
  [[nodiscard]] Result<intentlog::IntentRecord> append_result(std::string_view client_ref,
                                                              std::string_view outcome_json);

  // Run the installed pre-send barrier (no-op if none). Centralized so every
  // mutation observes the SAME record->fsync->[barrier]->send ordering.
  void run_pre_send_barrier();

  // True iff an error is a "dangerous, reconcile-don't-retry" outcome: a Timeout
  // or Network failure (SuggestedAction::ReconcileFirst), or the catch-all
  // Unknown. These mark the order UNKNOWN; anything else with action DoNotRetry is
  // a clean rejection.
  [[nodiscard]] static bool is_reconcile_first(const errors::Error& error) noexcept;

  // Wall-clock nanoseconds-since-epoch read from the injected ClockPort, stamped
  // into the result/outcome records as provenance (never from a direct
  // system_clock call — the injected port is the single time source, NFR/FR-23).
  [[nodiscard]] std::int64_t wall_ns() const noexcept;

  ports::BrokerPort& broker_;
  intentlog::IntentLog& log_;
  store::Store& store_;
  idempotency::IdempotencyIndex& index_;
  idempotency::UuidGenerator& uuids_;
  lifecycle::LifecycleEngine& fsm_;
  ports::ClockPort& clock_;
  std::function<void()> pre_send_barrier_;  // no-op until installed
};

}  // namespace broker_exec::runtime
