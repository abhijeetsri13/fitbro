#include "broker_exec/cli/cli_app.hpp"

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <string>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/result.hpp"

using broker_exec::Result;
using broker_exec::cli::dispatch;
using broker_exec::cli::is_mutating;
using broker_exec::cli::OperatorApi;
using broker_exec::cli::to_string;
using broker_exec::cli::Verb;
using broker_exec::errors::ErrorCategory;

namespace {

constexpr Verb kAllVerbs[] = {Verb::Status,        Verb::Reconcile,    Verb::ReplayIntentLog,
                              Verb::SafeStartCheck, Verb::SendTestAlert, Verb::VerifyIp,
                              Verb::Kill};

// A spy callback that records invocation and returns a tagged success line, so a
// test can prove dispatch routed to exactly the seam it expected.
[[nodiscard]] std::function<Result<std::string>()> spy(bool& flag, std::string tag) {
  return [&flag, tag]() -> Result<std::string> {
    flag = true;
    return tag;
  };
}

}  // namespace

// ── AC-1: is_mutating — only Kill mutates ───────────────────────────────────

TEST_CASE("is_mutating is true for Kill ONLY", "[cli][verb]") {
  for (const Verb verb : kAllVerbs) {
    if (verb == Verb::Kill) {
      CHECK(is_mutating(verb));
    } else {
      CHECK_FALSE(is_mutating(verb));
    }
  }
}

// ── AC-1: dispatch routes each verb to its own seam ─────────────────────────

TEST_CASE("dispatch(Status) calls ONLY the status seam", "[cli][dispatch]") {
  bool status_called = false;
  bool reconcile_called = false;
  bool kill_called = false;

  OperatorApi api;
  api.status = spy(status_called, "status-ok");
  api.reconcile = spy(reconcile_called, "reconcile-ok");
  api.kill = spy(kill_called, "kill-ok");

  const Result<std::string> result = dispatch(Verb::Status, api);
  REQUIRE(result);
  CHECK(result.value() == "status-ok");
  CHECK(status_called);
  CHECK_FALSE(reconcile_called);
  CHECK_FALSE(kill_called);  // a read verb NEVER reaches the mutating seam
}

TEST_CASE("every read verb maps to its own seam", "[cli][dispatch]") {
  bool status_f = false, reconcile_f = false, replay_f = false, safe_f = false, alert_f = false,
       ip_f = false, kill_f = false;

  OperatorApi api;
  api.status = spy(status_f, "status");
  api.reconcile = spy(reconcile_f, "reconcile");
  api.replay_intent_log = spy(replay_f, "replay");
  api.safe_start_check = spy(safe_f, "safe");
  api.send_test_alert = spy(alert_f, "alert");
  api.verify_ip = spy(ip_f, "ip");
  api.kill = spy(kill_f, "kill");

  CHECK(dispatch(Verb::Reconcile, api).value() == "reconcile");
  CHECK(reconcile_f);
  CHECK(dispatch(Verb::ReplayIntentLog, api).value() == "replay");
  CHECK(replay_f);
  CHECK(dispatch(Verb::SafeStartCheck, api).value() == "safe");
  CHECK(safe_f);
  CHECK(dispatch(Verb::SendTestAlert, api).value() == "alert");
  CHECK(alert_f);
  CHECK(dispatch(Verb::VerifyIp, api).value() == "ip");
  CHECK(ip_f);
  CHECK(dispatch(Verb::Kill, api).value() == "kill");
  CHECK(kill_f);
}

// ── AC-1: a null seam callback FAILS CLOSED (Error, no crash) ───────────────

TEST_CASE("a null read callback fails closed with a typed Error", "[cli][dispatch]") {
  OperatorApi api;  // every callback default-constructed -> null
  const Result<std::string> result = dispatch(Verb::Status, api);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::Internal);
  // The message names the verb and is redaction-safe.
  CHECK(result.error().message.find("status") != std::string::npos);
}

TEST_CASE("a null kill callback fails closed — no crash, no UB", "[cli][dispatch]") {
  OperatorApi api;  // kill is null
  const Result<std::string> result = dispatch(Verb::Kill, api);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::Internal);
  CHECK(result.error().message.find("kill") != std::string::npos);
}

TEST_CASE("every verb fails closed when its seam is unwired", "[cli][dispatch]") {
  const OperatorApi api;  // all null
  for (const Verb verb : kAllVerbs) {
    const Result<std::string> result = dispatch(verb, api);
    INFO("verb = " << to_string(verb));
    CHECK_FALSE(result);
  }
}

TEST_CASE("verb names are the stable subcommand spellings", "[cli][verb]") {
  CHECK(to_string(Verb::Status) == "status");
  CHECK(to_string(Verb::ReplayIntentLog) == "replay-intent-log");
  CHECK(to_string(Verb::SafeStartCheck) == "safe-start-check");
  CHECK(to_string(Verb::SendTestAlert) == "send-test-alert");
  CHECK(to_string(Verb::VerifyIp) == "verify-ip");
  CHECK(to_string(Verb::Kill) == "kill");
}
