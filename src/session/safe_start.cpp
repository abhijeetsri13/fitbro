#include "broker_exec/session/safe_start.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/redaction.hpp"
#include "broker_exec/domain/types.hpp"
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
  // Immediately after config, and before anything that touches a key, a clock or
  // the broker: the strategy names ARE configuration, and a name that makes every
  // client_ref unloggable must be fixed before a session exists to log about.
  if (auto r = run_check("strategy-names", ctx.strategy_name_check); !r) {
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
  // Before reconciliation: a projection still holding pre-IMP-11 stops is a world
  // whose ORDER IDENTITY we cannot trust, so there is no point reconciling it.
  if (auto r = run_check("legacy-stops", ctx.legacy_stop_check); !r) {
    return r;
  }
  if (auto r = run_check("reconciliation", ctx.reconciliation_check); !r) {
    return r;
  }
  return ports::ok();
}

bool is_legacy_trigger_less_stop(const domain::Order& order) noexcept {
  const bool is_stop = order.intent.order_type == domain::OrderType::StopLoss ||
                       order.intent.order_type == domain::OrderType::StopLossMarket;
  if (!is_stop || order.intent.trigger_price.has_value()) {
    return false;
  }
  // Only a WORKING order can still fire. A terminal row is history: it cannot be
  // re-placed, so it must not block a start (otherwise the gate would be
  // permanently stuck on a database that merely REMEMBERS an old stop).
  switch (order.state) {
    case domain::OrderState::Filled:
    case domain::OrderState::Rejected:
    case domain::OrderState::Cancelled:
      return false;
    default:
      return true;
  }
}

Result<ports::Ok> require_no_legacy_stops(const std::vector<domain::Order>& orders) {
  std::size_t count = 0;
  std::string first_ref;
  for (const domain::Order& order : orders) {
    if (!is_legacy_trigger_less_stop(order)) {
      continue;
    }
    ++count;
    if (first_ref.empty()) {
      first_ref = order.intent.client_ref;
    }
  }
  if (count == 0) {
    return ports::ok();
  }

  // Redaction-safe: a count and a client_ref (an id the operator needs in order to
  // act), never a price or a quantity.
  Error e = make_error(
      ErrorCategory::Validation,
      "projection holds " + std::to_string(count) +
          " working stop order(s) with no trigger price — these were placed by a "
          "pre-IMP-11 binary, which stored the stop level in `price`. Their signal "
          "signatures have changed, so restart dedupe cannot recognize them and they "
          "could be placed a SECOND time. Flatten or cancel every outstanding stop "
          "before deploying (see docs/upgrade-imp-11-stops.md). First: " +
          (first_ref.empty() ? std::string("<no client_ref>") : first_ref));
  // Validation's default action is DoNotRetry; this must HALT the runtime, and the
  // fix is a human one, so state BlockStrategy explicitly.
  e.action = SuggestedAction::BlockStrategy;
  return fail(std::move(e));
}

Result<ports::Ok> require_valid_strategy_names(const std::vector<std::string>& names) {
  std::size_t count = 0;
  std::string first_reason;
  for (const std::string& name : names) {
    std::string reason = domain::explain_invalid_strategy_name(name);
    if (reason.empty()) {
      continue;
    }
    ++count;
    if (first_reason.empty()) {
      first_reason = std::move(reason);
    }
  }
  if (count == 0) {
    return ports::ok();
  }

  // Redaction-safe by construction: explain_invalid_strategy_name sanitises and
  // truncates the echoed name, so nothing here can carry an arbitrary byte or an
  // arbitrary length into an alert body.
  Error e = make_error(
      ErrorCategory::Validation,
      std::to_string(count) + " of " + std::to_string(names.size()) +
          " configured strategy name(s) would make every client_ref they mint "
          "unloggable — an alert or ledger entry about such an order reads "
          "client_ref=***REDACTED*** and cannot be linked to the store or the intent "
          "log. Fix the name(s) in configuration before trading; renaming changes the "
          "signal signature, so do it BETWEEN sessions with no working orders. First: " +
          first_reason);
  // Validation's default action is DoNotRetry; this must HALT the runtime, and the
  // fix is a human one, so state BlockStrategy explicitly.
  e.action = SuggestedAction::BlockStrategy;
  return fail(std::move(e));
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
