#pragma once

// broker_exec::cli::HealthSnapshot — the immutable, integer-only health value the
// main loop publishes and the localhost health endpoint serves (Story 4.6, FR-36).
//
// A snapshot is a frozen reading of the engine's liveness/readiness inputs at one
// instant: the normalized session state, the age of the last heartbeat and last
// tick (milliseconds, NEVER float), the in-flight order count, and two boolean
// sanity gates (clock sane, replay clean). It owns NO logic and reaches NO module;
// the main loop fills it and `HealthState` distributes copies. Readers can never
// mutate a published snapshot — every member is `const`, so a reader holds a frozen
// copy (mirrors the immutable-value posture of ledger's EodReport/PositionHeartbeat).
//
// LIVENESS vs READINESS (the two endpoint questions, AC-2):
//   * is_live   — "is the process responding?" Weak: heartbeat within the budget
//                 AND the clock is sane (a stalled clock is NOT live).
//   * is_ready  — "is the engine fit to trade?" Strong: live PLUS replay clean,
//                 session Healthy and a non-negative in-flight count. Readiness is
//                 strictly stronger than liveness — a live-but-unhealthy engine
//                 answers /healthz 200 yet /ready 503.
// The liveness budget (max acceptable heartbeat age, ms) is a CALLER parameter, not
// baked in here — the composition root owns the SLO.
//
// OUTBOUND PAYLOAD: `to_json` renders the snapshot and runs the rendered string
// through `domain::scrub` before returning — in-memory fields are raw, but every
// payload that leaves the process scrubs itself (lesson from 4-2; SEC-3/SEC-6).
//
// SessionState vocabulary is `{Healthy, NeedsReauth, Failed}` — there is NO "Active".
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <cstdint>
#include <string>

#include "broker_exec/session/session_state.hpp"

namespace broker_exec::cli {

// An immutable reading of the engine's health inputs at one instant. Construct it
// all-at-once (the all-arg constructor); the const members make a published
// snapshot un-mutatable by any reader — readers receive frozen copies. Integers
// only (millisecond ages, integer counts): NO float anywhere on the health path.
struct HealthSnapshot {
  // All-arg construction — the only way to build a snapshot, so every field is set
  // explicitly (no half-filled reading). noexcept: trivial member copies only.
  HealthSnapshot(session::SessionState session_state, std::int64_t heartbeat_age_ms,
                 std::int64_t tick_age_ms, int in_flight_count, bool clock_sane,
                 bool replay_clean) noexcept;

  // The normalized session state (Healthy / NeedsReauth / Failed).
  const session::SessionState session_state;
  // Age of the last heartbeat, milliseconds. Compared against the caller's budget.
  const std::int64_t heartbeat_age_ms;
  // Age of the last market-data tick, milliseconds (observability detail).
  const std::int64_t tick_age_ms;
  // Orders currently in flight (sent, not yet terminal). A negative value is
  // treated as not-ready (a malformed reading must never read as fit-to-trade).
  const int in_flight_count;
  // Wall/monotonic clock sanity gate — a stalled/insane clock is NOT live.
  const bool clock_sane;
  // The intent-log replay verified clean at start (no torn/duplicated tail).
  const bool replay_clean;
};

// Render the snapshot as a compact JSON object and return it SCRUBBED. This is an
// outbound payload (it leaves the process over the health endpoint), so the
// rendered string is run through `domain::scrub` before return — no token-shaped
// substring can survive into the served body. Never throws (JSON errors are
// replaced, not raised); only a std::bad_alloc could escape, as with any string op.
[[nodiscard]] std::string to_json(const HealthSnapshot& snapshot);

// Liveness (process responding): the heartbeat is within `live_budget_ms` AND the
// clock is sane. Deliberately weaker than readiness. A negative budget or a
// negative heartbeat age reads as NOT live (fail closed).
[[nodiscard]] bool is_live(const HealthSnapshot& snapshot, std::int64_t live_budget_ms) noexcept;

// Readiness (fit to trade): is_live AND fully healthy — `clock_sane && replay_clean
// && session_state == SessionState::Healthy && in_flight_count >= 0` with the
// heartbeat within `live_budget_ms`. Strictly stronger than is_live.
[[nodiscard]] bool is_ready(const HealthSnapshot& snapshot, std::int64_t live_budget_ms) noexcept;

}  // namespace broker_exec::cli
