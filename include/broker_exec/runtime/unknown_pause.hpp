#pragma once

// broker_exec::runtime::UnknownPause — the process-wide UNKNOWN entry gate
// (Story 1.10, FR-9/FR-10).
//
// WHAT THIS IS: a tiny, single-threaded latch that tracks how many orders are
// currently in an unresolved UNKNOWN state. While ANY order is unresolved-
// UNKNOWN the process is "paused": new risk-INCREASING entries are blocked so an
// ambiguous in-flight order can never be compounded by a second fire before it
// is reconciled against broker truth (Story 1.10's no-second-fire guarantee).
//
// THE EXEMPTION (why exits are always allowed): the danger of an UNKNOWN is a
// hidden, already-live position. Blocking a risk-REDUCING exit while paused would
// trap us in that exposure — the opposite of safe. So the gate is asymmetric: it
// blocks entries (which add risk) but always lets exits (which shed risk)
// through. The caller classifies the action; the gate only decides on that flag.
//
// MEMBERSHIP, NOT A COUNTER: pause is keyed on the SET of outstanding client
// refs, not a bare integer. mark_unknown() is idempotent (marking the same ref
// twice does not double-count) and clear() is a no-op for an unknown ref, so the
// dispatcher (which may record the same UNKNOWN more than once across replays)
// and the resolver (which clears exactly the refs it resolves) compose without
// drift. is_paused() is simply "the set is non-empty".
//
// SINGLE WRITER (NFR-2): touched only on the main loop, exactly like the
// Dispatcher and LifecycleEngine. Not thread-safe by design.
//
// CROSS-PLATFORM: C++20 standard library only (<cstddef>, <string>,
// <string_view>, <unordered_set>). No OS APIs, no `#ifdef`, no floating point.

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>

namespace broker_exec::runtime {

// Process-wide UNKNOWN pause latch. While ANY order is unresolved-UNKNOWN, new
// risk-increasing entries are blocked; risk-reducing exits are always allowed.
class UnknownPause {
 public:
  UnknownPause() = default;

  // Record `client_ref` as an outstanding UNKNOWN. Idempotent: marking the same
  // ref again does not change the outstanding count (membership, not a counter).
  void mark_unknown(std::string client_ref);

  // Clear a previously-marked UNKNOWN once it has been resolved against broker
  // truth. A no-op if the ref was never marked (or was already cleared).
  void clear(std::string_view client_ref);

  // True iff at least one order is still unresolved-UNKNOWN (the process is
  // paused). When false, no entry is blocked.
  [[nodiscard]] bool is_paused() const noexcept;

  // The number of distinct outstanding UNKNOWN refs.
  [[nodiscard]] std::size_t outstanding() const noexcept;

  // The gate decision for a single action. While paused, a risk-INCREASING entry
  // (is_risk_reducing_exit == false) is BLOCKED -> returns false; a risk-REDUCING
  // exit (is_risk_reducing_exit == true) is ALWAYS allowed -> returns true. When
  // not paused, everything is allowed.
  [[nodiscard]] bool allows(bool is_risk_reducing_exit) const noexcept;

 private:
  // The set of outstanding (unresolved) UNKNOWN client refs. Pause == non-empty.
  std::unordered_set<std::string> outstanding_;
};

}  // namespace broker_exec::runtime
