#pragma once

// broker_exec::health — liveness and process self-health watchdogs (Story 3.6,
// FR-20).
//
// The bot must DEGRADE before it silently fails. This module DETECTS two classes
// of liveness/health breach and signals a single, blunt verdict —
// "degrade-to-exit-only" — that the posture coordinator (Story 3.7) later maps to
// a concrete posture and the gate (2.8) enforces. The watchdog itself only
// detects + alerts; it owns no reconnect, no kill, no posture mapping.
//
//   * connected-but-MUTE feed — the socket is up but no fresh tick has arrived
//     past the staleness threshold (AC-1). Reuses the Story-3.5
//     `marketdata::MarketDataView` classification via the free
//     `feed_connected_but_mute(...)` helper below.
//   * resource exhaustion — disk (WAL/audit growth), memory, and OS-handle counts
//     crossing HEADROOM limits set BELOW true capacity, so degradation + an alert
//     happen BEFORE the WAL/audit/logging substrate is actually corrupted (AC-2).
//   * clock skew / stall — an injected detector signal (the real one is the
//     Story-1.3 SkewStallDetector) feeding the SAME degrade-to-exit-only path
//     (AC-3).
//
// Fail-closed: ANY breach => `degrade_to_exit_only()` true; a breach is never
// ignored. Conventions: integer bytes/handles (no double/float), no-throw, the
// AlertSink Result is swallowed (alerting is best-effort and must not derail the
// safety verdict). Cross-platform: C++20 standard library only — NO OS APIs, NO
// `#ifdef`. This module never reads `/proc`, GlobalMemoryStatusEx, etc.; the host
// gathers the raw `ResourceUsage` behind the platform seam and passes it in.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "broker_exec/marketdata/market_data.hpp"
#include "broker_exec/ports/alert_sink.hpp"

namespace broker_exec::health {

// A single liveness/health breach class. Stable, log-friendly names (see
// to_string). The evaluation order in `Watchdog::check` mirrors this order.
enum class HealthSignal {
  MuteFeed,        // socket up but no fresh tick past threshold (AC-1)
  DiskPressure,    // disk usage crossed the headroom limit (AC-2)
  MemoryPressure,  // memory usage crossed the headroom limit (AC-2)
  HandlePressure,  // OS-handle count crossed the headroom limit (AC-2)
  ClockSkew,       // wall clock jumped vs monotonic (AC-3)
  ClockStall       // the main loop / clock stopped advancing (AC-3)
};

// Stable, log/serialization-friendly signal names (NFR-8 observability contract).
[[nodiscard]] std::string_view to_string(HealthSignal signal) noexcept;

// A snapshot of process resource consumption. Integer counts only (no float):
// bytes for disk/memory, a plain count for OS handles. The host gathers these
// behind the platform seam and passes them in — this module reads no OS API.
struct ResourceUsage {
  std::int64_t disk_bytes = 0;
  std::int64_t mem_bytes = 0;
  int handle_count = 0;
};

// HEADROOM limits, deliberately set BELOW true capacity so a breach alerts BEFORE
// the underlying resource is exhausted and WAL/audit/logging is corrupted (AC-2).
// A limit of 0 means "no limit" — that dimension is not checked.
struct ResourceLimits {
  std::int64_t max_disk_bytes = 0;
  std::int64_t max_mem_bytes = 0;
  int max_handles = 0;
};

// Everything the watchdog evaluates in one tick. The clock-skew/stall flags and
// the mute-feed flag are computed by injected detectors (the host calls
// `feed_connected_but_mute(...)` for the feed flag) and handed in here.
struct WatchdogInputs {
  ResourceUsage usage;
  bool feed_connected_but_mute = false;
  bool clock_skew = false;
  bool clock_stall = false;
};

// The blunt verdict: the list of breaches seen this tick plus a redaction-safe
// human/audit detail string. `degrade_to_exit_only()` is true for ANY breach —
// fail-closed; the watchdog detects + signals, Story 3.7 maps the posture.
struct WatchdogVerdict {
  std::vector<HealthSignal> breaches;
  std::string detail;

  [[nodiscard]] bool healthy() const { return breaches.empty(); }
  [[nodiscard]] bool degrade_to_exit_only() const { return !breaches.empty(); }
};

// The process self-health watchdog. Stateless apart from its limits + alert sink;
// `check` is const and may be called every loop tick. NOT thread-safe — it lives
// on the single main loop. The injected `AlertSink` MUST outlive the watchdog.
class Watchdog {
 public:
  Watchdog(ResourceLimits limits, ports::AlertSink& alerts)
      : limits_(normalize(limits)), alerts_(alerts) {}

  // Evaluate `in` in a FIXED order (the HealthSignal order): MuteFeed, then disk
  // / memory / handle pressure, then ClockSkew, then ClockStall. For each breach,
  // a HealthSignal is appended to the verdict, a redaction-safe note is appended
  // to `detail`, and one alert is sent — `(void)alerts_.send(...)`, the Result
  // swallowed so a failing alert channel never derails the safety verdict. Alert
  // levels: Critical for MuteFeed / ClockSkew / ClockStall (they put the bot into
  // exit-only immediately), Error for the resource-headroom breaches (degrade +
  // alert before corruption). Resource checks are STRICTLY greater (`>`): usage
  // exactly equal to the limit is NOT a breach. A 0 limit disables that check.
  // The verdict is healthy iff no breach fired. No throw, no float.
  [[nodiscard]] WatchdogVerdict check(const WatchdogInputs& in) const;

 private:
  // 0 is the documented "no limit" sentinel. A NEGATIVE limit is a misconfig that
  // would otherwise silently disable that dimension (fail-open) — normalize it to
  // 0 so only an explicit positive value ever arms a check.
  [[nodiscard]] static ResourceLimits normalize(ResourceLimits l) noexcept {
    if (l.max_disk_bytes < 0) {
      l.max_disk_bytes = 0;
    }
    if (l.max_mem_bytes < 0) {
      l.max_mem_bytes = 0;
    }
    if (l.max_handles < 0) {
      l.max_handles = 0;
    }
    return l;
  }

  ResourceLimits limits_;
  ports::AlertSink& alerts_;
};

// Mute-feed helper (AC-1), reusing the Story-3.5 tradability classification.
// Returns true iff the feed is CONNECTED-BUT-MUTE: `view.state_for(symbol)` is
// `MarketDataState::Stale` — the socket is up but no fresh tick has arrived past
// the staleness threshold. The caller passes the result as
// `WatchdogInputs::feed_connected_but_mute`.
//
// State mapping (deliberate, documented):
//   * Stale        -> true  (connected but mute — the watchdog signal)
//   * Disconnected -> false (the socket is DOWN — a DIFFERENT signal owned by the
//                            transport reconnect path, not a mute feed)
//   * Live         -> false (fresh ticks — healthy)
//   * Delayed      -> false (ticks ARE arriving, merely lagging the exchange; the
//                            feed is not mute. Per the story this is Stale-ONLY;
//                            Delayed is intentionally excluded)
//   * Unknown      -> false (no tick ever seen — startup, not a mute regression)
[[nodiscard]] bool feed_connected_but_mute(const marketdata::MarketDataView& view,
                                           std::string_view symbol);

}  // namespace broker_exec::health
