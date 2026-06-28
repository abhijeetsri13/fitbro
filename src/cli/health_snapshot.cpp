#include "broker_exec/cli/health_snapshot.hpp"

#include <string>

#include <nlohmann/json.hpp>

#include "broker_exec/domain/redaction.hpp"

namespace broker_exec::cli {

namespace {

using json = nlohmann::json;

// Compact, never-throwing JSON render. error_handler_t::replace swaps any invalid
// UTF-8 for the replacement char instead of throwing, so a malformed in-memory
// string can never raise across our no-throw boundary (mirrors ledger's
// dump_compact). Only std::bad_alloc could escape, as with any std::string op.
[[nodiscard]] std::string dump_compact(const json& value) {
  return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace

HealthSnapshot::HealthSnapshot(session::SessionState session_state, std::int64_t heartbeat_age_ms,
                               std::int64_t tick_age_ms, int in_flight_count, bool clock_sane,
                               bool replay_clean) noexcept
    : session_state(session_state),
      heartbeat_age_ms(heartbeat_age_ms),
      tick_age_ms(tick_age_ms),
      in_flight_count(in_flight_count),
      clock_sane(clock_sane),
      replay_clean(replay_clean) {}

std::string to_json(const HealthSnapshot& snapshot) {
  json out = json::object();
  // Stable, observability-contract field names. Integers only (millisecond ages,
  // integer counts) — NO float on the health path.
  out["session_state"] = std::string(session::to_string(snapshot.session_state));
  out["heartbeat_age_ms"] = snapshot.heartbeat_age_ms;
  out["tick_age_ms"] = snapshot.tick_age_ms;
  out["in_flight_count"] = snapshot.in_flight_count;
  out["clock_sane"] = snapshot.clock_sane;
  out["replay_clean"] = snapshot.replay_clean;

  // OUTBOUND PAYLOAD: the rendered body leaves the process over the health
  // endpoint, so scrub it. In-memory fields are raw; every payload scrubs itself
  // (lesson from 4-2). A session detail carrying a token-shaped string can never
  // survive into the served body.
  return domain::scrub(dump_compact(out));
}

bool is_live(const HealthSnapshot& snapshot, std::int64_t live_budget_ms) noexcept {
  // Fail closed on nonsense inputs: a negative budget or a negative heartbeat age
  // is not a live reading. Otherwise: heartbeat within budget AND clock sane (a
  // stalled clock is NOT live).
  if (live_budget_ms < 0 || snapshot.heartbeat_age_ms < 0) {
    return false;
  }
  return snapshot.heartbeat_age_ms <= live_budget_ms && snapshot.clock_sane;
}

bool is_ready(const HealthSnapshot& snapshot, std::int64_t live_budget_ms) noexcept {
  // Readiness is strictly stronger than liveness: live PLUS fully healthy.
  return is_live(snapshot, live_budget_ms) && snapshot.replay_clean &&
         snapshot.session_state == session::SessionState::Healthy &&
         snapshot.in_flight_count >= 0;
}

}  // namespace broker_exec::cli
