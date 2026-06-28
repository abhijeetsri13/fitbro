#pragma once

// broker_exec::feedsub — the websocket resubscribe guard (connected-but-mute
// defense).
//
// THE FAILURE THIS PREVENTS. A market-data websocket disconnects (close 1006)
// and reconnects. The socket is "up" again — but ticks silently STOP, because
// the client never re-issued the subscriptions it held before the drop. Kite
// and Kotak-Neo developers hit this constantly: Kotak's feed dies ~2 min in and
// its built-in reconnect does NOT resubscribe, so the strategy keeps trading on
// a last price that never moves. The socket lies: "connected" is not "ticking".
//
// THE GUARD'S TWO JOBS (and ONLY these two):
//   1. OWN THE DESIRED SUBSCRIPTION SET. Every subscribe/unsubscribe maintains
//      the FULL set of tokens the caller wants. On EVERY (re)connect the guard
//      hands back that full set so the caller deterministically RE-ISSUES every
//      subscription — never relying on the broker/socket to remember.
//   2. VERIFY THE RESUBSCRIBE ACTUALLY DELIVERS. Re-issuing a subscribe frame is
//      not proof a tick will flow. After a reconnect each token is armed with a
//      first-tick DEADLINE; a token that has not ticked by its deadline is
//      flagged MuteAfterResubscribe — the connected-but-mute failure — and is
//      NOT tradable. A token must PROVE it ticks post-resubscribe before trading
//      resumes on it (fail-closed).
//
// COMPLEMENTARY TO marketdata, NOT a dependency. `marketdata` classifies
// per-symbol staleness ONCE ticks flow (Live/Stale/Delayed/...). This guard owns
// the layer BEFORE that: the subscription set, the reconnect-resubscribe, and
// the first-tick-after-resubscribe deadline. It does not depend on marketdata.
//
// Time is injected (FR-23): every deadline is measured on the MONOTONIC clock
// via the `ClockPort` (now_steady) — never an ambient std::chrono::now. The
// deadline is evaluated LAZILY: state_of/mute_tokens/is_tradable each recompute
// against now_steady() at call time, so no timer/loop tick is needed to "fire"
// the mute transition.
//
// Conventions: no-throw; integer std::chrono durations only (no float); tokens
// are non-secret instrument ids (redaction-safe to log). NOT thread-safe — it
// lives on, and is driven by, the single main loop. The injected ClockPort MUST
// outlive the guard.

#include <chrono>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "broker_exec/ports/clock_port.hpp"

namespace broker_exec::feedsub {

// Per-token feed state across the reconnect/resubscribe lifecycle. Stable,
// log-friendly names (see to_string) — renaming one is a breaking observability
// change (NFR-8). Only `Live` is tradable.
enum class TokenFeedState {
  Subscribed,            // in the desired set; not currently awaiting a post-(re)connect
                         // tick. The inert baseline — NOT tradable (it has not proven it
                         // ticks since the last reconnect), but NOT mute either.
  AwaitingFirstTick,     // re-subscribed after a (re)connect; no tick seen yet and the
                         // first-tick deadline has NOT elapsed. NOT tradable (yet).
  Live,                  // a tick arrived after the resubscribe — healthy. The ONLY
                         // tradable state.
  MuteAfterResubscribe   // re-subscribed, the first-tick deadline ELAPSED with no tick:
                         // the connected-but-mute failure. NOT tradable; the caller
                         // blocks trading on this token until it ticks again.
};

// Stable, log/serialization-friendly state names (NFR-8 observability contract).
[[nodiscard]] std::string_view to_string(TokenFeedState state) noexcept;

// The resubscribe guard. Owns the desired subscription set and the
// reconnect-resubscribe verification. NOT thread-safe (single main loop).
class ResubscribeGuard {
 public:
  // Bind the guard to a monotonic clock and the first-tick deadline applied per
  // token after each (re)connect.
  //
  // FAIL-CLOSED CLAMP: a non-positive `first_tick_deadline` would otherwise mean
  // "the first tick is already overdue" or "never check" — both unsafe. A
  // deadline <= 0 is clamped UP to a small positive floor (see kMinDeadline) so
  // the mute check is NEVER disabled. A non-positive deadline must still arm and
  // still fire the connected-but-mute detection.
  ResubscribeGuard(const ports::ClockPort& clock, std::chrono::milliseconds first_tick_deadline);

  // The positive floor a non-positive deadline is clamped to. Any positive value
  // satisfies the fail-closed contract (the check stays armed); kept small so a
  // misconfigured deadline still flags mute promptly rather than silently
  // tolerating a silent feed.
  static constexpr std::chrono::milliseconds kMinDeadline{1};

  // Add `token` to the desired subscription set. IDEMPOTENT: a token already in
  // the set is left exactly as it is (its current phase/deadline is NOT reset).
  // A newly added token starts `Subscribed` — fail-closed: it is NOT tradable
  // until a reconnect arms it and a tick proves it (a freshly-subscribed token
  // that has never connected must not trade).
  void subscribe(const std::string& token);

  // Remove `token` from the desired set. It is no longer re-issued on reconnect
  // and no longer appears in mute_tokens(). A no-op for an unknown token.
  void unsubscribe(const std::string& token);

  // THE CORE. Call on EVERY (re)connect. Returns the FULL desired subscription
  // set in deterministic (sorted) order — the EXACT set the caller MUST re-issue
  // as subscribe frames (the guard does not send; it tells you what to send).
  // Simultaneously arms verification: EVERY token is set `AwaitingFirstTick` with
  // a fresh first-tick deadline = now_steady() + first_tick_deadline. A token
  // that was Live before the reconnect goes BACK to AwaitingFirstTick — it must
  // re-prove it ticks on the new socket before it is tradable again.
  [[nodiscard]] std::vector<std::string> on_reconnect();

  // Record that a tick arrived for `token` -> mark it `Live` (clears
  // AwaitingFirstTick / any pending mute). A tick for an unsubscribed/unknown
  // token is IGNORED (it never creates a spurious entry or state).
  void record_tick(const std::string& token);

  // The token's current state, evaluated LAZILY against now_steady(): an
  // AwaitingFirstTick token whose deadline has passed resolves to
  // MuteAfterResubscribe here (no timer needed). An UNKNOWN token (never
  // subscribed) resolves to `Subscribed` — the inert baseline: not tradable and
  // not mute. (The enum is kept minimal; Subscribed and "unknown" share identical
  // tradability semantics, so they are deliberately indistinguishable here.)
  [[nodiscard]] TokenFeedState state_of(const std::string& token) const;

  // The tokens that are connected-but-mute: subscribed, re-issued on a reconnect,
  // and STILL silent past their first-tick deadline (MuteAfterResubscribe),
  // computed against now_steady(). Deterministic (sorted) order. The caller
  // blocks trading on every token in this list. A token that ticked before its
  // deadline is Live and never appears here.
  [[nodiscard]] std::vector<std::string> mute_tokens() const;

  // True ONLY when state_of(token) == Live. FAIL-CLOSED: AwaitingFirstTick (not
  // yet ticked), MuteAfterResubscribe (proven mute), Subscribed (never armed),
  // and unknown tokens are ALL non-tradable. A token must prove it ticks
  // post-resubscribe before it is traded.
  [[nodiscard]] bool is_tradable(const std::string& token) const;

 private:
  // Stored per-token phase. NOTE: MuteAfterResubscribe is NEVER stored — it is
  // derived lazily from `AwaitingFirstTick` + an elapsed `first_tick_deadline`.
  // The three stored phases are Subscribed / AwaitingFirstTick / Live.
  struct Entry {
    TokenFeedState phase = TokenFeedState::Subscribed;
    // Valid only while phase == AwaitingFirstTick: the monotonic instant at/after
    // which silence becomes mute. Boundary is STRICT: now_steady() == deadline is
    // still AwaitingFirstTick; now_steady() > deadline is MuteAfterResubscribe.
    std::chrono::steady_clock::time_point first_tick_deadline{};
  };

  // Resolve an entry's stored phase to its effective state against now_steady()
  // (the single place the lazy mute transition is computed).
  [[nodiscard]] TokenFeedState resolve(const Entry& entry) const;

  const ports::ClockPort& clock_;
  std::chrono::milliseconds first_tick_deadline_;
  // The desired subscription set. std::map (ordered by token) so on_reconnect()
  // and mute_tokens() yield a DETERMINISTIC order without an extra sort.
  std::map<std::string, Entry> tokens_;
};

}  // namespace broker_exec::feedsub
