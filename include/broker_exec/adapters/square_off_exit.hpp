#pragma once

// broker_exec::adapters — THE DETERMINISTIC SQUARE-OFF EXIT REF (IMP-13, AC-1c/AC-2).
//
// ONE definition, shared by every adapter that implements a real flatten, so the
// two brokers cannot drift apart on the single string that makes a replayed
// square_off idempotent. Header-only and dependency-free (std::string only): it is
// a naming rule, not a module.
//
// ── WHAT A FLATTEN NEEDS FROM A NAME ────────────────────────────────────────
// `square_off` is the EMERGENCY exit. It runs on the paths where a process has
// just crashed, a supervisor is restarting, or an operator is hammering a panic
// button — precisely the conditions under which the SAME square-off is invoked
// twice. The second invocation must not open a SECOND opposite position, which is
// a brand-new naked leg in the opposite direction: worse than the position it was
// sent to close.
//
// The exit therefore carries a ref that is a pure function of the parent, so a
// fetch-first check at the broker can look for THAT exact exit before placing
// anything (see each adapter's square_off).
//
// ── WHAT THIS NAME DOES *NOT* BUY YOU (the claim this header used to overstate)
// It is tempting to write that the store's `UNIQUE(client_ref)` backstop (Story
// 1.6) and the idempotency index (Story 1.7) "also dedupe" the exit. THEY DO NOT
// DEDUPE THE ADAPTER'S EXIT, and believing they do is exactly the kind of
// imagined second lock that stops a reviewer from checking the first one.
//
// The reason is structural: an adapter's `square_off` builds this ref, uses it
// locally, and then calls the broker DIRECTLY. It never reaches `dispatch()`, so
// it is never inserted into the store and never claims an idempotency key — and
// on Kotak the ref does not even reach the WIRE (that adapter deliberately sends
// no client tag at all). A uniqueness constraint on a row nobody writes cannot
// reject anything, and a broker that never saw the string cannot reject it either.
//
// So the exit's actual duplicate guards, in full, are:
//   1. the WIRE-LEVEL anchor each adapter keys on (Kite: a `tag` derived from the
//      parent broker order id, which the broker echoes back; Kotak: attribute
//      corroboration against the live book, because it has no verified echo), and
//   2. the fetch-first check that consults that anchor BEFORE placing.
// That is TWO guards, not four, and both of them live in the adapters. This
// header only guarantees that the NAME is deterministic and cannot collide with a
// slicer child. If a future change routes the flatten through `dispatch()` (the
// tracked tier-2 item), the store/idempotency backstops become real for it — and
// this paragraph should be rewritten then, not before.
//
// ── THE SUFFIX IS `#X`, AND THAT IS A COLLISION DECISION ────────────────────
// The freeze-slicer (Story 2.9) already owns the `<parent>#<k>` child namespace
// with k >= 1, i.e. every child suffix is a run of DECIMAL DIGITS. `X` is not a
// digit, so `exit_client_ref(p)` can never equal `idempotency::child_ref(p, k)`
// for any k — a slice and a flatten of the same parent are always distinct orders.
// (Pinned by a parity test in the conformance suites; if the slicer ever admits a
// non-numeric suffix, that test fails before two orders can collide live.)
//
// The form is deliberately still `<parent>#<suffix>`, so `is_child_ref()` is true
// and `parent_of()` recovers the parent — the lifecycle FSM's parent/child fold
// keeps working on an exit without knowing this rule exists.
//
// ── WHAT "PARENT" MEANS HERE (the restart caveat, stated plainly) ───────────
// The anchor is the parent's `client_ref` WHEN THE ADAPTER CAN STILL NAME IT. An
// adapter's correlation maps are in-memory, so after a process restart a
// square-off may only know the parent's BROKER ORDER ID. That id is broker truth
// and is just as deterministic, so it is the documented fallback anchor — a
// restarted process still derives a stable exit name for the same parent, it is
// simply a different (equally unique) one from the pre-crash name.
//
// THE CONSEQUENCE, SAID OUT LOUD: the client-ref anchor alone does NOT make the
// pre-crash and post-crash exits the same string. That is why the wire-level
// duplicate guard in each adapter keys on something that is ALWAYS broker truth
// (Kite: a tag derived from the parent broker order id; Kotak: attribute
// corroboration against the book), and why the fetch-first check is mandatory
// rather than an optimization. This header names the order; the adapters are what
// refuse to place a second one.
//
// ── ONE EXIT PER PARENT, AND NO NUMBERED SIBLINGS ───────────────────────────
// There is deliberately NO `#X2` / `#X3` namespace. A flatten that discovers its
// existing exit is TOO SMALL for the position (the parent kept filling after the
// exit was sized) does not top up with a second, smaller order — it refuses and
// raises an alert. Two reasons, both learned from the same race:
//   * a top-up would have to be sized from `required - existing`, i.e. from the
//     ORDERED size of an exit whose own fill state we have not established; the
//     evidence that just proved itself stale is not evidence to fire a third
//     order on, and
//   * on Kotak a second look-alike exit makes the attribute rung AMBIGUOUS, which
//     turns the NEXT replay into a refusal anyway — after two orders are live.
// So the suffix stays a single `#X`, and "the exit does not cover the position"
// is an operator condition, not an automatic one. See each adapter's
// `...-SQUAREOFF-EXITSHORT` branch.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`, no float.

#include <string>
#include <string_view>

namespace broker_exec::adapters {

// The suffix that marks a square-off exit child ref. NOT a digit (see above).
inline constexpr std::string_view kExitRefSuffix = "#X";

// The deterministic exit client_ref for a parent anchor: `<anchor>#X`.
//
// `parent_anchor` is the parent's client_ref when the adapter can name it, else
// the parent's broker order id (see the restart caveat above). An EMPTY anchor
// yields an empty ref — a caller must never build an exit name out of nothing;
// the adapters treat that as "no deterministic name available" and fail closed.
[[nodiscard]] inline std::string exit_client_ref(std::string_view parent_anchor) {
  if (parent_anchor.empty()) {
    return std::string{};
  }
  std::string ref(parent_anchor);
  ref.append(kExitRefSuffix);
  return ref;
}

// True iff `client_ref` was produced by exit_client_ref (i.e. it ends in the exit
// suffix and has a non-empty anchor before it). Lets a caller tell a flatten apart
// from a slice without re-implementing the rule.
[[nodiscard]] inline bool is_exit_ref(std::string_view client_ref) noexcept {
  return client_ref.size() > kExitRefSuffix.size() &&
         client_ref.substr(client_ref.size() - kExitRefSuffix.size()) == kExitRefSuffix;
}

}  // namespace broker_exec::adapters
