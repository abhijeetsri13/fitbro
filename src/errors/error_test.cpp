#include "broker_exec/errors/error.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>

#include "broker_exec/expected.hpp"
#include "broker_exec/result.hpp"

using broker_exec::Result;
using broker_exec::errors::classify;
using broker_exec::errors::classify_http;
using broker_exec::errors::default_action_for;
using broker_exec::errors::Error;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::make_error;
using broker_exec::errors::SuggestedAction;
using broker_exec::errors::to_string;

TEST_CASE("default_action_for gives the documented baseline per category", "[errors]") {
  CHECK(default_action_for(ErrorCategory::Transient) == SuggestedAction::RetrySafe);
  CHECK(default_action_for(ErrorCategory::RateLimited) == SuggestedAction::RetrySafe);
  CHECK(default_action_for(ErrorCategory::Timeout) == SuggestedAction::ReconcileFirst);
  CHECK(default_action_for(ErrorCategory::Auth) == SuggestedAction::ReEstablishSession);
  CHECK(default_action_for(ErrorCategory::SessionExpired) == SuggestedAction::ReEstablishSession);
  CHECK(default_action_for(ErrorCategory::Validation) == SuggestedAction::DoNotRetry);
  CHECK(default_action_for(ErrorCategory::InsufficientFunds) == SuggestedAction::BlockStrategy);
  CHECK(default_action_for(ErrorCategory::RiskRejected) == SuggestedAction::ReconcileFirst);
  CHECK(default_action_for(ErrorCategory::NotSupported) == SuggestedAction::DoNotRetry);
  CHECK(default_action_for(ErrorCategory::DuplicateOrder) == SuggestedAction::ReconcileFirst);
  CHECK(default_action_for(ErrorCategory::OrderNotFound) == SuggestedAction::ReconcileFirst);
  CHECK(default_action_for(ErrorCategory::MarketClosed) == SuggestedAction::BlockStrategy);
  CHECK(default_action_for(ErrorCategory::DataStale) == SuggestedAction::BlockStrategy);
  CHECK(default_action_for(ErrorCategory::Internal) == SuggestedAction::RaiseAlert);
  CHECK(default_action_for(ErrorCategory::Unknown) == SuggestedAction::ReconcileFirst);
}

TEST_CASE("to_string is stable and total for both enums", "[errors]") {
  CHECK(to_string(ErrorCategory::SessionExpired) == "SessionExpired");
  CHECK(to_string(ErrorCategory::InsufficientFunds) == "InsufficientFunds");
  CHECK(to_string(ErrorCategory::RateLimited) == "RateLimited");
  CHECK(to_string(SuggestedAction::ReEstablishSession) == "ReEstablishSession");
  CHECK(to_string(SuggestedAction::ReconcileFirst) == "ReconcileFirst");
  CHECK(to_string(SuggestedAction::SquareOff) == "SquareOff");
}

TEST_CASE("classify_http maps representative HTTP statuses to category + action", "[errors]") {
  SECTION("401 without a session hint is a hard auth failure") {
    const Error e = classify_http(401, "bad api_key");
    CHECK(e.category == ErrorCategory::Auth);
    CHECK(e.action == SuggestedAction::ReEstablishSession);
    CHECK(e.broker_code == "HTTP 401");
  }
  SECTION("403 with an 'expired session' body is SessionExpired") {
    const Error e = classify_http(403, "your session has expired");
    CHECK(e.category == ErrorCategory::SessionExpired);
    CHECK(e.action == SuggestedAction::ReEstablishSession);
  }
  SECTION("429 is RateLimited / RetrySafe") {
    const Error e = classify_http(429, "Too Many Requests");
    CHECK(e.category == ErrorCategory::RateLimited);
    CHECK(e.action == SuggestedAction::RetrySafe);
  }
  SECTION("504 gateway timeout is Timeout / ReconcileFirst (never blind retry)") {
    const Error e = classify_http(504, "");
    CHECK(e.category == ErrorCategory::Timeout);
    CHECK(e.action == SuggestedAction::ReconcileFirst);
  }
  SECTION("400 is Validation / DoNotRetry") {
    const Error e = classify_http(400, "invalid order params");
    CHECK(e.category == ErrorCategory::Validation);
    CHECK(e.action == SuggestedAction::DoNotRetry);
  }
  SECTION("500 is a broker server error -> Network / ReconcileFirst") {
    const Error e = classify_http(500, "internal server error");
    CHECK(e.category == ErrorCategory::Network);
    CHECK(e.action == SuggestedAction::ReconcileFirst);
  }
  SECTION("404 is OrderNotFound / ReconcileFirst") {
    const Error e = classify_http(404, "");
    CHECK(e.category == ErrorCategory::OrderNotFound);
    CHECK(e.action == SuggestedAction::ReconcileFirst);
  }
}

TEST_CASE("classify_http never copies the response body into the Error (redaction-safe)",
          "[errors]") {
  // A body carrying a token-shaped string must not survive into a loggable
  // field. We only ever store the status code.
  const std::string secret = "access_token=abcd1234SECRETtoken";
  const Error e = classify_http(403, secret);
  CHECK(e.message.find("SECRET") == std::string::npos);
  CHECK(e.broker_code.find("SECRET") == std::string::npos);
  CHECK(e.broker_code == "HTTP 403");
}

TEST_CASE("classify maps representative broker error strings to the typed taxonomy", "[errors]") {
  SECTION("insufficient funds -> BlockStrategy") {
    const Error e = classify("RMS:RuleA", "Insufficient funds for this order");
    CHECK(e.category == ErrorCategory::InsufficientFunds);
    CHECK(e.action == SuggestedAction::BlockStrategy);
    CHECK(e.broker_code == "RMS:RuleA");
  }
  SECTION("expired session token -> ReEstablishSession") {
    const Error e = classify("TokenException", "Invalid or expired session token");
    CHECK(e.category == ErrorCategory::SessionExpired);
    CHECK(e.action == SuggestedAction::ReEstablishSession);
  }
  SECTION("rate limit text -> RetrySafe") {
    const Error e = classify("NetworkException", "Too many requests, please throttle");
    CHECK(e.category == ErrorCategory::RateLimited);
    CHECK(e.action == SuggestedAction::RetrySafe);
  }
  SECTION("duplicate order -> ReconcileFirst") {
    const Error e = classify("", "Order already exists for this client id");
    CHECK(e.category == ErrorCategory::DuplicateOrder);
    CHECK(e.action == SuggestedAction::ReconcileFirst);
  }
  SECTION("modify on a filled order -> OrderNotFound / ReconcileFirst") {
    const Error e = classify("", "Order already filled, cannot modify");
    CHECK(e.category == ErrorCategory::OrderNotFound);
    CHECK(e.action == SuggestedAction::ReconcileFirst);
  }
  SECTION("market closed -> BlockStrategy") {
    const Error e = classify("", "Market is closed for this segment");
    CHECK(e.category == ErrorCategory::MarketClosed);
    CHECK(e.action == SuggestedAction::BlockStrategy);
  }
  SECTION("invalid tick/lot -> Validation / DoNotRetry") {
    const Error e = classify("InputException", "Invalid price, not a tick multiple");
    CHECK(e.category == ErrorCategory::Validation);
    CHECK(e.action == SuggestedAction::DoNotRetry);
  }
  SECTION("unsupported capability -> NotSupported / DoNotRetry") {
    const Error e = classify("", "Operation not supported by this broker");
    CHECK(e.category == ErrorCategory::NotSupported);
    CHECK(e.action == SuggestedAction::DoNotRetry);
  }
  SECTION("a stated-but-unmapped broker code -> BrokerRejected, code preserved") {
    const Error e = classify("17082", "Some new broker reason");
    CHECK(e.category == ErrorCategory::BrokerRejected);
    CHECK(e.broker_code == "17082");
  }
  SECTION("nothing at all -> Unknown / ReconcileFirst") {
    const Error e = classify("", "");
    CHECK(e.category == ErrorCategory::Unknown);
    CHECK(e.action == SuggestedAction::ReconcileFirst);
  }
}

TEST_CASE("CAP-13: the same condition from two broker dialects normalizes identically",
          "[errors]") {
  const Error kite = classify("InsufficientFunds", "Insufficient funds");
  const Error kotak = classify("", "order rejected: not enough margin available");
  CHECK(kite.category == kotak.category);
  CHECK(kite.action == kotak.action);
  CHECK(kite.category == ErrorCategory::InsufficientFunds);
}

namespace {
// A fallible call modeled the way the library expects: it returns a value or a
// typed Error via Result<T>, and it does NOT throw.
Result<int> parse_quantity(int raw) {
  if (raw <= 0) {
    return broker_exec::fail(make_error(ErrorCategory::Validation, "quantity must be positive"));
  }
  return raw;
}
}  // namespace

TEST_CASE("Result<T> carries errors as values without throwing across the boundary", "[errors]") {
  SECTION("the happy path holds a value") {
    const Result<int> ok = parse_quantity(50);
    REQUIRE(ok.has_value());
    CHECK(static_cast<bool>(ok));
    CHECK(ok.value() == 50);
  }
  SECTION("the failure path holds a typed Error, no exception is thrown") {
    Result<int> bad = parse_quantity(0);  // must not throw
    REQUIRE_FALSE(bad.has_value());
    CHECK_FALSE(static_cast<bool>(bad));
    CHECK(bad.error().category == ErrorCategory::Validation);
    CHECK(bad.error().action == SuggestedAction::DoNotRetry);
    CHECK(bad.error().message == "quantity must be positive");
  }
}

TEST_CASE("expected<T,E> destroys the active alternative correctly for non-trivial types",
          "[errors]") {
  // Error holds std::strings; exercise copy/move/assign so a leak or
  // double-free would surface under ASan in the sanitizer CI matrix.
  Result<std::string> a = std::string("hello");
  REQUIRE(a.has_value());
  CHECK(a.value() == "hello");

  Result<std::string> b = broker_exec::fail(make_error(ErrorCategory::Internal, "boom", "X1"));
  REQUIRE_FALSE(b.has_value());
  CHECK(b.error().broker_code == "X1");

  a = b;  // value-state -> error-state copy assignment
  REQUIRE_FALSE(a.has_value());
  CHECK(a.error().message == "boom");

  Result<std::string> c = std::move(b);  // move-construct from error state
  REQUIRE_FALSE(c.has_value());
  CHECK(c.error().category == ErrorCategory::Internal);
}

TEST_CASE("make_error with an explicit action overrides the category default", "[errors]") {
  // Internal defaults to RaiseAlert. A schema too new to understand IS internal,
  // but retrying it is futile — the store's open paths need to say so without
  // aggregate-initializing Error, which is what broke the clang build (#8).
  REQUIRE(default_action_for(ErrorCategory::Internal) == SuggestedAction::RaiseAlert);

  const auto err = make_error(ErrorCategory::Internal, SuggestedAction::DoNotRetry,
                              "store: schema newer than this build", "SQLITE_SCHEMA");

  CHECK(err.category == ErrorCategory::Internal);
  CHECK(err.action == SuggestedAction::DoNotRetry);
  CHECK(err.message == "store: schema newer than this build");
  CHECK(err.broker_code == "SQLITE_SCHEMA");
}

TEST_CASE("make_error with an explicit action leaves broker_code empty by default", "[errors]") {
  const auto err = make_error(ErrorCategory::Validation, SuggestedAction::DoNotRetry, "bad input");
  CHECK(err.broker_code.empty());
  CHECK(err.action == SuggestedAction::DoNotRetry);
}
