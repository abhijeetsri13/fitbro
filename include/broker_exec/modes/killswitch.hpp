#pragma once

// broker_exec::modes — operator kill switches with an authenticated control
// plane (Story 3.8, FR-31). The reliable last line of defense.
//
// THREE PARTS, one chokepoint:
//   * `KillState` — the in-process flag set. The main loop is its SOLE WRITER:
//     it `drain()`s the control queue and `apply()`s each command. Reads are
//     no-throw and may be called every tick. Soft/Strategy/Broker/Account keep
//     the process alive (block new entries, allow risk-reducing exits); Panic
//     blocks everything via the normal gate (the emergency square-off runs
//     OUT-OF-BAND, see Story 3.7's `allows_risk_reducing_exits`).
//   * `KillController` — the authenticated control plane. `submit()`
//     AUTHENTICATES, then PERSISTS BEFORE it acks (AC-3 durability), then
//     enqueues onto a thread-safe queue so a CLI/control thread can hand a kill
//     to the main loop. Fail-closed: an unauthenticated command is rejected with
//     NOTHING persisted or enqueued; a persist failure is NOT enqueued (we never
//     ack a kill we could not make durable).
//   * `KillState::posture()` feeds the Story-3.7 coordinator's `operator_floor`,
//     so a kill becomes a posture the gate (Story 2.8) enforces — the single
//     chokepoint (Panic via the kill switch; no detector escalates to Panic).
//
// Conventions: no-throw across the boundary, no float, C++20 standard library
// only — NO OS APIs, NO `#ifdef`. The one synchronization primitive is a
// `std::mutex` guarding the handoff queue (the architecture's thread-safe
// control-plane handoff). KillState itself is single-writer and unsynchronized.
// Depends inward only on `ports` (Ok), `errors` (Auth/Internal), and the
// in-module Story-3.7 `Posture`.

#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/modes/posture.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::modes {

// The kill vocabulary, in ascending breadth. Every kill blocks new entries;
// they differ in WHICH strategies they block and whether exits stay open:
//   * Soft     — stop this account softly; block entries, allow exits.
//   * Strategy — stop one strategy (scope = strategy id); block entries, allow
//                exits. Other strategies keep running unless a broader kill.
//   * Broker   — stop everything on one broker (scope = broker id).
//   * Account  — stop everything on this account.
//   * Panic    — emergency: block entries, the normal exit gate closes, and the
//                emergency engine cancels + squares off OUT-OF-BAND.
// Renaming a returned to_string name is a breaking observability change (NFR-8).
enum class KillType { Soft, Strategy, Broker, Account, Panic };

// Stable, log/serialization-friendly name (NFR-8 observability contract).
[[nodiscard]] std::string_view to_string(KillType type) noexcept;

// A single operator kill command. `scope` narrows a Strategy/Broker/Account kill
// (strategy id / broker id / account id); it is empty for Soft and Panic. Equal
// commands are the same kill — used for the active-set dedup and queue identity.
struct KillCommand {
  KillType type = KillType::Soft;
  std::string scope;
};

[[nodiscard]] bool operator==(const KillCommand& lhs, const KillCommand& rhs) noexcept;
[[nodiscard]] bool operator!=(const KillCommand& lhs, const KillCommand& rhs) noexcept;

// The in-process flag set. Owned by the main loop, which is the SOLE writer (it
// drains the controller queue and applies each command). Reads are no-throw and
// safe to call every loop tick; they are NOT internally synchronized because the
// only writer is the single main loop.
class KillState {
 public:
  // Record a kill. Idempotent: re-applying an equal (type, scope) command is a
  // no-op (the active set holds each distinct kill once).
  void apply(const KillCommand& cmd);

  // Any active kill of type Panic.
  [[nodiscard]] bool panic_active() const noexcept;

  // Any active kill at all.
  [[nodiscard]] bool any_active() const noexcept;

  // Every kill blocks new entries (AC-1).
  [[nodiscard]] bool blocks_entries() const noexcept;

  // True under a Strategy kill whose scope == `strategy`, OR under ANY broader
  // kill (Soft/Broker/Account/Panic) — a broader kill blocks every strategy.
  [[nodiscard]] bool blocks_strategy(std::string_view strategy) const;

  // Exits stay open UNLESS panic. Under panic the emergency engine drives the
  // square-off out-of-band, so the normal gate blocks everything (AC-1/AC-2).
  [[nodiscard]] bool allows_risk_reducing_exits() const noexcept;

  // The posture this kill set imposes, for the Story-3.7 coordinator's
  // `operator_floor`: Panic if any panic; else SoftKill if any kill; else Normal.
  [[nodiscard]] Posture posture() const noexcept;

 private:
  // The active set: distinct kills, deduped by `apply`. A small vector beats a
  // map here — the active-kill count is tiny and reads are linear scans of a
  // handful of entries (no allocation per query, cache-friendly).
  std::vector<KillCommand> active_;
};

// The authenticated control plane. A CLI/control thread `submit()`s commands;
// the main loop `drain()`s them. `replay()` re-applies persisted kills on boot.
//
// THREADING CONTRACT: `submit()` is called from a SINGLE authenticated
// control-plane thread (the architecture's control handler). The `queue_mutex_`
// guards ONLY the submit-push / loop-drain handoff between that one control
// thread and the main loop; it does NOT serialize the validate/authenticate/
// persist steps. Concurrent `submit()` from multiple control threads is NOT
// supported — it would race the (un-mutexed) `persist_` seam.
class KillController {
 public:
  // `authenticate(token)` returns true iff the control command is authorized.
  // `persist(cmd)` makes an accepted kill durable BEFORE it is acked (AC-3);
  // returning an Error fails the submit closed (the kill is NOT enqueued).
  KillController(std::function<bool(std::string_view)> authenticate,
                 std::function<Result<ports::Ok>(const KillCommand&)> persist);

  // Validate -> authenticate -> persist -> enqueue (in that order; fail-closed at
  // each step). Validation comes FIRST so a malformed/mis-scoped command is
  // rejected loudly even with a bad token:
  //   * malformed command      -> Validation Error; NO persist, NO enqueue (a
  //                                Strategy/Broker/Account kill needs a scope; a
  //                                Soft/Panic kill must not carry one).
  //   * `!authenticate(token)` -> Auth Error; NO persist, NO enqueue.
  //   * persist fails           -> the seam's Error (e.g. Internal); NOT enqueued
  //                                (never ack a kill we could not make durable).
  //   * else                    -> enqueue on the thread-safe queue, return ok().
  // Called from the SINGLE control-plane thread (see the class threading
  // contract); concurrent calls from multiple threads are NOT supported.
  [[nodiscard]] Result<ports::Ok> submit(const KillCommand& cmd, std::string_view auth_token);

  // Pop the whole queued batch under the lock and return it. The main loop drains
  // at the TOP of each iteration AND immediately BEFORE dispatch, then applies
  // each command to KillState — so a kill lands within one bounded broker-call
  // timeout (AC-2). KillState stays single-writer (only the loop applies).
  [[nodiscard]] std::vector<KillCommand> drain();

  // Crash/restart recovery (AC-3): load the persisted kills and apply each to a
  // fresh `state`, so a crash between "accepted" and "acted" — and a normal
  // restart — both come back STILL KILLED. A load failure is surfaced, not
  // swallowed.
  [[nodiscard]] static Result<ports::Ok> replay(
      KillState& state, const std::function<Result<std::vector<KillCommand>>()>& load);

 private:
  std::function<bool(std::string_view)> authenticate_;
  std::function<Result<ports::Ok>(const KillCommand&)> persist_;

  mutable std::mutex queue_mutex_;  // guards the handoff queue only
  std::vector<KillCommand> queue_;  // control thread -> main loop
};

}  // namespace broker_exec::modes
