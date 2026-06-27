#pragma once

// broker_exec::modes — the degradation-posture coordinator (Story 3.7, FR-26).
//
// One SINGLE authority maps each detector failure into a coherent operating
// posture, so degradation is never contradictory. Detectors only FEED the
// coordinator `DetectorSignal`s; they never each decide a posture (AC-2). The
// coordinator resolves a possibly-contradictory mix of active signals into the
// SEVEREST posture (a total order, `std::max` over ascending enum values), and
// the gate (Story 2.8) enforces it as the single chokepoint (AC-1/AC-3):
//   * `allows_entries` / `require_entry_allowed` — every degraded posture blocks
//     new entries loudly (no silent failure, no duplicate orders).
//   * `allows_risk_reducing_exits` — exits stay open except under Panic, where
//     the emergency engine (Story 3.8) drives the square-off out-of-band.
//
// Panic is reached ONLY via the operator kill switch (Story 3.8), folded in as
// the `operator_floor`; no detector escalates to Panic on its own.
//
// Conventions: no-throw, no float, integer enum severities only. The AlertSink
// Result is swallowed (alerting is best-effort and must not derail the verdict).
// Cross-platform: C++20 standard library only — NO OS APIs, NO `#ifdef`. Depends
// inward only on `ports` (AlertSink), `errors` (RiskRejected), and `health`
// (HealthSignal, for `from_health`).

#include <string>
#include <string_view>
#include <vector>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/health/watchdog.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::modes {

// The operating posture vocabulary, a TOTAL ORDER by severity. The underlying
// values are ASCENDING (Normal < BlockEntries < ExitOnly < SoftKill < Panic) so
// `std::max` over the severities yields the severest posture (see severity()).
// Renaming a returned to_string name is a breaking observability change (NFR-8).
enum class Posture {
  Normal = 0,        // fully operational; entries and exits allowed
  BlockEntries = 1,  // block new/price-sensitive entries; resolve/reconcile proceeds
  ExitOnly = 2,      // degrade-to-exit-only; no new entries, exits allowed
  SoftKill = 3,      // stop the strategy/account; exits allowed
  Panic = 4          // operator kill switch; emergency square-off out-of-band
};

// The detector vocabulary the coordinator consumes. Detectors map their own
// breach into one of these and FEED it in; they do not pick a posture (AC-2).
enum class DetectorSignal {
  StaleData,         // a freshness gate failed
  MuteFeed,          // socket up but no fresh tick past threshold
  Unknown,           // an order/operation is in the UNKNOWN state, reconcile
  Mismatch,          // a reconciliation mismatch vs broker truth
  SessionExpiry,     // session token expired; re-establish
  BrokerDown,        // the broker/transport is unreachable
  ClockSkew,         // wall clock jumped vs monotonic
  ClockStall,        // the main loop / clock stopped advancing
  ResourcePressure,  // disk/memory/handle headroom breached
  RiskBreach         // a risk limit was breached
};

// Stable, log/serialization-friendly names (NFR-8 observability contract).
[[nodiscard]] std::string_view to_string(Posture p) noexcept;
[[nodiscard]] std::string_view to_string(DetectorSignal s) noexcept;

// The MINIMUM posture a single detector signal requires (documented mapping):
//   StaleData/MuteFeed/Unknown/Mismatch/SessionExpiry -> BlockEntries
//     (price-sensitive/new-order block; resolve/reconcile/re-establish proceeds)
//   BrokerDown/ClockSkew/ClockStall/ResourcePressure  -> ExitOnly
//     (degrade-to-exit-only)
//   RiskBreach                                         -> SoftKill
//     (stop the strategy/account, allow exits)
// No detector maps to Panic — Panic is operator-only (Story 3.8).
[[nodiscard]] Posture posture_for(DetectorSignal s) noexcept;

// The single posture authority (AC-2). Stateless; `evaluate` is const and may be
// called every loop tick. NOT thread-safe by itself — it lives on the single
// main loop.
class PostureCoordinator {
 public:
  // Resolve the active detector signals plus the operator floor into the SEVEREST
  // (max) posture (AC-1) — a contradictory mix resolves to the most severe. No
  // active signals and a Normal floor yields Normal.
  [[nodiscard]] Posture evaluate(const std::vector<DetectorSignal>& active,
                                 Posture operator_floor = Posture::Normal) const noexcept;

  // Entry gate: only the fully-operational Normal posture permits new entries;
  // every degraded posture blocks them (AC-1, no silent failure).
  [[nodiscard]] static bool allows_entries(Posture p) noexcept;

  // Risk-reducing exit gate: open for Normal/BlockEntries/ExitOnly/SoftKill;
  // CLOSED for Panic — under Panic the emergency engine (Story 3.8) drives the
  // square-off out-of-band, so the normal gate blocks everything.
  [[nodiscard]] static bool allows_risk_reducing_exits(Posture p) noexcept;

  // The gate's posture chokepoint: ok() iff Normal; otherwise a typed
  // `RiskRejected` Error (action BlockStrategy) naming the posture. Composes with
  // the Story-2.8 validation gate.
  [[nodiscard]] static Result<ports::Ok> require_entry_allowed(Posture p);

  // Map a watchdog liveness/health signal (Story 3.6) into the coordinator's
  // vocabulary so the watchdog can FEED the coordinator without deciding a
  // posture: MuteFeed->MuteFeed; ClockSkew->ClockSkew; ClockStall->ClockStall;
  // Disk/Memory/Handle Pressure -> ResourcePressure.
  [[nodiscard]] static DetectorSignal from_health(health::HealthSignal h) noexcept;

  // Compute the posture and, when it is not Normal, send exactly one
  // redaction-safe operator alert (level by severity: BlockEntries=Warning,
  // ExitOnly/SoftKill=Error, Panic=Critical). The AlertSink Result is swallowed;
  // no throw. Returns the same posture `evaluate` would.
  [[nodiscard]] Posture evaluate_and_alert(const std::vector<DetectorSignal>& active,
                                           Posture operator_floor,
                                           ports::AlertSink& alerts) const;
};

}  // namespace broker_exec::modes
