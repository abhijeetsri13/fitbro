#pragma once

// broker_exec::modes — the run-mode (trading mode) vocabulary + policy (Story
// 4.5, FR-30).
//
// One SINGLE per-mode behavior table is the source of truth: the operator picks
// a mode and the runtime/gate reads its `ModePolicy` to decide what may be done
// and whether anything actually reaches the real broker. `policy_for` owns the
// table; every other helper (can_place_entry/can_place_exit/will_execute_live/
// uses_recorded_clock/require_op_allowed) DERIVES from it so the modes can never
// drift out of agreement.
//
// The three load-bearing distinctions (AC-1/2/3):
//   * `live_execution` — the order is actually SENT to the real broker. True for
//     Live/ExitOnly/Emergency only. Paper/DryRun simulate; Replay/MonitorOnly do
//     not order at all.
//   * `read_only` — no orders at all (Replay replays a recorded decision-path,
//     MonitorOnly only observes). EVERY op is blocked in a read-only mode.
//   * `replay_clock` — the Clock is bound to the recorded timeline for a
//     deterministic decision-path (Replay only, AC-2). `uses_recorded_clock`
//     flags it; the composition root injects the replay Clock.
//
// Two contracts worth stating loudly:
//   * DRY-RUN VALIDATES BUT NEVER EXECUTES (AC-3): `require_op_allowed` lets the
//     op through (the validation gate runs) yet `will_execute_live(DryRun)` is
//     false, so the runtime suppresses the actual send. "Allowed to validate" is
//     NOT "will be sent".
//   * EMERGENCY = ONLY CANCEL / SQUARE-OFF (AC-3): Entry and a generic Exit are
//     both blocked; only risk-removing Cancel/SquareOff get through.
//
// Conventions: no-throw (Result<Ok>), no float, integer enums only. Cross-
// platform: C++20 standard library only — NO OS APIs, NO `#ifdef`. Depends
// inward only on `ports` (Ok) and `errors` (NotSupported/RiskRejected). An
// unmapped future mode FAILS CLOSED to the most restrictive (read-only) policy.

#include <string_view>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::modes {

// The selectable run-mode vocabulary (FR-30). The underlying values carry no
// ordering meaning (unlike Posture); modes are distinguished by their policy.
// Renaming a returned to_string name is a breaking observability change (NFR-8).
enum class TradingMode {
  Live,         // real broker, entries + exits
  Paper,        // simulated fills, no real send
  DryRun,       // validates every op but never executes (AC-3)
  Replay,       // read-only replay of a recorded decision-path; replay Clock (AC-2)
  MonitorOnly,  // read-only; observe, never order
  ExitOnly,     // risk-reducing exits only, on the real broker
  Emergency     // real broker, ONLY cancel / square-off (AC-3)
};

// The order operation a gate is asked to authorize for a mode.
enum class OrderOp {
  Entry,     // open / add to a position
  Exit,      // a generic risk-reducing exit
  Cancel,    // cancel an open order
  SquareOff  // flatten a position (emergency exit)
};

// The per-mode behavior table — the single source of truth read by the runtime.
//   allows_entry    — new entries permitted in this mode.
//   allows_exit     — risk-reducing exits permitted in this mode.
//   live_execution  — orders are SENT to the REAL broker (vs simulated/none).
//   read_only       — no orders at all (replay / monitor).
//   replay_clock    — the Clock is bound to the recorded timeline (AC-2).
struct ModePolicy {
  bool allows_entry = false;
  bool allows_exit = false;
  bool live_execution = false;
  bool read_only = false;
  bool replay_clock = false;
};

// Stable, log/serialization-friendly names (NFR-8 observability contract).
[[nodiscard]] std::string_view to_string(TradingMode m) noexcept;
[[nodiscard]] std::string_view to_string(OrderOp op) noexcept;

// The behavior table for a mode (the single source of truth). An unmapped future
// mode fails CLOSED to the most restrictive policy (all-false, read-only) so a
// missing mapping can never silently permit a live send.
[[nodiscard]] ModePolicy policy_for(TradingMode m) noexcept;

// Entry / exit gates, derived from the policy. ADVISORY/coarse: can_place_exit is
// true for any mode that permits an exit-class op, so it is true for Emergency
// (which permits cancel/square-off). It is NOT the per-op chokepoint — a generic
// Exit in Emergency is still blocked. Always gate an actual op through
// require_op_allowed(), not these coarse flags.
[[nodiscard]] bool can_place_entry(TradingMode m) noexcept;
[[nodiscard]] bool can_place_exit(TradingMode m) noexcept;

// True ONLY for modes that hit the real broker (Live/ExitOnly/Emergency).
// Paper/DryRun/Replay/MonitorOnly are non-live. NB (AC-3): DryRun allows an op to
// be VALIDATED (see require_op_allowed) yet this stays false — the runtime runs
// the gate but suppresses the actual send.
[[nodiscard]] bool will_execute_live(TradingMode m) noexcept;

// True ONLY for Replay: the Clock is bound to the recorded timeline for a
// deterministic decision-path (AC-2). The composition root injects the replay
// Clock when this is true; this only flags it.
[[nodiscard]] bool uses_recorded_clock(TradingMode m) noexcept;

// Authorize an order operation for a mode (the gate's mode chokepoint):
//   * read-only modes (Replay, MonitorOnly): EVERY op blocked -> NotSupported.
//   * Emergency: ONLY Cancel and SquareOff allowed; Entry AND a generic Exit are
//     blocked -> RiskRejected (AC-3, only cancel/square-off).
//   * ExitOnly: Entry blocked (NotSupported); Exit/Cancel/SquareOff allowed.
//   * Live/Paper/DryRun: all four ops allowed (ok()). For DryRun this means the
//     op is VALIDATED; will_execute_live(DryRun)==false suppresses the send (AC-3
//     validates-but-never-executes) — "allowed to validate" is NOT "will send".
// A blocked op returns a typed Error naming the mode + op (redaction-safe, no
// secrets). ok() otherwise.
[[nodiscard]] Result<ports::Ok> require_op_allowed(TradingMode m, OrderOp op);

}  // namespace broker_exec::modes
