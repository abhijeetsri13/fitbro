#include "broker_exec/sessionguard/session_guard.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/session/session_state.hpp"

using broker_exec::errors::ErrorCategory;
using broker_exec::errors::SuggestedAction;
using broker_exec::session::SessionState;
using broker_exec::sessionguard::assess_session;
using broker_exec::sessionguard::is_reauth_needed;
using broker_exec::sessionguard::OpClass;
using broker_exec::sessionguard::require_op_allowed;
using broker_exec::sessionguard::SessionPosture;
using broker_exec::sessionguard::to_string;

TEST_CASE("a SessionExpired-classifying error mid-session flips Healthy -> NeedsReauth",
          "[sessionguard]") {
  // The mid-session death this module exists to catch: the caller still believes
  // it is Healthy, but the broker just answered a live call with a dead-token
  // reject. classify_rejection maps these to SessionExpired, which OVERRIDES the
  // caller's stale Healthy and freezes new entries.
  for (const char* text : {"Token is invalid or has expired", "401 unauthorized"}) {
    const SessionPosture p = assess_session(SessionState::Healthy, text);
    CHECK(p.state == SessionState::NeedsReauth);
    CHECK(p.freeze_entries);
    CHECK(p.allow_exits);
    CHECK(p.allow_reconcile_reads);
    CHECK(p.alert);
    CHECK(is_reauth_needed(p));
  }
}

TEST_CASE("a non-auth error stays on the caller's state — a margin reject is not auth-dead",
          "[sessionguard]") {
  // "insufficient margin" classifies to Margin, NOT SessionExpired, so the posture
  // stays Healthy and entries remain open: a margin reject must never trip the
  // re-auth freeze.
  const SessionPosture p = assess_session(SessionState::Healthy, "insufficient margin");
  CHECK(p.state == SessionState::Healthy);
  CHECK_FALSE(p.freeze_entries);
  CHECK_FALSE(p.alert);
  CHECK_FALSE(is_reauth_needed(p));
}

TEST_CASE("Healthy + benign/empty text stays Healthy with entries allowed", "[sessionguard]") {
  for (const char* text : {"", "   ", "OK"}) {
    const SessionPosture p = assess_session(SessionState::Healthy, text);
    CHECK(p.state == SessionState::Healthy);
    CHECK_FALSE(p.freeze_entries);
    CHECK(p.allow_exits);
    CHECK(p.allow_reconcile_reads);
    CHECK_FALSE(p.alert);
    CHECK(require_op_allowed(p, OpClass::Entry).has_value());
  }
}

TEST_CASE("require_op_allowed under NeedsReauth: entry blocked, exit/read allowed",
          "[sessionguard]") {
  const SessionPosture p = assess_session(SessionState::Healthy, "TokenException");
  REQUIRE(p.state == SessionState::NeedsReauth);

  const auto entry = require_op_allowed(p, OpClass::Entry);
  REQUIRE_FALSE(entry.has_value());
  CHECK(entry.error().category == ErrorCategory::SessionExpired);
  CHECK(entry.error().action == SuggestedAction::ReEstablishSession);
  // The Error names the op and points at re-establishing the session.
  CHECK(entry.error().message.find(std::string(to_string(OpClass::Entry))) != std::string::npos);
  CHECK(entry.error().message.find("session") != std::string::npos);

  // The load-bearing invariant: exits + reconcile reads are ALWAYS allowed.
  CHECK(require_op_allowed(p, OpClass::Exit).has_value());
  CHECK(require_op_allowed(p, OpClass::ReconcileRead).has_value());
}

TEST_CASE("require_op_allowed under Healthy: entry allowed", "[sessionguard]") {
  const SessionPosture p = assess_session(SessionState::Healthy, "");
  CHECK(require_op_allowed(p, OpClass::Entry).has_value());
  CHECK(require_op_allowed(p, OpClass::Exit).has_value());
  CHECK(require_op_allowed(p, OpClass::ReconcileRead).has_value());
}

TEST_CASE("current=Failed freezes entries but still permits exits", "[sessionguard]") {
  // A non-auth failure with benign text keeps `current` -> Failed: fail-closed.
  const SessionPosture p = assess_session(SessionState::Failed, "");
  CHECK(p.state == SessionState::Failed);
  CHECK(p.freeze_entries);
  CHECK(p.allow_exits);
  CHECK(p.alert);
  CHECK_FALSE(is_reauth_needed(p));  // Failed is NOT NeedsReauth

  REQUIRE_FALSE(require_op_allowed(p, OpClass::Entry).has_value());
  CHECK(require_op_allowed(p, OpClass::Exit).has_value());  // exits still ok
  CHECK(require_op_allowed(p, OpClass::ReconcileRead).has_value());
}

TEST_CASE("is_reauth_needed is true only for NeedsReauth", "[sessionguard]") {
  CHECK(is_reauth_needed(assess_session(SessionState::Healthy, "401 unauthorized")));
  CHECK_FALSE(is_reauth_needed(assess_session(SessionState::Healthy, "")));
  CHECK_FALSE(is_reauth_needed(assess_session(SessionState::Failed, "")));
}

TEST_CASE("a default-constructed SessionPosture is fail-closed", "[sessionguard]") {
  // A forgotten assignment must FREEZE new risk, never open it.
  const SessionPosture p;
  CHECK(p.state == SessionState::Failed);
  CHECK(p.freeze_entries);
  CHECK(p.allow_exits);
  CHECK(p.allow_reconcile_reads);
  CHECK(p.alert);

  REQUIRE_FALSE(require_op_allowed(p, OpClass::Entry).has_value());  // entry frozen
  CHECK(require_op_allowed(p, OpClass::Exit).has_value());           // exit still ok
  CHECK(require_op_allowed(p, OpClass::ReconcileRead).has_value());
}

TEST_CASE("redaction: a token-shaped broker_error_text never leaks into the detail",
          "[sessionguard]") {
  // The text is auth-dead phrasing carrying a token-shaped secret. The posture
  // detail must name the state only — never echo the raw text.
  const std::string token = "abcdef0123456789ABCDEF0123456789";  // 32-char token shape
  const std::string text = "TokenException: access_token " + token + " is invalid";
  const SessionPosture p = assess_session(SessionState::Healthy, text);
  REQUIRE(p.state == SessionState::NeedsReauth);
  CHECK(p.detail.find(token) == std::string::npos);
  CHECK(p.detail.find("access_token") == std::string::npos);
}

TEST_CASE("to_string names are the stable observability contract for OpClass", "[sessionguard]") {
  CHECK(to_string(OpClass::Entry) == "entry");
  CHECK(to_string(OpClass::Exit) == "exit");
  CHECK(to_string(OpClass::ReconcileRead) == "reconcile_read");
}
