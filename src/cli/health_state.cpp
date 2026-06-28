#include "broker_exec/cli/health_state.hpp"

#include <cstdint>
#include <limits>
#include <mutex>

#include "broker_exec/session/session_state.hpp"

namespace broker_exec::cli {

HealthSnapshot fail_closed_default() noexcept {
  // The closed posture: nothing is trusted until the loop proves it. A Failed
  // session, a max-int heartbeat age (always outside any sane budget), a negative
  // in-flight count, and both sanity gates false — so is_live and is_ready are
  // false for ANY budget. /healthz and /ready both answer 503 before first publish.
  return HealthSnapshot(session::SessionState::Failed,
                        std::numeric_limits<std::int64_t>::max(),
                        std::numeric_limits<std::int64_t>::max(),
                        /*in_flight_count=*/-1, /*clock_sane=*/false, /*replay_clean=*/false);
}

void HealthState::publish(const HealthSnapshot& snapshot) {
  const std::lock_guard<std::mutex> lock(mutex_);
  // emplace (not assignment): HealthSnapshot's const members make it non-assignable,
  // so we destroy-and-reconstruct in place. The previous snapshot is discarded.
  latest_.emplace(snapshot);
}

HealthSnapshot HealthState::latest() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (latest_.has_value()) {
    return *latest_;  // value copy — the reader holds a frozen snapshot
  }
  return fail_closed_default();  // never published -> fail closed (503)
}

bool HealthState::has_published() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return latest_.has_value();
}

}  // namespace broker_exec::cli
