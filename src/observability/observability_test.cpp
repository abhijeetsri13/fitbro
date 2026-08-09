#include "broker_exec/observability/audit_event.hpp"
#include "broker_exec/observability/audit_trail.hpp"
#include "broker_exec/observability/structured_logger.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/details/log_msg.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/base_sink.h>

#include "broker_exec/clock/test_clock.hpp"

using broker_exec::clock::TestClock;
using broker_exec::observability::AuditEvent;
using broker_exec::observability::AuditTrail;
using broker_exec::observability::EventType;
using broker_exec::observability::StructuredLogger;
using broker_exec::observability::to_json_line;
using json = nlohmann::json;

namespace {

// A spdlog sink that captures each formatted message (and its level) into an
// in-memory vector, so a test can inspect the exact bytes emitted WITHOUT a
// file. The logger's pattern is "%v" (see make_logger), so msg.payload is our
// JSON line verbatim.
class CapturingSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  struct Line {
    spdlog::level::level_enum level;
    std::string text;
  };

  [[nodiscard]] std::vector<Line> lines() {
    std::lock_guard<std::mutex> lock(this->mutex_);
    return lines_;
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    lines_.push_back(Line{msg.level, std::string(msg.payload.data(), msg.payload.size())});
  }
  void flush_() override {}

 private:
  std::vector<Line> lines_;
};

// A spdlog sink whose sink_it_ ALWAYS throws, modelling a misbehaving sink
// (full disk, broken pipe). StructuredLogger::log must swallow this so nothing
// propagates across the no-throw boundary.
class ThrowingSink final : public spdlog::sinks::base_sink<std::mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg& /*msg*/) override {
    throw std::runtime_error("sink down");
  }
  void flush_() override {}
};

// A synthetic, token-shaped Kite access_token: a 32-char alnum run mixing
// letters AND digits, so domain::scrub's bare-high-entropy rule redacts it
// wherever it appears. NOT a live credential.
constexpr const char* kSyntheticToken = "abcd1234EFGH5678ijkl9012MNOP3456";

// A real-shaped client_ref EXACTLY as idempotency::make_client_ref mints it:
// `<strategy>-<8 hex signature>-<uuid>`, where <uuid> is the CANONICAL RFC-4122
// text form 8-4-4-4-12 WITH its dashes (idempotency/uuid.cpp format_uuid_v4) —
// 51 chars, seven alphanumeric segments. Before IMP-15 this rendered as
// `"client_ref":"***REDACTED***"` on EVERY audit line. The regression baseline
// must pin the shape the library really mints, not a simplification of it.
constexpr const char* kClientRef = "alpha-1a2b3c4d-deadbeef-cafe-4bab-8abe-0123456789ab";

// The dashless 32-hex-tail spelling, kept as an ADDITIONAL case: a caller may
// hand make_client_ref any uuid text, so both shapes must behave identically.
constexpr const char* kDashlessClientRef = "alpha-1a2b3c4d-deadbeefcafebabe0123456789abcdef";

// A Kite-shaped broker order id: 15 digits.
constexpr const char* kBrokerOrderId = "240627000123456";

// Log `ev` through a capturing sink and return the ONE emitted line, parsed.
[[nodiscard]] std::string logged_text(const AuditEvent& ev) {
  auto sink = std::make_shared<CapturingSink>();
  auto logger = StructuredLogger::make_logger(sink);
  TestClock clock;
  StructuredLogger structured(logger, clock);
  structured.log(ev);
  const auto lines = sink->lines();
  return lines.size() == 1 ? lines.front().text : std::string{};
}

}  // namespace

TEST_CASE("to_json_line emits a versioned schema with stable type and ISO ts (AC-3)") {
  AuditEvent ev;
  ev.type = EventType::OrderPlaced;
  ev.ts = std::chrono::sys_days{std::chrono::year{2026} / 6 / 27} + std::chrono::hours{9} +
          std::chrono::minutes{15} + std::chrono::seconds{0};
  ev.strategy = "alpha";
  ev.broker = "kite";
  ev.account = "ZZ1234";
  ev.client_ref = "alpha-1";
  ev.broker_order_id = "240627000123456";

  const json parsed = json::parse(to_json_line(ev), nullptr, /*allow_exceptions=*/false);
  REQUIRE_FALSE(parsed.is_discarded());
  REQUIRE(parsed.is_object());

  CHECK(parsed.at("schema_version").get<int>() == AuditEvent::kSchemaVersion);
  CHECK(parsed.at("schema_version").get<int>() == 1);
  CHECK(parsed.at("type").get<std::string>() == "order.placed");  // STABLE name

  REQUIRE(parsed.contains("ts"));
  const auto ts = parsed.at("ts").get<std::string>();
  CHECK_FALSE(ts.empty());
  CHECK(ts == "2026-06-27T09:15:00Z");

  CHECK(parsed.at("strategy").get<std::string>() == "alpha");
  CHECK(parsed.at("broker").get<std::string>() == "kite");
  CHECK(parsed.at("account").get<std::string>() == "ZZ1234");
  CHECK(parsed.at("client_ref").get<std::string>() == "alpha-1");
  CHECK(parsed.at("broker_order_id").get<std::string>() == "240627000123456");
}

TEST_CASE("stable EventType names cover the observability contract") {
  CHECK(to_json_line(AuditEvent{.type = EventType::OrderAcknowledged}).find("order.acknowledged") !=
        std::string::npos);
  CHECK(to_json_line(AuditEvent{.type = EventType::OrderFilled}).find("order.filled") !=
        std::string::npos);
  CHECK(to_json_line(AuditEvent{.type = EventType::RiskResult}).find("risk.result") !=
        std::string::npos);
  CHECK(to_json_line(AuditEvent{.type = EventType::ReconcileResult}).find("reconcile.result") !=
        std::string::npos);
  CHECK(to_json_line(AuditEvent{.type = EventType::Error}).find("\"error\"") != std::string::npos);
  CHECK(to_json_line(AuditEvent{.type = EventType::Override}).find("override") != std::string::npos);
}

TEST_CASE("full provenance round-trips into the JSON line (AC-1)") {
  AuditEvent ev;
  ev.type = EventType::OrderFilled;
  ev.ts = std::chrono::sys_days{std::chrono::year{2026} / 6 / 27};
  ev.strategy = "alpha";
  ev.broker = "kite";
  ev.account = "ZZ1234";
  ev.client_ref = "alpha-1";
  ev.broker_order_id = "240627000123456";
  ev.fields = json{
      {"status", "COMPLETE"},
      {"risk_result", "passed"},
      {"reconcile_result", "matched"},
      {"pnl", 125000},  // integer paise — NEVER a float
      {"error", ""},
      {"override", false},
  };

  const json parsed = json::parse(to_json_line(ev), nullptr, /*allow_exceptions=*/false);
  REQUIRE_FALSE(parsed.is_discarded());

  // Typed columns at top level.
  CHECK(parsed.at("strategy").get<std::string>() == "alpha");
  CHECK(parsed.at("broker").get<std::string>() == "kite");
  CHECK(parsed.at("account").get<std::string>() == "ZZ1234");
  CHECK(parsed.at("client_ref").get<std::string>() == "alpha-1");
  CHECK(parsed.at("broker_order_id").get<std::string>() == "240627000123456");

  // The rest under a namespaced "fields" object.
  REQUIRE(parsed.contains("fields"));
  const json& f = parsed.at("fields");
  CHECK(f.at("status").get<std::string>() == "COMPLETE");
  CHECK(f.at("risk_result").get<std::string>() == "passed");
  CHECK(f.at("reconcile_result").get<std::string>() == "matched");
  CHECK(f.at("pnl").get<long long>() == 125000);
  CHECK(f.at("pnl").is_number_integer());  // paise stayed an integer, not a double
  CHECK(f.contains("error"));
  CHECK(f.at("override").get<bool>() == false);
}

TEST_CASE("the redaction binding scrubs a token from EVERY field (AC-1 crux)") {
  auto sink = std::make_shared<CapturingSink>();
  auto logger = StructuredLogger::make_logger(sink);
  TestClock clock;
  StructuredLogger structured(logger, clock);

  AuditEvent ev;
  ev.type = EventType::OrderPlaced;
  ev.client_ref = "alpha-1";
  ev.broker_order_id = kSyntheticToken;  // token in a TYPED column
  ev.fields = json{{"access_token", kSyntheticToken}};  // ... and in fields

  structured.log(ev);

  const auto lines = sink->lines();
  REQUIRE(lines.size() == 1);
  // ZERO occurrences of the token anywhere in the emitted line.
  CHECK(lines.front().text.find(kSyntheticToken) == std::string::npos);
  // And it must actually have rendered (and redacted, not just dropped).
  CHECK(lines.front().text.find("REDACTED") != std::string::npos);

  // The SCRUBBED line must still be valid JSON: scrub replaced the token without
  // mangling the structure (not just that the token is gone).
  const json parsed = json::parse(lines.front().text, nullptr, /*allow_exceptions=*/false);
  REQUIRE_FALSE(parsed.is_discarded());
  CHECK(parsed.at("schema_version").get<int>() == 1);
  CHECK(parsed.at("type").get<std::string>() == "order.placed");  // STABLE name
}

// ── IMP-15: provenance IDs survive redaction ─────────────────────────────────

TEST_CASE("a real client_ref and broker order id survive the logged line INTACT (IMP-15)") {
  AuditEvent ev;
  ev.type = EventType::OrderPlaced;
  ev.strategy = "alpha";
  ev.broker = "kite";
  ev.account = "ZZ1234";
  ev.client_ref = kClientRef;
  ev.broker_order_id = kBrokerOrderId;

  const std::string text = logged_text(ev);
  REQUIRE_FALSE(text.empty());

  // The operator must be able to grep the raw line for the ref they hold.
  CHECK(text.find(kClientRef) != std::string::npos);

  const json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
  REQUIRE_FALSE(parsed.is_discarded());
  CHECK(parsed.at("client_ref").get<std::string>() == kClientRef);
  CHECK(parsed.at("broker_order_id").get<std::string>() == kBrokerOrderId);
  CHECK(parsed.at("strategy").get<std::string>() == "alpha");
  CHECK(parsed.at("broker").get<std::string>() == "kite");
  CHECK(parsed.at("account").get<std::string>() == "ZZ1234");
  // Nothing was redacted at all: there was no secret-shaped content anywhere.
  CHECK(text.find("REDACTED") == std::string::npos);
}

TEST_CASE("both uuid spellings of a client_ref survive the logged line INTACT (IMP-15)") {
  // The canonical 8-4-4-4-12 uuid is what uuid.cpp actually mints; the dashless
  // 32-hex tail is the other spelling a caller could pass. Behaviour is identical.
  for (const char* ref : {kClientRef, kDashlessClientRef}) {
    AuditEvent ev;
    ev.type = EventType::OrderPlaced;
    ev.client_ref = ref;

    const std::string text = logged_text(ev);
    REQUIRE_FALSE(text.empty());
    const json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
    REQUIRE_FALSE(parsed.is_discarded());
    CHECK(parsed.at("client_ref").get<std::string>() == ref);
    CHECK(text.find("REDACTED") == std::string::npos);
  }
}

TEST_CASE("an IMP-13 exit ref and a slicer child ref survive the logged line INTACT (IMP-15)") {
  const std::string exit_ref = std::string(kClientRef) + "#X";   // square-off exit
  const std::string child_ref = std::string(kClientRef) + "#3";  // freeze-slicer child

  for (const std::string& ref : {exit_ref, child_ref}) {
    AuditEvent ev;
    ev.type = EventType::OrderPlaced;
    ev.client_ref = ref;

    const std::string text = logged_text(ev);
    REQUIRE_FALSE(text.empty());
    const json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
    REQUIRE_FALSE(parsed.is_discarded());
    CHECK(parsed.at("client_ref").get<std::string>() == ref);
  }
}

TEST_CASE("a token-shaped value in a TYPED column is still redacted (IMP-15 fail-closed)") {
  // The exemption is a SHAPE allowlist, not a per-column pass: a credential run
  // is heterogeneous (digits mixed with non-hex letters), so it is redacted even
  // sitting in client_ref — WITH OR WITHOUT an embedded '-'/'_'. The separated
  // spelling is the base64url case that the segment count alone let through.
  AuditEvent ev;
  ev.type = EventType::OrderPlaced;
  ev.client_ref = kSyntheticToken;                      // a token in the id column
  ev.broker_order_id = kSyntheticToken;                 // ... and in the other id column
  ev.account = std::string("access_token=") + kSyntheticToken;  // ... and as a pasted pair
  ev.strategy = "v4Xk29mZpQ7rTb-4Lw8Nc1Vd6Ya3Hs0Ue5";   // ... and URL-safe, with a '-'
  ev.broker = "token_Ab12Cd34Ef56Gh78Ij90Kl12";         // ... and with a '_'

  const std::string text = logged_text(ev);
  REQUIRE_FALSE(text.empty());
  CHECK(text.find(kSyntheticToken) == std::string::npos);
  CHECK(text.find("REDACTED") != std::string::npos);

  // The base64url probes: NOT emitted, in whole or in part, from a typed column.
  CHECK(text.find("v4Xk29mZpQ7rTb") == std::string::npos);
  CHECK(text.find("4Lw8Nc1Vd6Ya3Hs0Ue5") == std::string::npos);
  CHECK(text.find("Ab12Cd34Ef56Gh78Ij90Kl12") == std::string::npos);

  const json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
  REQUIRE_FALSE(parsed.is_discarded());
  CHECK(parsed.at("schema_version").get<int>() == 1);
  CHECK(parsed.at("strategy").get<std::string>() == "***REDACTED***");
  CHECK(parsed.at("broker").get<std::string>() == "***REDACTED***");
}

TEST_CASE("`fields` keeps FULL scrubbing while the typed column survives (IMP-15)") {
  AuditEvent ev;
  ev.type = EventType::Error;
  ev.client_ref = kClientRef;  // typed column: exempt
  ev.fields = json{
      {"access_token", kSyntheticToken},  // key=value rule
      {"blob", kSyntheticToken},          // bare high-entropy rule
      {"echoed_ref", kClientRef},         // free-form text is NOT exempt, even if it
                                          // happens to hold a copy of the ref
      {"note", "user entered MPIN 4321 at the prompt"},  // MPIN auth-context rule
  };

  const std::string text = logged_text(ev);
  REQUIRE_FALSE(text.empty());
  const json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
  REQUIRE_FALSE(parsed.is_discarded());

  // The typed column is legible...
  CHECK(parsed.at("client_ref").get<std::string>() == kClientRef);

  // ...and every free-form rule still fires exactly as it did before.
  const json& f = parsed.at("fields");
  CHECK(f.at("access_token").get<std::string>().find(kSyntheticToken) == std::string::npos);
  CHECK(f.at("blob").get<std::string>().find(kSyntheticToken) == std::string::npos);
  CHECK(f.at("echoed_ref").get<std::string>() == "***REDACTED***");
  CHECK(f.at("note").get<std::string>().find("4321") == std::string::npos);
  // The token appears NOWHERE in the emitted bytes.
  CHECK(text.find(kSyntheticToken) == std::string::npos);
}

TEST_CASE("a planted sentinel cannot forge a provenance column (IMP-15 safety net)") {
  // A caller who plants a sentinel-looking literal in `fields` under a colliding
  // key must not be able to make the logger emit a WRONG id. The invariant is
  // unconditional and holds whichever path runs: the sentinels now carry a
  // per-process random tail, so the planted literal is simply not a sentinel and
  // the splice proceeds normally (see the anti-forensics case below); if a splice
  // ever DID miss, the safety net falls back to the legacy whole-line scrub —
  // a lost id beats a wrong one.
  AuditEvent ev;
  ev.type = EventType::OrderPlaced;
  ev.strategy = "alpha-beta";  // id-shaped -> would normally be substituted
  ev.client_ref = kClientRef;
  ev.fields = json{{"strategy", "PROVENANCESENTINELSTRATEGY"}};

  const std::string text = logged_text(ev);
  REQUIRE_FALSE(text.empty());
  const json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
  REQUIRE_FALSE(parsed.is_discarded());
  // Whatever the fallback decided, the strategy column is never the sentinel.
  CHECK(parsed.at("strategy").get<std::string>() != "PROVENANCESENTINELSTRATEGY");
}

TEST_CASE("a planted sentinel LITERAL cannot suppress the splice (IMP-15 anti-forensics)") {
  // The sentinels used to be compile-time CONSTANTS, so caller text that merely
  // CONTAINED one tripped the safety net and threw the whole splice away —
  // silently stripping provenance from that line, with nothing marking the
  // fallback. Repeated deliberately, that lets an attacker choose which orders
  // become un-correlatable in the audit trail (anti-forensics). The sentinels now
  // carry a per-process random tail, so the guessable prefix is inert.
  AuditEvent ev;
  ev.type = EventType::OrderRejected;
  ev.strategy = "alpha-beta";  // id-shaped -> substituted, then spliced back
  ev.client_ref = kClientRef;
  ev.broker_order_id = kBrokerOrderId;
  ev.fields = json{
      {"reason", "rejected: PROVENANCESENTINELCLIENTREF"},
      {"strategy", "PROVENANCESENTINELSTRATEGY"},  // ... and under a COLLIDING key
      {"note", "PROVENANCESENTINELORDERID PROVENANCESENTINELACCOUNT PROVENANCESENTINELBROKER"},
  };

  const std::string text = logged_text(ev);
  REQUIRE_FALSE(text.empty());
  const json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
  REQUIRE_FALSE(parsed.is_discarded());

  // EVERY provenance column survived: the planted literals bought nothing.
  CHECK(parsed.at("client_ref").get<std::string>() == kClientRef);
  CHECK(parsed.at("strategy").get<std::string>() == "alpha-beta");
  CHECK(parsed.at("broker_order_id").get<std::string>() == kBrokerOrderId);
  // The caller's own text is still there, untouched and harmless.
  CHECK(parsed.at("fields").at("reason").get<std::string>() ==
        "rejected: PROVENANCESENTINELCLIENTREF");
  // Nothing was redacted: no secret-shaped content anywhere in the line.
  CHECK(text.find("REDACTED") == std::string::npos);
}

TEST_CASE("the decision path is legible end to end: every line names the same ref (IMP-15)") {
  auto sink = std::make_shared<CapturingSink>();
  auto logger = StructuredLogger::make_logger(sink);
  logger->set_level(spdlog::level::trace);
  TestClock clock;
  StructuredLogger structured(logger, clock);
  AuditTrail trail;

  const auto step = [](EventType type, const std::string& ref) {
    AuditEvent ev;
    ev.type = type;
    ev.client_ref = ref;
    ev.broker_order_id = kBrokerOrderId;
    return ev;
  };

  for (const EventType type : {EventType::OrderPlaced, EventType::RiskResult,
                               EventType::OrderAcknowledged, EventType::OrderFilled,
                               EventType::ReconcileResult}) {
    trail.record_and_log(step(type, kClientRef), structured);
  }
  // A second order's steps must not bleed into the first order's path.
  const std::string other = std::string(kClientRef) + "#X";
  trail.record_and_log(step(EventType::OrderPlaced, other), structured);

  // (1) The in-memory reconstruction still keys off the raw ref.
  CHECK(trail.decision_path(kClientRef).size() == 5);
  CHECK(trail.decision_path(other).size() == 1);

  // (2) And the LOGGED bytes carry the same ref, so the same path can be
  // rebuilt by an operator grepping the log — which is the point of FR-27.
  const auto lines = sink->lines();
  REQUIRE(lines.size() == 6);
  int matched = 0;
  for (const auto& line : lines) {
    const json parsed = json::parse(line.text, nullptr, /*allow_exceptions=*/false);
    REQUIRE_FALSE(parsed.is_discarded());
    if (parsed.at("client_ref").get<std::string>() == kClientRef) {
      ++matched;
    }
    CHECK(parsed.at("broker_order_id").get<std::string>() == kBrokerOrderId);
  }
  CHECK(matched == 5);
  // The exit ref is legible too, and is NOT mistaken for its parent.
  const json last = json::parse(lines.back().text, nullptr, /*allow_exceptions=*/false);
  REQUIRE_FALSE(last.is_discarded());
  CHECK(last.at("client_ref").get<std::string>() == other);
}

TEST_CASE("log() swallows a throwing sink — never throws across the boundary") {
  auto sink = std::make_shared<ThrowingSink>();
  auto logger = StructuredLogger::make_logger(sink);
  TestClock clock;
  StructuredLogger structured(logger, clock);

  AuditEvent ev;
  ev.type = EventType::OrderPlaced;
  ev.client_ref = "alpha-1";

  // The sink throws on every emit; log() must contain it.
  REQUIRE_NOTHROW(structured.log(ev));
}

TEST_CASE("log() on a null logger is a no-op and never throws") {
  TestClock clock;
  StructuredLogger structured(nullptr, clock);

  AuditEvent ev;
  ev.type = EventType::OrderPlaced;
  ev.client_ref = "alpha-1";

  REQUIRE_NOTHROW(structured.log(ev));
}

TEST_CASE("invalid UTF-8 in a field does not throw and stays parseable JSON") {
  // Arbitrary broker/caller bytes (e.g. a Latin-1 reject reason) — invalid UTF-8
  // that a strict dump() would throw json::type_error.316 on.
  const std::string bad = std::string("bad\xff\xfe");

  AuditEvent ev;
  ev.type = EventType::Error;
  ev.client_ref = "alpha-1";
  ev.broker_order_id = bad;  // invalid UTF-8 in a TYPED column
  ev.fields = json{{"error", bad}};  // ... and in fields

  // The non-throwing UTF-8 handler keeps to_json_line total.
  REQUIRE_NOTHROW(to_json_line(ev));
  const json rendered = json::parse(to_json_line(ev), nullptr, /*allow_exceptions=*/false);
  REQUIRE_FALSE(rendered.is_discarded());
  CHECK(rendered.at("schema_version").get<int>() == 1);

  // And the same bytes flowing through log() must not derail the engine.
  auto sink = std::make_shared<CapturingSink>();
  auto logger = StructuredLogger::make_logger(sink);
  TestClock clock;
  StructuredLogger structured(logger, clock);
  REQUIRE_NOTHROW(structured.log(ev));

  const auto lines = sink->lines();
  REQUIRE(lines.size() == 1);
  const json parsed = json::parse(lines.front().text, nullptr, /*allow_exceptions=*/false);
  REQUIRE_FALSE(parsed.is_discarded());
  CHECK(parsed.at("schema_version").get<int>() == 1);
}

TEST_CASE("AuditTrail reconstructs the decision path in order (AC-2)") {
  AuditTrail trail;

  const auto make = [](EventType type, std::string client_ref) {
    AuditEvent ev;
    ev.type = type;
    ev.client_ref = std::move(client_ref);
    return ev;
  };

  trail.record(make(EventType::OrderPlaced, "alpha-1"));
  trail.record(make(EventType::RiskResult, "alpha-1"));
  trail.record(make(EventType::OrderAcknowledged, "alpha-1"));
  // A different order's events must not bleed in.
  trail.record(make(EventType::OrderPlaced, "beta-2"));
  trail.record(make(EventType::OrderFilled, "alpha-1"));

  const auto path = trail.decision_path("alpha-1");
  REQUIRE(path.size() == 4);
  CHECK(path[0].type == EventType::OrderPlaced);
  CHECK(path[1].type == EventType::RiskResult);
  CHECK(path[2].type == EventType::OrderAcknowledged);
  CHECK(path[3].type == EventType::OrderFilled);

  CHECK(trail.decision_path("beta-2").size() == 1);
  CHECK(trail.decision_path("does-not-exist").empty());
}

TEST_CASE("an empty ts is stamped from the injected clock") {
  auto sink = std::make_shared<CapturingSink>();
  auto logger = StructuredLogger::make_logger(sink);

  const auto pinned = std::chrono::sys_days{std::chrono::year{2026} / 6 / 27} +
                      std::chrono::hours{12} + std::chrono::minutes{34} + std::chrono::seconds{56};
  TestClock clock;
  clock.set_wall(pinned);
  StructuredLogger structured(logger, clock);

  AuditEvent ev;
  ev.type = EventType::OrderPlaced;
  ev.client_ref = "alpha-1";  // ts left empty -> stamped from the clock

  structured.log(ev);

  const auto lines = sink->lines();
  REQUIRE(lines.size() == 1);
  const json parsed = json::parse(lines.front().text, nullptr, /*allow_exceptions=*/false);
  REQUIRE_FALSE(parsed.is_discarded());
  CHECK(parsed.at("ts").get<std::string>() == "2026-06-27T12:34:56Z");
}

TEST_CASE("event type maps to the emission level") {
  auto sink = std::make_shared<CapturingSink>();
  auto logger = StructuredLogger::make_logger(sink);
  // Ensure even info passes the logger's level filter.
  logger->set_level(spdlog::level::trace);
  TestClock clock;
  StructuredLogger structured(logger, clock);

  AuditEvent err;
  err.type = EventType::Error;
  err.client_ref = "alpha-1";

  AuditEvent placed;
  placed.type = EventType::OrderPlaced;
  placed.client_ref = "alpha-1";

  AuditEvent override_ev;
  override_ev.type = EventType::Override;
  override_ev.client_ref = "alpha-1";

  structured.log(err);
  structured.log(placed);
  structured.log(override_ev);

  const auto lines = sink->lines();
  REQUIRE(lines.size() == 3);
  CHECK(lines[0].level == spdlog::level::err);
  CHECK(lines[1].level == spdlog::level::info);
  CHECK(lines[2].level == spdlog::level::warn);
}
