#include "broker_exec/session/safe_start.hpp"

#include <string>
#include <utility>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/session/kite_session_establisher.hpp"

namespace broker_exec::session {

namespace {

using errors::Error;
using errors::ErrorCategory;
using errors::make_error;
using errors::SuggestedAction;

// Every safe-start Error message NAMES the failing check with a stable prefix,
// mirroring the validation gate's naming (AC-1/AC-2 "block loudly").
[[nodiscard]] std::string named(const char* check, const std::string& detail) {
  return std::string("safe-start: ") + check + " check failed: " + detail;
}

// Fail-closed Error for an UNSET (empty) check: a safety gate must never pass a
// world it cannot verify. Internal because an unconfigured gate is a deployment/
// wiring fault, but the runtime must HALT (not just alert), so the action is set
// explicitly to BlockStrategy (Internal's default is RaiseAlert).
[[nodiscard]] Error not_configured_error(const char* check) {
  Error e = make_error(ErrorCategory::Internal, named(check, "not configured"));
  e.action = SuggestedAction::BlockStrategy;
  return e;
}

// Wrap a propagated inner Error (from an injected check) so the message NAMES
// the check, while PRESERVING its category, action and broker_code (e.g. a
// DataStale from require_fresh, a SessionExpired from the session check stay
// intact for the runtime's deterministic switch). Copies validation_gate.cpp.
[[nodiscard]] Error wrap_error(const char* check, Error inner) {
  Error wrapped = std::move(inner);
  wrapped.message = named(check, wrapped.message);
  return wrapped;
}

// Run one required check fail-closed: empty -> not-configured block; failing ->
// the inner Error wrapped/named (category/action preserved); passing -> ok()
// (continue to the next check).
[[nodiscard]] Result<ports::Ok> run_check(const char* name, const SafeCheck& check) {
  if (!check) {
    return fail(not_configured_error(name));
  }
  if (auto r = check(); !r) {
    return fail(wrap_error(name, std::move(r.error())));
  }
  return ports::ok();
}

}  // namespace

Result<ports::Ok> SafeStartGate::verify(const SafeStartContext& ctx) const {
  // Fixed order: foundational/cheap first, reconciliation last. The first
  // failing/empty check short-circuits and returns its named Error.
  if (auto r = run_check("config", ctx.config_check); !r) {
    return r;
  }
  if (auto r = run_check("crypto-keys", ctx.crypto_keys_check); !r) {
    return r;
  }
  if (auto r = run_check("clock", ctx.clock_check); !r) {
    return r;
  }
  if (auto r = run_check("session", ctx.session_check); !r) {
    return r;
  }
  if (auto r = run_check("egress-IP", ctx.egress_ip_check); !r) {
    return r;
  }
  if (auto r = run_check("instrument-master", ctx.instrument_master_check); !r) {
    return r;
  }
  if (auto r = run_check("calendar", ctx.calendar_check); !r) {
    return r;
  }
  if (auto r = run_check("reconciliation", ctx.reconciliation_check); !r) {
    return r;
  }
  return ports::ok();
}

Result<ports::Ok> session_state_to_result(SessionState state) {
  switch (state) {
    case SessionState::Healthy:
      return ports::ok();
    case SessionState::NeedsReauth:
    case SessionState::Failed:
      return fail(KiteSessionEstablisher::needs_reauth_error());
  }
  // Unreachable for the fixed enum; fail closed if a new state is ever added.
  return fail(KiteSessionEstablisher::needs_reauth_error());
}

}  // namespace broker_exec::session
