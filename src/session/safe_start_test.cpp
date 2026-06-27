#include "broker_exec/session/safe_start.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <system_error>
#include <utility>

#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/refdata/instrument_master.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/session/session_state.hpp"

using broker_exec::Result;
using broker_exec::clock::TestClock;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::make_error;
using broker_exec::errors::SuggestedAction;
using broker_exec::refdata::InstrumentMaster;
using broker_exec::session::SafeCheck;
using broker_exec::session::SafeStartContext;
using broker_exec::session::SafeStartGate;
using broker_exec::session::session_state_to_result;
using broker_exec::session::SessionState;

namespace {

// A check that always passes.
const SafeCheck ok_check = [] { return broker_exec::ports::ok(); };

// A check that always fails with the given category/message.
[[nodiscard]] SafeCheck fail_check(ErrorCategory category, std::string msg) {
  return [category, msg = std::move(msg)]() -> Result<broker_exec::ports::Ok> {
    return broker_exec::fail(make_error(category, msg));
  };
}

// A context with all eight checks wired to a passing check.
[[nodiscard]] SafeStartContext make_all_passing() {
  SafeStartContext ctx;
  ctx.config_check = ok_check;
  ctx.crypto_keys_check = ok_check;
  ctx.clock_check = ok_check;
  ctx.session_check = ok_check;
  ctx.egress_ip_check = ok_check;
  ctx.instrument_master_check = ok_check;
  ctx.calendar_check = ok_check;
  ctx.reconciliation_check = ok_check;
  return ctx;
}

// The eight checks, in verify()'s fixed order, with the name that appears in the
// Error message and a pointer-to-member to address the field generically.
struct CheckEntry {
  const char* name;
  SafeCheck SafeStartContext::*field;
};

const std::array<CheckEntry, 8> kChecks = {{
    {"config", &SafeStartContext::config_check},
    {"crypto-keys", &SafeStartContext::crypto_keys_check},
    {"clock", &SafeStartContext::clock_check},
    {"session", &SafeStartContext::session_check},
    {"egress-IP", &SafeStartContext::egress_ip_check},
    {"instrument-master", &SafeStartContext::instrument_master_check},
    {"calendar", &SafeStartContext::calendar_check},
    {"reconciliation", &SafeStartContext::reconciliation_check},
}};

// A unique temp directory, cleaned up on destruction (RAII).
struct TempDir {
  std::filesystem::path path;
  TempDir() {
    std::random_device rd;
    path = std::filesystem::temp_directory_path() /
           ("brexec_safestart_" + std::to_string(rd()) + "_" + std::to_string(rd()));
    std::filesystem::create_directories(path);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

[[nodiscard]] std::chrono::system_clock::time_point wall_on(int year, unsigned month, unsigned day) {
  return std::chrono::system_clock::time_point(std::chrono::sys_days{
      std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day}});
}

constexpr const char* kCsv =
    "instrument_token,tradingsymbol,expiry,tick_size,lot_size,exchange\n"
    "256265,NIFTY26JUL24000CE,2026-07-30,0.05,75,NFO\n";

}  // namespace

TEST_CASE("all eight checks pass -> verify() allows trading", "[session][safe-start][AC1]") {
  const SafeStartGate gate;
  CHECK(gate.verify(make_all_passing()).has_value());
}

TEST_CASE("each check fails in isolation -> a named Error preserving the inner category",
          "[session][safe-start][AC2]") {
  const SafeStartGate gate;
  for (const CheckEntry& entry : kChecks) {
    SafeStartContext ctx = make_all_passing();
    // Use DataStale so we can assert the wrapped inner category is preserved.
    ctx.*(entry.field) = fail_check(ErrorCategory::DataStale, "inner detail");

    const Result<broker_exec::ports::Ok> r = gate.verify(ctx);
    REQUIRE_FALSE(r.has_value());
    INFO("check = " << entry.name);
    CHECK(r.error().message.find(std::string("safe-start: ") + entry.name) != std::string::npos);
    // Inner category/action preserved through the wrap.
    CHECK(r.error().category == ErrorCategory::DataStale);
    CHECK(r.error().action == SuggestedAction::BlockStrategy);
    CHECK(r.error().message.find("inner detail") != std::string::npos);
  }
}

TEST_CASE("an UNSET check fails closed -> verify() blocks naming it not configured",
          "[session][safe-start][AC2]") {
  const SafeStartGate gate;
  for (const CheckEntry& entry : kChecks) {
    SafeStartContext ctx = make_all_passing();
    ctx.*(entry.field) = SafeCheck{};  // empty std::function

    const Result<broker_exec::ports::Ok> r = gate.verify(ctx);
    REQUIRE_FALSE(r.has_value());
    INFO("check = " << entry.name);
    CHECK(r.error().message.find(entry.name) != std::string::npos);
    CHECK(r.error().message.find("not configured") != std::string::npos);
    // Fail-closed: an unconfigured gate must HALT, not merely alert.
    CHECK(r.error().category == ErrorCategory::Internal);
    CHECK(r.error().action == SuggestedAction::BlockStrategy);
  }
}

TEST_CASE("ordering: the FIRST failing check in the fixed order is named",
          "[session][safe-start][AC2]") {
  const SafeStartGate gate;
  SafeStartContext ctx = make_all_passing();
  // Both config (first) and reconciliation (last) fail; config must win.
  ctx.config_check = fail_check(ErrorCategory::Validation, "bad config");
  ctx.reconciliation_check = fail_check(ErrorCategory::DataStale, "not reconciled");

  const Result<broker_exec::ports::Ok> r = gate.verify(ctx);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().message.find("safe-start: config") != std::string::npos);
  CHECK(r.error().message.find("reconciliation") == std::string::npos);
}

TEST_CASE("session_state_to_result maps health to ok and reauth/failed to SessionExpired",
          "[session][safe-start][AC1][AC3]") {
  CHECK(session_state_to_result(SessionState::Healthy).has_value());

  const Result<broker_exec::ports::Ok> reauth = session_state_to_result(SessionState::NeedsReauth);
  REQUIRE_FALSE(reauth.has_value());
  CHECK(reauth.error().category == ErrorCategory::SessionExpired);
  CHECK(reauth.error().action == SuggestedAction::ReEstablishSession);

  const Result<broker_exec::ports::Ok> failed = session_state_to_result(SessionState::Failed);
  REQUIRE_FALSE(failed.has_value());
  CHECK(failed.error().category == ErrorCategory::SessionExpired);
}

TEST_CASE("a healthy session check contributes a pass", "[session][safe-start][AC1]") {
  const SafeStartGate gate;
  SafeStartContext ctx = make_all_passing();
  ctx.session_check = [] { return session_state_to_result(SessionState::Healthy); };
  CHECK(gate.verify(ctx).has_value());
}

TEST_CASE("a NeedsReauth session check blocks the start (SessionExpired)",
          "[session][safe-start][AC2]") {
  const SafeStartGate gate;
  SafeStartContext ctx = make_all_passing();
  ctx.session_check = [] { return session_state_to_result(SessionState::NeedsReauth); };

  const Result<broker_exec::ports::Ok> r = gate.verify(ctx);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().category == ErrorCategory::SessionExpired);
  CHECK(r.error().message.find("safe-start: session") != std::string::npos);
}

TEST_CASE("AC-3: a REAL never-refreshed InstrumentMaster blocks the start (DataStale)",
          "[session][safe-start][AC3]") {
  TempDir dir;
  TestClock clock(std::chrono::steady_clock::time_point{}, wall_on(2026, 6, 28));
  auto fetcher = [] { return Result<std::string>(std::string(kCsv)); };
  InstrumentMaster im(fetcher, clock, dir.path, "kite", "NFO");
  // Deliberately do NOT refresh -> require_fresh() is stale (DataStale).

  const SafeStartGate gate;
  SafeStartContext ctx = make_all_passing();
  ctx.instrument_master_check = [&im] { return im.require_fresh(); };

  const Result<broker_exec::ports::Ok> r = gate.verify(ctx);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().category == ErrorCategory::DataStale);
  CHECK(r.error().action == SuggestedAction::BlockStrategy);
  CHECK(r.error().message.find("safe-start: instrument-master") != std::string::npos);

  // Sanity: after a same-day refresh the master is fresh and the start passes.
  REQUIRE(im.refresh().has_value());
  CHECK(gate.verify(ctx).has_value());
}

TEST_CASE("AC-3: a forced egress-IP mismatch blocks the start naming egress-IP",
          "[session][safe-start][AC3]") {
  const SafeStartGate gate;
  SafeStartContext ctx = make_all_passing();
  ctx.egress_ip_check = fail_check(ErrorCategory::Validation, "egress IP mismatch");

  const Result<broker_exec::ports::Ok> r = gate.verify(ctx);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().category == ErrorCategory::Validation);
  CHECK(r.error().message.find("safe-start: egress-IP") != std::string::npos);
  CHECK(r.error().message.find("egress IP mismatch") != std::string::npos);
}
