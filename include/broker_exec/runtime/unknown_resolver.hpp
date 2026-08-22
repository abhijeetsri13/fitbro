#pragma once

// broker_exec::runtime::UnknownResolver — resolve an UNKNOWN order against broker
// truth by a strict match-key precedence (Story 1.10, FR-9/FR-10).
//
// WHAT THIS IS: the read-only counterpart to the Dispatcher (Story 1.9). When a
// dangerous mutation yields a Timeout/Network result the Dispatcher records the
// order as OrderState::Unknown and NEVER blindly retries — the order may or may
// not have reached the exchange. This module resolves that ambiguity by FETCHING
// broker truth and matching the UNKNOWN order against it through a defined
// precedence ladder, so an ambiguous order becomes either a confirmed order or
// stays UNKNOWN-with-an-alert — but never a SECOND FIRE.
//
// READ-ONLY / NO SECOND FIRE (the whole point): the resolver holds a BrokerPort
// but calls ONLY fetch_orders() (an idempotent read). It NEVER places, modifies,
// cancels, or squares off. Resolving an UNKNOWN must not be able to create the
// very duplicate the UNKNOWN exists to prevent. The tests assert this by counting
// FakeBroker::request_count() (it advances only by the read).
//
// THE PRECEDENCE LADDER (stop at the FIRST authoritative match):
//   1. BROKER ORDER ID — if the UNKNOWN order already carries a broker_order_id
//      and a broker order has the SAME id, that is the strongest possible match:
//      the broker itself minted that id for this order. Authoritative.
//   2. CORRELATION TOKEN — else, if a broker order's client-ref/tag equals the
//      UNKNOWN order's intent.client_ref (the idempotency key we sent), the broker
//      echoed our token back. Authoritative.
//   3. ATTRIBUTE CORROBORATION — else, if a broker order matches on
//      (symbol, side, quantity, price) AND no stronger evidence exists, treat it
//      as a corroborated match. This is the WEAKEST rung: attributes can collide
//      across distinct orders (two identical lots), so it is gated by a time
//      window and used ONLY when both ids are absent. Documented risk; see the
//      .cpp and the Dev Notes.
//   4. FAIL-CLOSED — no authoritative match: the order STAYS Unknown, a Critical
//      alert is raised via AlertSink, and nothing is sent. Fail-closed is the safe
//      default: we would rather hold an UNKNOWN (and pause entries) than guess.
//
// SINGLE WRITER (NFR-2): applied only on the main loop, like the Dispatcher and
// the LifecycleEngine. Not thread-safe by design.
//
// CROSS-PLATFORM: C++20 standard library only (<optional>, <vector>).
// No OS APIs, no `#ifdef`, no floating point — money/price stay exact integer
// paise via the domain types, and time flows through the injected ClockPort.

#include <optional>
#include <string_view>
#include <vector>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/lifecycle/lifecycle.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/clock_port.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/store/store.hpp"

namespace broker_exec::runtime {

// Which rung of the precedence ladder produced a resolution (or NoMatch). Stable,
// log-friendly ordering strongest -> weakest -> none (observability contract).
enum class MatchKind { BrokerOrderId, CorrelationToken, AttributeCorroboration, NoMatch };

// Stable, log/serialization-friendly name for a MatchKind (NFR-8).
[[nodiscard]] std::string_view to_string(MatchKind kind) noexcept;

// The outcome of resolving a single UNKNOWN order.
struct UnknownResolution {
  // The rung that matched (or NoMatch). The whole decision is captured here.
  MatchKind kind = MatchKind::NoMatch;
  // The broker-truth order we matched to, when an authoritative match was found.
  // Empty on NoMatch.
  std::optional<domain::Order> resolved;
  // The order's state AFTER resolution: the adopted broker-truth state on a match,
  // or OrderState::Unknown when it stays unresolved.
  domain::OrderState new_state = domain::OrderState::Unknown;
  // True iff an authoritative match was found (kinds 1-3). False on NoMatch.
  bool resolved_ok = false;
};

// Resolves UNKNOWN orders against broker truth using the precedence ladder.
// Constructed once at composition time with references to every collaborator;
// all references must outlive the resolver (reference-injection, matching the
// rest of the codebase). Not thread-safe by design — main loop only (NFR-2).
class UnknownResolver {
 public:
  // ── RUNG 3 IS NOT TIME-BOUNDED. IT USED TO SAY IT WAS. ────────────────────
  //
  // This class carried a `Config { std::chrono::seconds attr_window{5}; }` and a
  // stored `ClockPort&`, and the comments here said the attribute rung was
  // "gated by the time window" — "a safe-by-default guard against stale
  // collisions". None of it was ever enforced. `within_attr_window()` returned
  // `true` unconditionally and did `(void)config_;`, because `domain::Order`
  // carries no placement time to compare `clock_.now()` against; no caller ever
  // set the window; and nothing read it. A knob that silently does nothing is
  // worse than no knob, because it invites an operator to tighten the window and
  // believe they have. All three are gone rather than silenced.
  //
  // WHAT THAT MEANS NOW, STATED PLAINLY: attribute corroboration admits a broker
  // row of matching attributes however old it is. That is tolerable here for two
  // specific reasons, and only these two:
  //   * it is the LAST rung, reached only when the broker order id AND the
  //     correlation token both failed to match, and
  //   * this resolver is READ-ONLY — it never sends. The worst case is adopting
  //     a collided identical lot, not firing a second order.
  // It is still weaker than the header used to claim. To bound it for real,
  // `domain::Order` needs a broker placement timestamp, populated by both
  // adapters; the clock parameter below is retained for that day.
  //
  // Collaborators (all by reference, must outlive this object):
  //   broker — read-only here: ONLY fetch_orders() is called (no second fire).
  //   store  — the projection; a resolved order is upsert'd back as broker truth.
  //   fsm    — the lifecycle engine; adopts the broker-truth state via apply().
  //   alerts — the operator escalation path; a NoMatch raises a Critical alert.
  //   clock  — CURRENTLY UNUSED. Kept so the signature does not churn twice: it
  //            is what a real attribute window will be measured on once an order
  //            carries a broker placement time. Do not read it as evidence that
  //            time is considered today; it is not.
  UnknownResolver(ports::BrokerPort& broker, store::Store& store, lifecycle::LifecycleEngine& fsm,
                  ports::AlertSink& alerts, ports::ClockPort& clock);

  UnknownResolver(const UnknownResolver&) = delete;
  UnknownResolver& operator=(const UnknownResolver&) = delete;
  UnknownResolver(UnknownResolver&&) = delete;
  UnknownResolver& operator=(UnknownResolver&&) = delete;
  ~UnknownResolver() = default;

  // Resolve a SINGLE UNKNOWN order against broker truth via the precedence ladder.
  // Reads broker.fetch_orders() once, classifies, and on a match adopts the
  // broker-truth state through the FSM and upserts the order back to the store. On
  // NoMatch raises a Critical alert and leaves the order Unknown. NEVER sends a
  // mutation. The Result error state is reserved for an infrastructure failure
  // (e.g. the broker read itself failed) — a clean "no match" is a successful
  // UnknownResolution with kind == NoMatch, not an Error.
  [[nodiscard]] Result<UnknownResolution> resolve(const domain::Order& unknown_order);

  // Resolve EVERY order currently in OrderState::Unknown in the store, in store
  // order. Each is resolved independently; the vector preserves one resolution per
  // UNKNOWN order. A broker-read or store failure short-circuits with that Error.
  [[nodiscard]] Result<std::vector<UnknownResolution>> resolve_all();

 private:
  // Classify `unknown_order` against the fetched broker truth via the ladder. Pure
  // (no I/O, no mutation) so the precedence is unit-testable in isolation: returns
  // the matched broker order + the rung, or NoMatch.
  struct Classification {
    MatchKind kind = MatchKind::NoMatch;
    std::optional<domain::Order> matched;
  };
  [[nodiscard]] Classification classify(const domain::Order& unknown_order,
                                        const std::vector<domain::Order>& broker_orders) const;

  // Adopt a matched broker-truth order's state onto the local UNKNOWN order via
  // the FSM (Unknown -> the observed state) and upsert the result. Returns the
  // resolved local order.
  [[nodiscard]] Result<domain::Order> adopt(const domain::Order& unknown_order,
                                            const domain::Order& broker_truth);

  ports::BrokerPort& broker_;
  store::Store& store_;
  lifecycle::LifecycleEngine& fsm_;
  ports::AlertSink& alerts_;
};

}  // namespace broker_exec::runtime
