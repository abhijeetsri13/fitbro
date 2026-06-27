#include "broker_exec/observability/audit_event.hpp"
#include "broker_exec/observability/audit_trail.hpp"
#include "broker_exec/observability/structured_logger.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
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
