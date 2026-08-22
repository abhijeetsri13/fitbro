// Kotak Neo error-mapping tests (Story 6.1, AC-2/AC-3).
//
// Every body below is a COMMITTED RECORDED-RESPONSE FIXTURE: the exact envelope
// shapes the Kotak Neo API returns, pinned here so the mapping is verified with
// no network and no live credentials (the VCR substrate, TO-6 tier-1). Where the
// real value is uncertain the fixture is our recorded assumption and the matching
// capability stays Unknown until a tier-2 live run confirms it.
//
// The credentials in these fixtures are SYNTHETIC and token-shaped (long
// alphanumeric runs) precisely so `domain::scrub` must redact them.

#include "broker_exec/adapters/kotak/kotak_errors.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <utility>
#include <vector>

#include "broker_exec/adapters/kotak/kotak_transport.hpp"
#include "broker_exec/errors/error.hpp"

using broker_exec::adapters::kotak::HttpResponse;
using broker_exec::adapters::kotak::is_kotak_success;
using broker_exec::adapters::kotak::is_session_death;
using broker_exec::adapters::kotak::KotakEnvelope;
using broker_exec::adapters::kotak::map_kotak_error;
using broker_exec::adapters::kotak::parse_kotak_envelope;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::SuggestedAction;

namespace {

// ── Committed Kotak Neo response fixtures ────────────────────────────────────

// A successful trading envelope (order book read).
constexpr const char* kOrderBookOk =
    R"({"stat":"Ok","stCode":200,"data":[{"nOrdNo":"220101000000001","ordSt":"complete","trdSym":"INFY-EQ"}]})";

// The funds/limits envelope (a day-one-critical-path unknown resolved by fixture).
// Money arrives as STRINGS on the wire; nothing here converts them to a double.
constexpr const char* kLimitsOk =
    R"({"stat":"Ok","stCode":200,"Net":"104325.75","MarginUsed":"12500.00","CollateralValue":"0.00","AdhocMargin":"0.00"})";

// THE TRAP: an HTTP 200 that is actually a rejection.
constexpr const char* kNotOkMargin =
    R"({"stat":"Not_Ok","errMsg":"RMS:Margin Exceeds,Available margin is 1200.00","stCode":5203})";

constexpr const char* kNotOkInvalidQty =
    R"({"stat":"Not_Ok","errMsg":"Invalid quantity for this contract","stCode":5001})";

constexpr const char* kNotOkSessionDead =
    R"({"stat":"Not_Ok","errMsg":"Invalid Session or Token expired, please re-login","stCode":10502})";

constexpr const char* kNotOkUnclassifiable =
    R"({"stat":"Not_Ok","errMsg":"Something the table has never seen","stCode":9999})";

// An ORDER reject that merely contains the word "token" — NOT a dead session.
constexpr const char* kNotOkInstrumentToken =
    R"({"stat":"Not_Ok","errMsg":"Invalid instrument token for this exchange segment","stCode":5006})";

// A failing gateway whose BODY carries misleading session phrasing. The status
// is the fact; the body is only a hint. (The throttle half of this regression
// reuses kFaultThrottled below with a 5xx status.)
constexpr const char* kServerErrorSayingTokenExpired =
    R"({"stat":"Not_Ok","errMsg":"token expired while routing, please re-login","stCode":500})";

// The API-gateway fault shape (sits beside the trading envelope, not inside it).
constexpr const char* kFaultThrottled =
    R"({"fault":{"code":"900802","message":"Message throttled out","description":"You have exceeded your quota limit"}})";

constexpr const char* kFaultMissingCreds =
    R"({"fault":{"faultstring":"Invalid Credentials. Make sure you have given the correct access token","detail":{"errorcode":"900901"}}})";

constexpr const char* kFaultServerError =
    R"({"fault":{"code":"500","message":"Internal Server Error at the gateway"}})";

// The login endpoints answer failures with an `error` node.
constexpr const char* kLoginErrorNode =
    R"({"error":[{"code":"10022","message":"Invalid Credentials"}]})";

[[nodiscard]] HttpResponse response(long status, std::string body,
                                    std::vector<std::pair<std::string, std::string>> headers = {}) {
  HttpResponse r;
  r.status_code = status;
  r.body = std::move(body);
  r.headers = std::move(headers);
  return r;
}

}  // namespace

TEST_CASE("the Kotak envelope parses stat / errMsg / stCode", "[kotak][errors]") {
  const KotakEnvelope ok = parse_kotak_envelope(kOrderBookOk);
  CHECK(ok.parsed);
  CHECK(ok.has_stat);
  CHECK(ok.stat_ok);
  CHECK(ok.has_data);
  CHECK(ok.message.empty());

  const KotakEnvelope bad = parse_kotak_envelope(kNotOkMargin);
  CHECK(bad.parsed);
  CHECK(bad.has_stat);
  CHECK_FALSE(bad.stat_ok);
  CHECK(bad.message.find("Margin Exceeds") != std::string::npos);
  CHECK(bad.status_code == "5203");
}

TEST_CASE("the margins/limits envelope parses (day-one critical path)", "[kotak][errors]") {
  const KotakEnvelope limits = parse_kotak_envelope(kLimitsOk);
  CHECK(limits.parsed);
  CHECK(limits.stat_ok);
  CHECK(is_kotak_success(response(200, kLimitsOk)));
}

TEST_CASE("the gateway fault shape parses in both spellings", "[kotak][errors]") {
  const KotakEnvelope throttled = parse_kotak_envelope(kFaultThrottled);
  CHECK(throttled.has_fault);
  CHECK(throttled.status_code == "900802");
  CHECK(throttled.message.find("throttled") != std::string::npos);

  // faultstring + detail.errorcode is the other shipped spelling.
  const KotakEnvelope missing = parse_kotak_envelope(kFaultMissingCreds);
  CHECK(missing.has_fault);
  CHECK(missing.status_code == "900901");
  CHECK_FALSE(missing.message.empty());
}

TEST_CASE("the login error node parses", "[kotak][errors]") {
  const KotakEnvelope login = parse_kotak_envelope(kLoginErrorNode);
  CHECK(login.has_error);
  CHECK(login.status_code == "10022");
  CHECK(login.message == "Invalid Credentials");
}

TEST_CASE("a malformed body never throws and never contradicts the status", "[kotak][errors]") {
  const KotakEnvelope garbage = parse_kotak_envelope("not json {{{");
  CHECK_FALSE(garbage.parsed);
  CHECK_FALSE(is_kotak_success(response(500, "not json {{{")));

  // NARROW CONTRACT: `is_kotak_success` answers "did the ENVELOPE contradict this
  // 2xx?", and an unparseable body cannot. It is NOT proof the call succeeded —
  // callers that need that (request_json's payload parse, validate()'s health
  // gate) must additionally require `parsed`, because a captive portal or an
  // HTML maintenance page is also a 200 with a body that is not Kotak.
  CHECK(is_kotak_success(response(200, "not json {{{")));
  CHECK_FALSE(parse_kotak_envelope("<html>portal login</html>").parsed);
}

TEST_CASE("HTTP 200 with stat Not_Ok is NOT success (the Kotak trap)", "[kotak][errors]") {
  CHECK(is_kotak_success(response(200, kOrderBookOk)));
  CHECK_FALSE(is_kotak_success(response(200, kNotOkMargin)));
  CHECK_FALSE(is_kotak_success(response(200, kFaultThrottled)));
  CHECK_FALSE(is_kotak_success(response(200, kLoginErrorNode)));
}

TEST_CASE("401/403 and session phrasing both read as session death", "[kotak][errors]") {
  CHECK(is_session_death(response(401, kLoginErrorNode)));
  CHECK(is_session_death(response(403, "")));
  // The dangerous one: HTTP 200 carrying a dead-session envelope.
  CHECK(is_session_death(response(200, kNotOkSessionDead)));
  CHECK_FALSE(is_session_death(response(200, kOrderBookOk)));
  CHECK_FALSE(is_session_death(response(200, kNotOkMargin)));
}

TEST_CASE("session death maps to SessionExpired / ReEstablishSession", "[kotak][errors]") {
  const auto from_status = map_kotak_error(response(401, kLoginErrorNode));
  CHECK(from_status.category == ErrorCategory::SessionExpired);
  CHECK(from_status.action == SuggestedAction::ReEstablishSession);

  const auto from_envelope = map_kotak_error(response(200, kNotOkSessionDead));
  CHECK(from_envelope.category == ErrorCategory::SessionExpired);
  CHECK(from_envelope.action == SuggestedAction::ReEstablishSession);
  CHECK(from_envelope.broker_code.find("stCode=10502") != std::string::npos);
}

TEST_CASE("429 maps to RateLimited / RetrySafe and surfaces Retry-After", "[kotak][errors]") {
  const auto error = map_kotak_error(
      response(429, kFaultThrottled, {{"Retry-After", "3"}, {"X-RateLimit-Remaining", "0"}}));
  CHECK(error.category == ErrorCategory::RateLimited);
  CHECK(error.action == SuggestedAction::RetrySafe);
  CHECK(error.broker_code.find("Retry-After=3") != std::string::npos);
  CHECK(error.broker_code.find("stCode=900802") != std::string::npos);
}

TEST_CASE("5xx NEVER maps to DoNotRetry — the order may be live", "[kotak][errors]") {
  for (const long status : {500L, 502L, 503L, 504L}) {
    const auto error = map_kotak_error(response(status, kFaultServerError));
    INFO("status " << status);
    CHECK(error.category == ErrorCategory::Network);
    CHECK(error.action == SuggestedAction::ReconcileFirst);
    CHECK(error.action != SuggestedAction::DoNotRetry);
  }
}

TEST_CASE("a synthesized response with no status reconciles first", "[kotak][errors]") {
  const auto error = map_kotak_error(response(0, ""));
  CHECK(error.action == SuggestedAction::ReconcileFirst);
}

TEST_CASE("the 5xx guard OUTRANKS misleading body phrasing", "[kotak][errors]") {
  // Both of these previously escaped through a phrasing branch before the status
  // was ever considered. Each escape is an order-safety bug in its own right.

  SECTION("a 503 whose body says the token expired must NOT demand a re-auth") {
    // Re-establishing a session abandons the in-flight order: whatever the
    // gateway says, the place may have reached the exchange.
    const auto error = map_kotak_error(response(503, kServerErrorSayingTokenExpired));
    CHECK(error.category == ErrorCategory::Network);
    CHECK(error.action == SuggestedAction::ReconcileFirst);
    CHECK(error.action != SuggestedAction::ReEstablishSession);
    CHECK(error.category != ErrorCategory::SessionExpired);
  }

  SECTION("a 502 whose body says throttled must NOT read as safe-to-retry") {
    // RetrySafe on a possibly-delivered place is a DOUBLE ORDER.
    const auto error = map_kotak_error(response(502, kFaultThrottled));
    CHECK(error.category == ErrorCategory::Network);
    CHECK(error.action == SuggestedAction::ReconcileFirst);
    CHECK(error.action != SuggestedAction::RetrySafe);
  }

  SECTION("a status-less transport failure with session phrasing still reconciles") {
    const auto error = map_kotak_error(response(0, kServerErrorSayingTokenExpired));
    CHECK(error.action == SuggestedAction::ReconcileFirst);
  }
}

TEST_CASE("a 5xx is never reported as session death", "[kotak][errors]") {
  // Otherwise a broker outage would demand a spurious operator re-auth and mask
  // the reconcile the outage actually calls for.
  CHECK_FALSE(is_session_death(response(503, kServerErrorSayingTokenExpired)));
  CHECK_FALSE(is_session_death(response(0, kServerErrorSayingTokenExpired)));
}

TEST_CASE("a 404 reconciles rather than abandoning a possibly-live order", "[kotak][errors]") {
  // Our endpoint paths are an UNVERIFIED tier-2 assumption, so a 404 on a
  // cancel/modify may mean "wrong URL", not "no such order". DoNotRetry here
  // would silently strand a live order.
  const auto error = map_kotak_error(response(404, ""));
  CHECK(error.category == ErrorCategory::OrderNotFound);
  CHECK(error.action == SuggestedAction::ReconcileFirst);
  CHECK(error.action != SuggestedAction::DoNotRetry);
}

TEST_CASE("a bare 'token' in a reject is NOT a session verdict", "[kotak][errors]") {
  // The canonical classifier matches a bare "token", so "Invalid instrument
  // token" would otherwise masquerade as a dead session — forcing a pointless
  // re-auth and, from validate(), a false NeedsReauth on a healthy session.
  const auto at_200 = map_kotak_error(response(200, kNotOkInstrumentToken));
  CHECK(at_200.category != ErrorCategory::SessionExpired);
  CHECK(at_200.action != SuggestedAction::ReEstablishSession);
  CHECK(at_200.category == ErrorCategory::BrokerRejected);  // fail closed
  CHECK(at_200.action == SuggestedAction::ReconcileFirst);

  const auto at_400 = map_kotak_error(response(400, kNotOkInstrumentToken));
  CHECK(at_400.category == ErrorCategory::Validation);
  CHECK(at_400.action == SuggestedAction::DoNotRetry);

  CHECK_FALSE(is_session_death(response(200, kNotOkInstrumentToken)));

  // The genuine article still resolves to session death, on the co-phrasing.
  CHECK(is_session_death(response(200, kNotOkSessionDead)));
}

TEST_CASE("a margin reject maps to InsufficientFunds via the canonical classifier",
          "[kotak][errors]") {
  const auto error = map_kotak_error(response(200, kNotOkMargin));
  CHECK(error.category == ErrorCategory::InsufficientFunds);
  CHECK(error.action == SuggestedAction::DoNotRetry);
}

TEST_CASE("a 400 client reject maps to Validation / DoNotRetry", "[kotak][errors]") {
  const auto error = map_kotak_error(response(400, kNotOkInvalidQty));
  CHECK(error.category == ErrorCategory::Validation);
  CHECK(error.action == SuggestedAction::DoNotRetry);
}

TEST_CASE("an unclassifiable 200 rejection fails CLOSED to reconcile-first", "[kotak][errors]") {
  const auto error = map_kotak_error(response(200, kNotOkUnclassifiable));
  CHECK(error.category == ErrorCategory::BrokerRejected);
  CHECK(error.action == SuggestedAction::ReconcileFirst);
}

TEST_CASE("no credential survives into an Error, even when the broker echoes it",
          "[kotak][errors][scrub]") {
  // The broker echoes back a token-shaped sid, an access token and an MPIN.
  constexpr const char* kSid = "sid9f2b7c1d4e8a0b3c5d7e9f11";
  constexpr const char* kToken = "authTOKEN0000ZZZZ9999YYYY8888";
  const std::string body = std::string(R"({"stat":"Not_Ok","errMsg":"call failed for sid )") +
                           kSid + " token " + kToken + R"( mpin 4321","stCode":5203})";

  const auto error = map_kotak_error(response(400, body));

  CHECK(error.message.find(kSid) == std::string::npos);
  CHECK(error.message.find(kToken) == std::string::npos);
  CHECK(error.message.find("4321") == std::string::npos);
  CHECK(error.broker_code.find(kSid) == std::string::npos);
  CHECK(error.broker_code.find(kToken) == std::string::npos);
  // The prose survives; only the values are gone.
  CHECK(error.message.find("REDACTED") != std::string::npos);
}

TEST_CASE("a SHORT all-digit sid is redacted, which domain::scrub alone cannot do",
          "[kotak][errors][scrub]") {
  // domain::scrub redacts high-entropy runs (>=20 mixed alnum) and values behind
  // its own denylisted keys — but `sid` is not on that denylist and a real Kotak
  // sid can be a 6-digit run. Without the Kotak-specific pre-redaction pass this
  // walks straight into the Error message. (The longer fixtures elsewhere pass
  // on the entropy rule alone and would NOT have caught this.)
  constexpr const char* kShortSid = "884213";

  SECTION("space-separated, the way Kotak phrases it in prose") {
    const auto error = map_kotak_error(
        response(400, R"({"stat":"Not_Ok","errMsg":"invalid sid 884213","stCode":5001})"));
    CHECK(error.message.find(kShortSid) == std::string::npos);
    CHECK(error.message.find("REDACTED") != std::string::npos);
  }

  SECTION("as an embedded key:value pair") {
    const auto error = map_kotak_error(
        response(400, R"({"stat":"Not_Ok","errMsg":"rejected for sid=884213","stCode":5001})"));
    CHECK(error.message.find(kShortSid) == std::string::npos);
  }

  SECTION("an Auth artifact too") {
    const auto error = map_kotak_error(
        response(400, R"({"stat":"Not_Ok","errMsg":"bad Auth: 884213 supplied","stCode":5001})"));
    CHECK(error.message.find(kShortSid) == std::string::npos);
  }

  SECTION("ordinary prose after the keyword still survives for diagnostics") {
    const auto error = map_kotak_error(
        response(400, R"({"stat":"Not_Ok","errMsg":"auth failed for this user","stCode":5001})"));
    CHECK(error.message.find("failed") != std::string::npos);
  }
}

TEST_CASE("a hostile stCode cannot smuggle a token through broker_code", "[kotak][errors][scrub]") {
  constexpr const char* kLeak = "leakedTOKEN1111AAAA2222BBBB";
  const std::string body =
      std::string(R"({"stat":"Not_Ok","errMsg":"nope","stCode":")") + kLeak + R"("})";

  const auto error = map_kotak_error(response(400, body));
  CHECK(error.broker_code.find(kLeak) == std::string::npos);
}
