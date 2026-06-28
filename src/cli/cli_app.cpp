#include "broker_exec/cli/cli_app.hpp"

#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <string_view>

// CLI11 is CONFINED to this translation unit (its headers never enter any public
// header).
#include <CLI/CLI.hpp>

#include "broker_exec/domain/redaction.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::cli {

namespace {

using errors::Error;
using errors::ErrorCategory;
using errors::make_error;
using errors::SuggestedAction;

// Fail-closed Error for a verb whose seam callback is null (not wired). A missing
// wire must HALT, not silently no-op: Internal category, redaction-safe message
// naming only the verb. Action BlockStrategy (a wiring fault is a deployment fault,
// not a retry) — overriding Internal's RaiseAlert default, mirroring safe_start.cpp.
[[nodiscard]] Error not_wired_error(Verb verb) {
  Error err = make_error(ErrorCategory::Internal,
                         "cli: verb '" + std::string(to_string(verb)) + "' is not wired");
  err.action = SuggestedAction::BlockStrategy;
  return err;
}

// Forward to the seam callback if present; otherwise fail closed. The single place
// that maps a Verb's std::function to its invocation — dispatch performs no module
// work itself and never mutates.
[[nodiscard]] Result<std::string> invoke(Verb verb,
                                         const std::function<Result<std::string>()>& callback) {
  if (!callback) {
    return fail(not_wired_error(verb));
  }
  return callback();
}

}  // namespace

std::string_view to_string(Verb verb) noexcept {
  switch (verb) {
    case Verb::Status:
      return "status";
    case Verb::Reconcile:
      return "reconcile";
    case Verb::ReplayIntentLog:
      return "replay-intent-log";
    case Verb::SafeStartCheck:
      return "safe-start-check";
    case Verb::SendTestAlert:
      return "send-test-alert";
    case Verb::VerifyIp:
      return "verify-ip";
    case Verb::Kill:
      return "kill";
  }
  return "unknown";
}

bool is_mutating(Verb verb) noexcept {
  // Exactly one mutating verb (AC-1). Every other verb is read-only and must never
  // reach a mutating path.
  return verb == Verb::Kill;
}

Result<std::string> dispatch(Verb verb, const OperatorApi& api) {
  switch (verb) {
    case Verb::Status:
      return invoke(verb, api.status);
    case Verb::Reconcile:
      return invoke(verb, api.reconcile);
    case Verb::ReplayIntentLog:
      return invoke(verb, api.replay_intent_log);
    case Verb::SafeStartCheck:
      return invoke(verb, api.safe_start_check);
    case Verb::SendTestAlert:
      return invoke(verb, api.send_test_alert);
    case Verb::VerifyIp:
      return invoke(verb, api.verify_ip);
    case Verb::Kill:
      return invoke(verb, api.kill);
  }
  // Unreachable for a valid enumerator (the switch is exhaustive; -Wswitch/WX guards
  // an added one). Fail CLOSED rather than silently no-op.
  return fail(not_wired_error(verb));
}

int run_cli(int argc, char** argv, const OperatorApi& api) {
  CLI::App app{"broker_exec operator CLI"};
  app.require_subcommand(1);  // exactly one verb per invocation

  // One subcommand per verb. Each only records the chosen verb — dispatch (the
  // tested seam) does the routing; this layer is a pure shell.
  Verb selected = Verb::Status;
  bool chosen = false;
  const auto add = [&](Verb verb, const char* help) {
    CLI::App* sub = app.add_subcommand(std::string(to_string(verb)), help);
    sub->callback([&selected, &chosen, verb]() {
      selected = verb;
      chosen = true;
    });
  };

  add(Verb::Status, "Report engine status (read-only)");
  add(Verb::Reconcile, "Run reconciliation report (read-only)");
  add(Verb::ReplayIntentLog, "Verify intent-log replay (read-only)");
  add(Verb::SafeStartCheck, "Run the safe-start gate (read-only)");
  add(Verb::SendTestAlert, "Emit a test alert (read-only)");
  add(Verb::VerifyIp, "Verify the egress IP (read-only)");
  add(Verb::Kill, "Engage the kill switch (MUTATING)");

  CLI11_PARSE(app, argc, argv);

  if (!chosen) {
    // require_subcommand(1) should prevent this; guard anyway (fail closed).
    std::fprintf(stderr, "%s\n", "cli: no verb selected");
    return 1;
  }

  // dispatch() itself is no-throw, but a caller-supplied seam callback could throw
  // (the OperatorApi is external). Contain it here so a misbehaving callback exits
  // non-zero with a scrubbed line instead of escaping main -> std::terminate.
  Result<std::string> result = fail(make_error(ErrorCategory::Internal, "cli: dispatch failed"));
  try {
    result = dispatch(selected, api);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", domain::scrub(std::string("cli: ") + e.what()).c_str());
    return 1;
  } catch (...) {
    std::fprintf(stderr, "%s\n", "cli: dispatch failed (unknown exception)");
    return 1;
  }
  if (!result) {
    // The Error message is already redaction-safe by contract; scrub again as a
    // belt-and-braces outbound guard (every printed line scrubs itself).
    std::fprintf(stderr, "%s\n", domain::scrub(result.error().message).c_str());
    return 1;
  }

  // Scrub the success line before printing — it is an outbound payload.
  std::fprintf(stdout, "%s\n", domain::scrub(result.value()).c_str());
  return 0;
}

}  // namespace broker_exec::cli
