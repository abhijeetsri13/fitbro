#pragma once

// broker_exec::cli::HealthState — the single-producer / multi-reader latest-snapshot
// holder shared by the main loop and the health endpoint (Story 4.6, AC-2).
//
// The main loop is the SOLE producer: it `publish()`es a fresh `HealthSnapshot`
// each cycle. The endpoint (and any reader thread) calls `latest()` to read the
// most recent one. Access is guarded by a `std::mutex` and `latest()` returns a
// VALUE COPY — a reader can never observe a half-written snapshot nor mutate the
// stored one (the snapshot's fields are const besides).
//
// FAIL-CLOSED EMPTY STATE: a freshly constructed HealthState has published nothing,
// so `latest()` returns a fail-closed DEFAULT — session Failed, an effectively
// infinite heartbeat age, clock NOT sane, replay NOT clean. Before the loop's first
// publish, /ready and /healthz both answer 503. The engine is NEVER assumed healthy
// until it proves it (this is the readiness analog of safe-start's "verify before
// you trust").
//
// Cross-platform: C++20 standard library only (`<mutex>`, `<optional>`). No OS APIs,
// no `#ifdef`.

#include <mutex>
#include <optional>

#include "broker_exec/cli/health_snapshot.hpp"

namespace broker_exec::cli {

// The fail-closed default a never-published HealthState yields: Failed session, a
// max-int heartbeat age (always outside any sane budget), a negative in-flight
// count, clock NOT sane and replay NOT clean — so both is_live and is_ready are
// false for ANY budget. Exposed so tests/composition can assert the closed posture.
[[nodiscard]] HealthSnapshot fail_closed_default() noexcept;

// Thread-safe holder for the latest published snapshot. Copyable snapshots in, value
// copies out — the mutex makes publish/read atomic with respect to one another.
class HealthState {
 public:
  HealthState() = default;

  // Replace the stored snapshot with `snapshot` (called by the main loop each
  // cycle). Thread-safe. The previous snapshot is discarded.
  void publish(const HealthSnapshot& snapshot);

  // Return a copy of the most recently published snapshot, or `fail_closed_default()`
  // if nothing has been published yet. Thread-safe; never throws.
  [[nodiscard]] HealthSnapshot latest() const;

  // Whether the main loop has published at least once. Mainly diagnostic — readers
  // should not branch on this (the fail-closed default already makes an empty state
  // answer 503); it exists so a caller can distinguish "never published" from a
  // genuinely unhealthy reading.
  [[nodiscard]] bool has_published() const;

 private:
  mutable std::mutex mutex_;
  std::optional<HealthSnapshot> latest_;
};

}  // namespace broker_exec::cli
