#include "broker_exec/modes/posture.hpp"

#include <algorithm>

#include "broker_exec/ports/ports_common.hpp"

namespace broker_exec::modes {

namespace {

// The ascending integer severity of a posture. Centralizes the single explicit
// enum->int cast so the `std::max` compare stays /WX-clean (no implicit
// enum->int in arithmetic) and the ordering lives in exactly one place.
[[nodiscard]] int severity(Posture p) noexcept { return static_cast<int>(p); }

// The operator alert level for a degraded posture, by severity. Normal never
// reaches here (the caller only alerts when p != Normal).
[[nodiscard]] ports::AlertLevel level_for(Posture p) noexcept {
  switch (p) {
    case Posture::BlockEntries:
      return ports::AlertLevel::Warning;
    case Posture::ExitOnly:
    case Posture::SoftKill:
      return ports::AlertLevel::Error;
    case Posture::Panic:
      return ports::AlertLevel::Critical;
    case Posture::Normal:
      break;
  }
  return ports::AlertLevel::Warning;
}

}  // namespace

std::string_view to_string(Posture p) noexcept {
  switch (p) {
    case Posture::Normal:
      return "Normal";
    case Posture::BlockEntries:
      return "BlockEntries";
    case Posture::ExitOnly:
      return "ExitOnly";
    case Posture::SoftKill:
      return "SoftKill";
    case Posture::Panic:
      return "Panic";
  }
  return "Unknown";
}

std::string_view to_string(DetectorSignal s) noexcept {
  switch (s) {
    case DetectorSignal::StaleData:
      return "StaleData";
    case DetectorSignal::MuteFeed:
      return "MuteFeed";
    case DetectorSignal::Unknown:
      return "Unknown";
    case DetectorSignal::Mismatch:
      return "Mismatch";
    case DetectorSignal::SessionExpiry:
      return "SessionExpiry";
    case DetectorSignal::BrokerDown:
      return "BrokerDown";
    case DetectorSignal::ClockSkew:
      return "ClockSkew";
    case DetectorSignal::ClockStall:
      return "ClockStall";
    case DetectorSignal::ResourcePressure:
      return "ResourcePressure";
    case DetectorSignal::RiskBreach:
      return "RiskBreach";
  }
  return "Unknown";
}

Posture posture_for(DetectorSignal s) noexcept {
  switch (s) {
    // Price-sensitive / new-order block; resolve/reconcile/re-establish proceeds.
    case DetectorSignal::StaleData:
    case DetectorSignal::MuteFeed:
    case DetectorSignal::Unknown:
    case DetectorSignal::Mismatch:
    case DetectorSignal::SessionExpiry:
      return Posture::BlockEntries;
    // Degrade-to-exit-only.
    case DetectorSignal::BrokerDown:
    case DetectorSignal::ClockSkew:
    case DetectorSignal::ClockStall:
    case DetectorSignal::ResourcePressure:
      return Posture::ExitOnly;
    // Stop the strategy/account, allow exits.
    case DetectorSignal::RiskBreach:
      return Posture::SoftKill;
  }
  // Unreachable for a valid enumerator (the switch is exhaustive, -Wswitch/WX
  // guards an added one). Fail-SEVERE rather than fail-mild: an unmapped future
  // signal degrades to SoftKill, never silently under-degrades to entries-blocked.
  return Posture::SoftKill;
}

Posture PostureCoordinator::evaluate(const std::vector<DetectorSignal>& active,
                                     Posture operator_floor) const noexcept {
  Posture result = operator_floor;
  for (const DetectorSignal s : active) {
    const Posture candidate = posture_for(s);
    // Severest-wins: max by ascending integer severity (single source of order).
    if (severity(candidate) > severity(result)) {
      result = candidate;
    }
  }
  return result;
}

bool PostureCoordinator::allows_entries(Posture p) noexcept { return p == Posture::Normal; }

bool PostureCoordinator::allows_risk_reducing_exits(Posture p) noexcept {
  return p != Posture::Panic;
}

Result<ports::Ok> PostureCoordinator::require_entry_allowed(Posture p) {
  if (p == Posture::Normal) {
    return ports::ok();
  }
  // RiskRejected's baseline action is ReconcileFirst; the posture chokepoint must
  // halt the strategy, so set BlockStrategy explicitly (composes with Story 2.8).
  errors::Error err = errors::make_error(
      errors::ErrorCategory::RiskRejected,
      "posture " + std::string(to_string(p)) + " blocks new entries");
  err.action = errors::SuggestedAction::BlockStrategy;
  return fail(std::move(err));
}

DetectorSignal PostureCoordinator::from_health(health::HealthSignal h) noexcept {
  switch (h) {
    case health::HealthSignal::MuteFeed:
      return DetectorSignal::MuteFeed;
    case health::HealthSignal::ClockSkew:
      return DetectorSignal::ClockSkew;
    case health::HealthSignal::ClockStall:
      return DetectorSignal::ClockStall;
    case health::HealthSignal::DiskPressure:
    case health::HealthSignal::MemoryPressure:
    case health::HealthSignal::HandlePressure:
      return DetectorSignal::ResourcePressure;
  }
  // Unreachable for a valid enumerator; map to a resource-pressure exit-only.
  return DetectorSignal::ResourcePressure;
}

Posture PostureCoordinator::evaluate_and_alert(const std::vector<DetectorSignal>& active,
                                               Posture operator_floor,
                                               ports::AlertSink& alerts) const {
  const Posture p = evaluate(active, operator_floor);
  if (p != Posture::Normal) {
    // Redaction-safe: only the posture name, no secrets. Result swallowed so a
    // failing alert channel never derails the posture verdict.
    (void)alerts.send(level_for(p), "degradation posture: " + std::string(to_string(p)));
  }
  return p;
}

}  // namespace broker_exec::modes
