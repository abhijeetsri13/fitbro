#include "broker_exec/health/watchdog.hpp"

#include "broker_exec/ports/ports_common.hpp"

namespace broker_exec::health {

namespace {

// Append a breach to the verdict, append its redaction-safe note to `detail`, and
// fire the (best-effort, swallowed) operator alert. Centralizes the fail-closed
// "a breach is never ignored" contract so every breach path is identical.
void flag(WatchdogVerdict& verdict, ports::AlertSink& alerts, HealthSignal signal,
          ports::AlertLevel level, const std::string& note) {
  verdict.breaches.push_back(signal);
  if (!verdict.detail.empty()) {
    verdict.detail += "; ";
  }
  verdict.detail += note;
  (void)alerts.send(level, note);  // Result swallowed: alerting must not derail the verdict.
}

// A redaction-safe "usage vs limit" tag for resource messages. Plain integer
// byte/handle counts are NOT secrets.
[[nodiscard]] std::string usage_tag(std::int64_t usage, std::int64_t limit) {
  return "usage=" + std::to_string(usage) + " limit=" + std::to_string(limit);
}

}  // namespace

std::string_view to_string(HealthSignal signal) noexcept {
  switch (signal) {
    case HealthSignal::MuteFeed:
      return "MuteFeed";
    case HealthSignal::DiskPressure:
      return "DiskPressure";
    case HealthSignal::MemoryPressure:
      return "MemoryPressure";
    case HealthSignal::HandlePressure:
      return "HandlePressure";
    case HealthSignal::ClockSkew:
      return "ClockSkew";
    case HealthSignal::ClockStall:
      return "ClockStall";
  }
  return "Unknown";
}

WatchdogVerdict Watchdog::check(const WatchdogInputs& in) const {
  WatchdogVerdict verdict;

  // Fixed evaluation order, mirroring the HealthSignal enum. Each breach is
  // strictly-greater (`>`) for resources, and a 0 limit disables that dimension.

  // AC-1: connected-but-mute feed -> force reconnect-or-degrade. The verdict
  // signals degrade; the transport owns the actual reconnect attempt.
  if (in.feed_connected_but_mute) {
    flag(verdict, alerts_, HealthSignal::MuteFeed, ports::AlertLevel::Critical,
         std::string(to_string(HealthSignal::MuteFeed)) +
             " feed connected but no fresh tick past threshold");
  }

  // AC-2: resource headroom breaches alert BEFORE real exhaustion (the limit sits
  // below true capacity). 0 == no limit; usage == limit is NOT a breach.
  if (limits_.max_disk_bytes > 0 && in.usage.disk_bytes > limits_.max_disk_bytes) {
    flag(verdict, alerts_, HealthSignal::DiskPressure, ports::AlertLevel::Error,
         std::string(to_string(HealthSignal::DiskPressure)) + " " +
             usage_tag(in.usage.disk_bytes, limits_.max_disk_bytes));
  }
  if (limits_.max_mem_bytes > 0 && in.usage.mem_bytes > limits_.max_mem_bytes) {
    flag(verdict, alerts_, HealthSignal::MemoryPressure, ports::AlertLevel::Error,
         std::string(to_string(HealthSignal::MemoryPressure)) + " " +
             usage_tag(in.usage.mem_bytes, limits_.max_mem_bytes));
  }
  if (limits_.max_handles > 0 && in.usage.handle_count > limits_.max_handles) {
    flag(verdict, alerts_, HealthSignal::HandlePressure, ports::AlertLevel::Error,
         std::string(to_string(HealthSignal::HandlePressure)) + " " +
             usage_tag(in.usage.handle_count, limits_.max_handles));
  }

  // AC-3: clock skew / stall feed the SAME degrade-to-exit-only path.
  if (in.clock_skew) {
    flag(verdict, alerts_, HealthSignal::ClockSkew, ports::AlertLevel::Critical,
         std::string(to_string(HealthSignal::ClockSkew)) + " wall clock skew detected");
  }
  if (in.clock_stall) {
    flag(verdict, alerts_, HealthSignal::ClockStall, ports::AlertLevel::Critical,
         std::string(to_string(HealthSignal::ClockStall)) + " main-loop clock stall detected");
  }

  return verdict;
}

bool feed_connected_but_mute(const marketdata::MarketDataView& view, std::string_view symbol) {
  // Stale-ONLY: socket up but no fresh tick past the staleness threshold. A
  // Disconnected feed is a different (transport) signal; Live/Delayed/Unknown are
  // not mute. See the header for the full mapping rationale.
  return view.state_for(symbol) == marketdata::MarketDataState::Stale;
}

}  // namespace broker_exec::health
