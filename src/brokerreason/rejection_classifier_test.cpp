#include "broker_exec/brokerreason/rejection_classifier.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>

#include "broker_exec/errors/error.hpp"

using broker_exec::brokerreason::Classification;
using broker_exec::brokerreason::classify_rejection;
using broker_exec::brokerreason::classify_status;
using broker_exec::brokerreason::kClassifierVersion;
using broker_exec::brokerreason::RejectReason;
using broker_exec::brokerreason::RetryPosture;
using broker_exec::brokerreason::to_string;
using broker_exec::brokerreason::to_suggested_action;
using broker_exec::errors::SuggestedAction;

namespace {

struct RejectCase {
  std::string_view raw;  // a REAL-shaped broker rejection string
  RejectReason reason;
  RetryPosture posture;
  bool should_alert;
};

// Table-driven coverage of EVERY mapping, with real-complaint-shaped strings.
constexpr RejectCase kRejectCases[] = {
    // Margin / funds — DoNotRetry, alert.
    {"Insufficient margin", RejectReason::Margin, RetryPosture::DoNotRetry, true},
    {"RMS:Margin Exceeds,Available", RejectReason::Margin, RetryPosture::DoNotRetry, true},
    {"insufficient funds for this order", RejectReason::Margin, RetryPosture::DoNotRetry, true},

    // Circuit / price band — DoNotRetry, alert.
    {"price out of LPP range", RejectReason::CircuitLimit, RetryPosture::DoNotRetry, true},
    {"Order price is out of range", RejectReason::CircuitLimit, RetryPosture::DoNotRetry, true},
    {"circuit limit breached", RejectReason::CircuitLimit, RetryPosture::DoNotRetry, true},
    {"DPR violation", RejectReason::CircuitLimit, RetryPosture::DoNotRetry, true},

    // Freeze quantity — DoNotRetry (caller re-routes to the slicer), alert.
    {"Quantity higher than maximum allowed by exchange", RejectReason::FreezeQuantity,
     RetryPosture::DoNotRetry, true},
    {"order quantity in freeze", RejectReason::FreezeQuantity, RetryPosture::DoNotRetry, true},

    // Illiquid — DoNotRetry, alert.
    {"no trades in this instrument", RejectReason::Illiquid, RetryPosture::DoNotRetry, true},
    {"instrument is illiquid", RejectReason::Illiquid, RetryPosture::DoNotRetry, true},

    // Session / auth — ReconcileFirst, alert.
    {"Token is invalid or has expired", RejectReason::SessionExpired, RetryPosture::ReconcileFirst,
     true},
    {"401 Unauthorized", RejectReason::SessionExpired, RetryPosture::ReconcileFirst, true},
    {"Invalid `api_key` or `access_token`", RejectReason::SessionExpired,
     RetryPosture::ReconcileFirst, true},

    // Rate limited — SafeToRetryReadOnly, NO alert.
    {"Too many requests", RejectReason::RateLimited, RetryPosture::SafeToRetryReadOnly, false},
    {"429: rate limit exceeded", RejectReason::RateLimited, RetryPosture::SafeToRetryReadOnly,
     false},

    // Already complete — ReconcileFirst, NO alert.
    {"order is already complete", RejectReason::AlreadyComplete, RetryPosture::ReconcileFirst,
     false},
    {"This order has already been cancelled", RejectReason::AlreadyComplete,
     RetryPosture::ReconcileFirst, false},

    // Order not found — ReconcileFirst, alert.
    {"order not found", RejectReason::OrderNotFound, RetryPosture::ReconcileFirst, true},
    {"Invalid order id", RejectReason::OrderNotFound, RetryPosture::ReconcileFirst, true},

    // Indeterminate / duplicate-order hazard — ReconcileFirst, alert.
    {"Error parsing response (kt-oms) for order placement", RejectReason::Indeterminate,
     RetryPosture::ReconcileFirst, true},
    {"gateway timeout", RejectReason::Indeterminate, RetryPosture::ReconcileFirst, true},
    {"503 Service Unavailable", RejectReason::Indeterminate, RetryPosture::ReconcileFirst, true},
    {"no response from OMS", RejectReason::Indeterminate, RetryPosture::ReconcileFirst, true},
    {"request timed out", RejectReason::Indeterminate, RetryPosture::ReconcileFirst, true},

    // Generic RMS block — DoNotRetry, alert.
    {"RMS rejected: not allowed to trade this scrip", RejectReason::RmsBlock,
     RetryPosture::DoNotRetry, true},
    {"Your account is blocked for trading", RejectReason::RmsBlock, RetryPosture::DoNotRetry, true},

    // FAIL-CLOSED: empty + gibberish ⇒ Unknown / DoNotRetry / alert.
    {"", RejectReason::Unknown, RetryPosture::DoNotRetry, true},
    {"   ", RejectReason::Unknown, RetryPosture::DoNotRetry, true},
    {"asdf qwerty", RejectReason::Unknown, RetryPosture::DoNotRetry, true},
    {"\xC2\xA1ole!", RejectReason::Unknown, RetryPosture::DoNotRetry, true},
};

}  // namespace

TEST_CASE("classify_rejection maps every reason with real complaint strings", "[brokerreason]") {
  for (const RejectCase& tc : kRejectCases) {
    const Classification c = classify_rejection(tc.raw);
    INFO("raw=\"" << tc.raw << "\" detail=\"" << c.canonical_detail << "\"");
    CHECK(c.reason == tc.reason);
    CHECK(c.posture == tc.posture);
    CHECK(c.should_alert == tc.should_alert);
    // A mutating reject is NEVER safe-to-retry: only RateLimited earns that.
    if (tc.reason != RejectReason::RateLimited) {
      CHECK(c.posture != RetryPosture::SafeToRetryReadOnly);
    }
  }
}

TEST_CASE("FAIL-CLOSED: empty and unmatched text never become safe-to-retry", "[brokerreason]") {
  for (const std::string_view raw : {std::string_view(""), std::string_view("   "),
                                     std::string_view("totally novel broker wording 2027")}) {
    const Classification c = classify_rejection(raw);
    CHECK(c.reason == RejectReason::Unknown);
    CHECK(c.posture == RetryPosture::DoNotRetry);  // the core safety property
    CHECK(c.should_alert);
    CHECK(c.posture != RetryPosture::SafeToRetryReadOnly);
  }
}

TEST_CASE("FAIL-OPEN GUARD: a hard reject merely CONTAINING '429' in an id is not retry-able",
          "[brokerreason]") {
  // A bare "429" substring collides with arbitrary numeric ids. Such a message has
  // no rate-limit phrasing, so it must NOT become SafeToRetryReadOnly (which would
  // tell the caller it may resend a possibly-live order => duplicate).
  for (const std::string_view raw :
       {std::string_view("Order rejected at exchange (ref: 980429117)"),
        std::string_view("could not place order #1234290")}) {
    const Classification c = classify_rejection(raw);
    CHECK(c.posture != RetryPosture::SafeToRetryReadOnly);
    CHECK(c.reason != RejectReason::RateLimited);
  }
  // A genuine throttle (with rate-limit phrasing) IS rate-limited.
  CHECK(classify_rejection("HTTP 429 Too Many Requests").reason == RejectReason::RateLimited);
  CHECK(classify_rejection("request was throttled").reason == RejectReason::RateLimited);
}

TEST_CASE("Indeterminate covers 502/504 and a bare timeout (duplicate-order hazard)",
          "[brokerreason]") {
  for (const std::string_view raw :
       {std::string_view("502 Bad Gateway"), std::string_view("504 Gateway Timeout"),
        std::string_view("request timeout")}) {
    const Classification c = classify_rejection(raw);
    CHECK(c.reason == RejectReason::Indeterminate);
    CHECK(c.posture == RetryPosture::ReconcileFirst);
  }
}

TEST_CASE("classify_status('rejected') fails closed (no reason in a bare status)",
          "[brokerreason]") {
  const Classification c = classify_status("REJECTED");
  CHECK(c.reason == RejectReason::Unknown);
  CHECK(c.posture == RetryPosture::DoNotRetry);
  CHECK(c.should_alert);
}

TEST_CASE("classification is case-insensitive", "[brokerreason]") {
  CHECK(classify_rejection("INSUFFICIENT MARGIN").reason == RejectReason::Margin);
  CHECK(classify_rejection("insufficient margin").reason == RejectReason::Margin);
  CHECK(classify_rejection("InSuFfIcIeNt MaRgIn").reason == RejectReason::Margin);
  CHECK(classify_rejection("TOO MANY REQUESTS").reason == RejectReason::RateLimited);
  CHECK(classify_rejection("Gateway TimeOut").reason == RejectReason::Indeterminate);
}

TEST_CASE("Margin wins over generic RMS for 'RMS:Margin'", "[brokerreason]") {
  // Precedence guard: the specific Margin reason must beat the generic RmsBlock
  // ("rms") even though the text contains both signals.
  CHECK(classify_rejection("RMS:Margin shortfall").reason == RejectReason::Margin);
}

TEST_CASE("canonical_detail is redaction-safe and omits raw broker text/tokens", "[brokerreason]") {
  // Feed a reject carrying a fake token-shaped secret. The detail must NOT contain
  // any part of the raw input — by construction it echoes only canonical words.
  const std::string_view token = "abcdef0123456789ABCDEF0123456789";
  const std::string raw = std::string("RMS rejected order; access_token=") + std::string(token) +
                          " for account ABCD1234";
  const Classification c = classify_rejection(raw);
  CHECK(c.canonical_detail.find(std::string(token)) == std::string::npos);
  CHECK(c.canonical_detail.find("access_token=") == std::string::npos);
  CHECK(c.canonical_detail.find("ABCD1234") == std::string::npos);
  // It DOES name the canonical reason + posture + version.
  CHECK(c.canonical_detail.find(std::string(to_string(c.reason))) != std::string::npos);
  CHECK(c.canonical_detail.find(std::string(to_string(c.posture))) != std::string::npos);
  CHECK(c.canonical_detail.find(std::to_string(kClassifierVersion)) != std::string::npos);
}

TEST_CASE("classify_status recognizes known states and reconciles unknown ones", "[brokerreason]") {
  // COMPLETE recognized (terminal-done): not the unknown-status default.
  const Classification complete = classify_status("COMPLETE");
  CHECK(complete.reason == RejectReason::AlreadyComplete);
  CHECK(complete.posture == RetryPosture::ReconcileFirst);
  CHECK_FALSE(complete.should_alert);

  // A known in-flight state: reconcile, no alert.
  const Classification open = classify_status("TRIGGER PENDING");
  CHECK(open.posture == RetryPosture::ReconcileFirst);
  CHECK_FALSE(open.should_alert);

  // The fail-safe: an UNRECOGNIZED status ⇒ Indeterminate + ReconcileFirst + alert.
  for (const std::string_view bogus :
       {std::string_view("WAT"), std::string_view(""), std::string_view("ordSt=99")}) {
    const Classification c = classify_status(bogus);
    CHECK(c.reason == RejectReason::Indeterminate);
    CHECK(c.posture == RetryPosture::ReconcileFirst);
    CHECK(c.should_alert);
  }
}

TEST_CASE("to_string is stable for both enums", "[brokerreason]") {
  CHECK(to_string(RejectReason::Margin) == "margin");
  CHECK(to_string(RejectReason::CircuitLimit) == "circuit_limit");
  CHECK(to_string(RejectReason::FreezeQuantity) == "freeze_quantity");
  CHECK(to_string(RejectReason::Illiquid) == "illiquid");
  CHECK(to_string(RejectReason::RmsBlock) == "rms_block");
  CHECK(to_string(RejectReason::SessionExpired) == "session_expired");
  CHECK(to_string(RejectReason::RateLimited) == "rate_limited");
  CHECK(to_string(RejectReason::OrderNotFound) == "order_not_found");
  CHECK(to_string(RejectReason::AlreadyComplete) == "already_complete");
  CHECK(to_string(RejectReason::Indeterminate) == "indeterminate");
  CHECK(to_string(RejectReason::Unknown) == "unknown");

  CHECK(to_string(RetryPosture::DoNotRetry) == "do_not_retry");
  CHECK(to_string(RetryPosture::ReconcileFirst) == "reconcile_first");
  CHECK(to_string(RetryPosture::SafeToRetryReadOnly) == "safe_to_retry_read_only");
}

TEST_CASE("to_suggested_action bridges posture onto the typed error action", "[brokerreason]") {
  CHECK(to_suggested_action(RetryPosture::DoNotRetry) == SuggestedAction::DoNotRetry);
  CHECK(to_suggested_action(RetryPosture::ReconcileFirst) == SuggestedAction::ReconcileFirst);
  CHECK(to_suggested_action(RetryPosture::SafeToRetryReadOnly) == SuggestedAction::RetrySafe);
}
