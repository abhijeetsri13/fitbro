# Story 1.10 — UNKNOWN handling and match-key precedence

**Status:** review
**Epic:** 1 — Cross-Platform Safety Core
**Module:** `broker_exec::runtime` (additive to the Story 1.9 dispatcher)

---

## Story

As an operator,
I want uncertain orders resolved against broker truth by a defined precedence,
So that ambiguity never resolves into a duplicate or wrong order. (FR-9, FR-10)

When `dispatch()` (Story 1.9) cannot tell whether a dangerous mutation reached the
exchange, it records the order as `OrderState::Unknown` and **never blindly
retries**. Story 1.10 supplies the read-only machinery that resolves those UNKNOWN
orders against broker truth — and the process-wide pause that keeps new risk from
piling onto an unresolved ambiguity.

---

## Acceptance Criteria

**Given** an order in UNKNOWN
**When** the library resolves it
**Then** new risky entries are paused process-wide (risk-reducing exits exempt) until resolved
**And** matching uses precedence: broker order_id > short correlation token > attribute corroboration > fail-closed
**And** when no authoritative match exists, the order stays UNKNOWN with an alert — never a second fire.

---

## Tasks

- [x] `UnknownPause` — a process-wide latch keyed on the SET of outstanding UNKNOWN
  client refs. `mark_unknown` (idempotent) / `clear` (per-ref no-op) / `is_paused`
  / `outstanding` / `allows(is_risk_reducing_exit)`. Entries blocked while paused;
  risk-reducing exits always allowed.
- [x] `MatchKind` + `UnknownResolution` (the binding result type) + `to_string`.
- [x] `UnknownResolver` — `resolve(order)` and `resolve_all()`. Read-only on the
  broker (`fetch_orders()` only), classifies via the precedence ladder, adopts
  broker truth through the FSM + store on a match, raises a Critical alert and
  stays UNKNOWN on NoMatch.
- [x] `src/runtime/unknown_pause_test.cpp`, `src/runtime/unknown_resolver_test.cpp`
  with an in-test `RecordingAlertSink` mock.
- [x] `src/runtime/CMakeLists.txt` — added the two sources to `broker_exec_runtime`
  (moved `store`/`lifecycle` to PUBLIC deps as they are now in the module's public
  API), and added two test executables with `add_test`. No top-level CMake change.

---

## Dev Notes

### The precedence ladder (BINDING; stop at the FIRST authoritative match)

Implemented in `UnknownResolver::classify` over `broker.fetch_orders()`:

1. **Broker order id (strongest).** If the UNKNOWN order already carries a
   `broker_order_id` and a broker order has the same id, that id was minted by the
   broker for this very order — unambiguous. `MatchKind::BrokerOrderId`.
2. **Short correlation token.** Else, if a broker order's client-ref/tag equals the
   UNKNOWN order's `intent.client_ref` (the idempotency key we sent and the broker
   echoed), it is authoritative. `MatchKind::CorrelationToken`.
3. **Attribute corroboration (weakest).** Else, if a broker order matches on
   `(symbol, side, quantity, price)` within the time window, treat it as a
   corroborated match. Reached ONLY when both id rungs miss.
   `MatchKind::AttributeCorroboration`.
4. **Fail-closed.** No match → `MatchKind::NoMatch`: the order **stays Unknown**, a
   `AlertLevel::Critical` alert is raised via `AlertSink`, and **nothing is sent**.

On any of rungs 1–3 the resolver adopts the broker-truth state onto the local
order through `LifecycleEngine::apply` (a `BrokerView` with a strictly-increasing
`ordering_key` so the reconcile snapshot is never dropped as stale), then
`store.upsert_order`s the result. `resolved_ok = true` for 1–3, `false` for 4.

### Attribute-corroboration RISK note

Rung 3 is the weakest evidence and is **deliberately last**: attributes can collide
across genuinely distinct orders (two identical lots at the same price). It is
therefore admitted only when **both** id rungs fail, and is gated by
`Config::attr_window` (default 5s) — a recency guard so a stale identical order does
not get adopted. The current broker-neutral `domain::Order` carries no placement
timestamp, so `within_attr_window` admits the rung at this layer and the window
lives in the public `Config` as the contract a real adapter (Epic 3) honours once it
can stamp a broker placement time. Admitting it is **safe-by-construction** because
the resolver is **read-only**: even a false corroboration cannot fire a second order
— at worst it adopts a wrong-but-equivalent broker order, and a true NoMatch still
fails closed. This trade-off (prefer resolving an ambiguous order over leaving it
UNKNOWN forever, accepting a small collision risk on the weakest rung) is the
documented design decision for this story.

### No second fire / read-only invariant

The resolver holds a `ports::BrokerPort&` but calls **only** `fetch_orders()` (an
idempotent read). It never places/modifies/cancels/squares-off. Resolving an
UNKNOWN must not create the duplicate the UNKNOWN exists to prevent — proven by the
fail-closed test asserting `FakeBroker::request_count()` advances by exactly the one
read and the book is unchanged.

### Pause semantics

`UnknownPause` is membership-based (a set of refs), not a counter: `mark_unknown`
is idempotent and `clear` is a per-ref no-op, so the dispatcher (which may re-record
an UNKNOWN across replays) and the resolver (which clears exactly the refs it
resolves) compose without drift. The gate is asymmetric: a risk-reducing **exit**
is always allowed (blocking it would trap a live position behind the pause — the
unsafe outcome); a risk-increasing **entry** is blocked while paused.

### Constraints honoured

Cross-platform C++20 stdlib only; no OS APIs / `#ifdef`; no floating point (money
and price stay exact integer paise); time only via the injected `ClockPort`;
`#pragma once`; 2-space / 100-col `.clang-format`; single-threaded main-loop owner
(NFR-2). Read-only against the broker (no second fire).

---

## Completion Record

### Files created

- `include/broker_exec/runtime/unknown_resolver.hpp`
- `src/runtime/unknown_resolver.cpp`
- `include/broker_exec/runtime/unknown_pause.hpp`
- `src/runtime/unknown_pause.cpp`
- `src/runtime/unknown_resolver_test.cpp`
- `src/runtime/unknown_pause_test.cpp`

### Files edited

- `src/runtime/CMakeLists.txt` — added the two sources to `broker_exec_runtime`,
  moved `broker_exec_store` / `broker_exec_lifecycle` to PUBLIC link deps (now in
  the public API), and registered `broker_exec_runtime_unknown_resolver_tests` and
  `broker_exec_runtime_unknown_pause_tests` via `add_test`. The existing dispatcher
  sources/tests are untouched. **No top-level / `tests/` CMake changes.**

### Tests (mapped to ACs)

- `resolver` (a) broker_order_id match adopts broker truth, `resolved_ok`.
- `resolver` (b) correlation-token match when the local broker id is empty.
- `resolver` (c) attribute corroboration when neither id matches.
- `resolver` (d) **NoMatch fails closed**: stays Unknown, a Critical alert is
  raised, NOTHING sent (`request_count()` advances only by the read, book
  unchanged).
- `resolver` precedence: broker_order_id wins over a colliding attribute match.
- `resolver` `resolve_all`: resolves only the Unknown orders, leaves others alone.
- `pause` (e): blocks an entry while paused, allows a risk-reducing exit, clears on
  resolve; idempotent mark / per-ref clear / multi-ref pause.

### Committed API

```cpp
namespace broker_exec::runtime {

enum class MatchKind { BrokerOrderId, CorrelationToken, AttributeCorroboration, NoMatch };
std::string_view to_string(MatchKind kind) noexcept;

struct UnknownResolution {
  MatchKind kind = MatchKind::NoMatch;
  std::optional<domain::Order> resolved;
  domain::OrderState new_state = domain::OrderState::Unknown;
  bool resolved_ok = false;
};

class UnknownResolver {
 public:
  struct Config { std::chrono::seconds attr_window{5}; };
  UnknownResolver(ports::BrokerPort&, store::Store&, lifecycle::LifecycleEngine&,
                  ports::AlertSink&, ports::ClockPort&, Config = {});
  Result<UnknownResolution> resolve(const domain::Order& unknown_order);
  Result<std::vector<UnknownResolution>> resolve_all();
};

class UnknownPause {
 public:
  void mark_unknown(std::string client_ref);
  void clear(std::string_view client_ref);
  bool is_paused() const noexcept;
  std::size_t outstanding() const noexcept;
  bool allows(bool is_risk_reducing_exit) const noexcept;
};

}  // namespace broker_exec::runtime
```

### Integration caveats

- **Wiring is the composition root's job, not this story's.** Nothing yet calls
  `UnknownResolver` / `UnknownPause` — the dispatcher records UNKNOWN, the future
  main loop (Epic 3 reconciler) must (a) `mark_unknown` when the dispatcher returns
  an Unknown order, (b) call `resolve`/`resolve_all` against broker truth, and
  (c) `clear` each ref the resolver returns `resolved_ok` for.
- **Resolver and pause do not auto-couple.** `resolve` does not call
  `pause.clear()` itself (it has no `UnknownPause&`); the caller clears the ref on a
  successful resolution. This keeps the resolver a pure read-only classifier.
- **Attribute window is a forward contract.** `within_attr_window` admits the rung
  unconditionally until the order model carries a broker placement timestamp
  (Epic 3); real adapters should populate it and the resolver should then enforce
  `Config::attr_window`.
- **Alert delivery is best-effort.** A failed `AlertSink::send` does not flip the
  fail-closed posture into an error — the order is (and stays) Unknown either way.
```
