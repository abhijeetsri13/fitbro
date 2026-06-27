#pragma once

// broker_exec::session::SafeStartGate — the fail-closed cold-boot gate (Story
// 2.13, FR-22). Trading is blocked until the WHOLE world is verified fresh and
// authenticated, so a restart can never trade into a stale/unauthenticated state.
//
// The gate is decoupled from every concrete module: it composes the world via
// INJECTED `std::function<Result<Ok>()>` checks (one per concern). The
// composition root binds each to the real module (session::validate, the
// reconciler, clock skew/stall, config, refdata::InstrumentMaster::require_fresh,
// TradingCalendar::require_fresh, egress-IP allow-list, crypto-key presence). The
// gate itself depends INWARD only on `ports` (Ok) and `errors`, so it stays
// uniformly testable with simple lambdas.
//
// FAIL-CLOSED, ALL-REQUIRED semantics (unlike the validation gate): every check
// is mandatory. An UNSET (empty std::function) check is a HARD FAILURE — a safety
// gate must never pass a world it cannot verify. The checks run in a FIXED,
// documented order (cheap/foundational first; reconciliation last); the FIRST
// failing/empty check returns a NAMED Error and short-circuits the rest. On a
// wrapped inner Error the category/action/broker_code are PRESERVED (e.g. a
// DataStale from require_fresh, a SessionExpired from the session check) while the
// check name is prepended to the message — mirroring the validation gate's naming.
//
// NO-THROW POLICY: every method returns `Result<Ok>`; nothing throws.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <functional>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/session/session_state.hpp"

namespace broker_exec::session {

// One world-verification check: success-or-Error, no value. The composition root
// binds each to a real module; tests bind simple lambdas.
using SafeCheck = std::function<Result<ports::Ok>()>;

// The eight cold-boot checks, all REQUIRED. An empty std::function is treated as
// an unconfigured (hence unverifiable) world and BLOCKS the start.
struct SafeStartContext {
  SafeCheck config_check;             // config integrity / parse
  SafeCheck crypto_keys_check;        // crypto-key presence (SE-5)
  SafeCheck clock_check;              // clock sanity (skew / stall)
  SafeCheck session_check;            // broker session establishment / liveness
  SafeCheck egress_ip_check;          // egress-IP allow-list match
  SafeCheck instrument_master_check;  // fresh instrument master (refdata)
  SafeCheck calendar_check;           // fresh trading calendar (refdata)
  SafeCheck reconciliation_check;     // reconciliation completeness (Epic 3)
};

// The single cold-boot authority. There is NO partial/skip variant: verify()
// flips "may trade" true only when the entire world passes.
class SafeStartGate {
 public:
  // Run all eight checks in the FIXED order: config -> crypto-keys -> clock ->
  // session -> egress-IP -> instrument-master -> calendar -> reconciliation. An
  // empty check -> a fail-closed Internal/BlockStrategy "not configured" Error
  // naming it. A check that returns an Error -> that Error WRAPPED (category/
  // action/broker_code preserved, name prepended). The first failing/empty check
  // returns; all pass -> ok() (trading allowed).
  [[nodiscard]] Result<ports::Ok> verify(const SafeStartContext& ctx) const;
};

// Convenience for wiring the session check: turn a SessionState into a Result.
// Healthy -> ok(); NeedsReauth or Failed -> KiteSessionEstablisher::
// needs_reauth_error() (SessionExpired + ReEstablishSession).
//
// Composition-root wiring (recommended): preserve a transport/5xx error from
// validate() rather than collapsing it to SessionExpired — only map the STATE:
//   session_check = [&]() -> Result<ports::Ok> {
//     auto s = establisher.validate();
//     if (!s) return fail(std::move(s.error()));   // keep Network/ReconcileFirst etc.
//     return session_state_to_result(s.value());   // Healthy/NeedsReauth/Failed
//   };
// (`validate().value_or(SessionState::Failed)` also fails closed, but it
// mislabels a transient transport failure as a dead session.)
[[nodiscard]] Result<ports::Ok> session_state_to_result(SessionState state);

}  // namespace broker_exec::session
