#include "broker_exec/session/safe_start.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/redaction.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/refdata/instrument_master.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/session/session_state.hpp"

using broker_exec::Result;
using broker_exec::clock::TestClock;
using broker_exec::domain::kMaxStrategyNameChars;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::make_error;
using broker_exec::errors::SuggestedAction;
using broker_exec::refdata::InstrumentMaster;
using broker_exec::session::require_valid_strategy_names;
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

// A context with all ten checks wired to a passing check.
[[nodiscard]] SafeStartContext make_all_passing() {
  SafeStartContext ctx;
  ctx.config_check = ok_check;
  ctx.strategy_name_check = ok_check;
  ctx.crypto_keys_check = ok_check;
  ctx.clock_check = ok_check;
  ctx.session_check = ok_check;
  ctx.egress_ip_check = ok_check;
  ctx.instrument_master_check = ok_check;
  ctx.calendar_check = ok_check;
  ctx.legacy_stop_check = ok_check;
  ctx.reconciliation_check = ok_check;
  return ctx;
}

// The ten checks, in verify()'s fixed order, with the name that appears in the
// Error message and a pointer-to-member to address the field generically.
struct CheckEntry {
  const char* name;
  SafeCheck SafeStartContext::*field;
};

// The number of checks in SafeStartContext, and in verify()'s fixed order.
constexpr std::size_t kCheckCount = 10;

const std::array<CheckEntry, kCheckCount> kChecks = {{
    {"config", &SafeStartContext::config_check},
    {"strategy-names", &SafeStartContext::strategy_name_check},
    {"crypto-keys", &SafeStartContext::crypto_keys_check},
    {"clock", &SafeStartContext::clock_check},
    {"session", &SafeStartContext::session_check},
    {"egress-IP", &SafeStartContext::egress_ip_check},
    {"instrument-master", &SafeStartContext::instrument_master_check},
    {"calendar", &SafeStartContext::calendar_check},
    {"legacy-stops", &SafeStartContext::legacy_stop_check},
    {"reconciliation", &SafeStartContext::reconciliation_check},
}};

// AN 11th CHECK MUST NOT SILENTLY ESCAPE THE GENERIC LOOPS BELOW. kChecks is what
// "every check" means in this file (each-fails-in-isolation, each-unset-blocks);
// a field added to SafeStartContext without a row here would simply never be
// tested, and nothing would say so. SafeStartContext is a struct of nothing but
// SafeCheck members, so its size is exactly that many of them — adding or removing
// one without updating kChecks breaks the build here, on the line that says why.
static_assert(sizeof(SafeStartContext) == kCheckCount * sizeof(SafeCheck),
              "SafeStartContext gained or lost a check: add/remove its row in kChecks (and its "
              "position in SafeStartGate::verify's fixed order) so the generic tests cover it");

// A projection row: an order of `type` in `state`, optionally armed.
[[nodiscard]] broker_exec::domain::Order order_row(std::string client_ref,
                                                   broker_exec::domain::OrderType type,
                                                   broker_exec::domain::OrderState state,
                                                   bool armed) {
  broker_exec::domain::Order o;
  o.intent.client_ref = std::move(client_ref);
  o.intent.symbol = "NIFTY26JUL24000CE";
  o.intent.quantity = broker_exec::domain::Quantity::of(50);
  o.intent.price = broker_exec::domain::Price::from_rupees(120);
  o.intent.order_type = type;
  if (armed) {
    // Buy-side ordering (limit >= trigger), so the row is a shape the gate accepts.
    o.intent.trigger_price = broker_exec::domain::Price::from_rupees(119);
  }
  o.state = state;
  return o;
}

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

[[nodiscard]] std::chrono::system_clock::time_point wall_on(int year, unsigned month,
                                                            unsigned day) {
  return std::chrono::system_clock::time_point(std::chrono::sys_days{
      std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day}});
}

constexpr const char* kCsv =
    "instrument_token,tradingsymbol,expiry,tick_size,lot_size,exchange\n"
    "256265,NIFTY26JUL24000CE,2026-07-30,0.05,75,NFO\n";

}  // namespace

TEST_CASE("all ten checks pass -> verify() allows trading", "[session][safe-start][AC1]") {
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

// ── IMP-11 HIGH-2: the legacy-stop cold-boot guard ───────────────────────────
//
// A stop placed by a pre-IMP-11 binary stored its activation level in `price`;
// this binary stores it in `trigger_price`. The two therefore compute DIFFERENT
// signal signatures for the same economic order, so restart dedupe misses and the
// working stop can be placed a SECOND time — both fire, and the position inverts.
// No signature scheme can fix that (the builds genuinely disagree about which
// field holds the level), so the residual is caught operationally, here, at cold
// boot — where a refusal costs a deploy rather than a naked position.
//
// The fingerprint is exact: store migration 2 backfills trigger_price_paise as
// NULL, and the validation gate refuses a trigger-less stop outright, so a
// WORKING stop with no trigger can ONLY have been written by the old binary.

TEST_CASE("legacy-stop guard: a clean projection starts", "[session][safe-start][IMP-11]") {
  using broker_exec::domain::OrderState;
  using broker_exec::domain::OrderType;

  // Empty is fine, and so is a book of well-formed orders.
  CHECK(broker_exec::session::require_no_legacy_stops({}).has_value());

  const std::vector<broker_exec::domain::Order> clean = {
      order_row("a", OrderType::Limit, OrderState::Acknowledged, /*armed=*/false),
      order_row("b", OrderType::Market, OrderState::Sent, /*armed=*/false),
      order_row("c", OrderType::StopLoss, OrderState::Acknowledged, /*armed=*/true),
      order_row("d", OrderType::StopLossMarket, OrderState::PartiallyFilled, /*armed=*/true),
  };
  CHECK(broker_exec::session::require_no_legacy_stops(clean).has_value());
}

TEST_CASE("legacy-stop guard: a WORKING trigger-less stop refuses the start",
          "[session][safe-start][IMP-11]") {
  using broker_exec::domain::OrderState;
  using broker_exec::domain::OrderType;

  for (const OrderType type : {OrderType::StopLoss, OrderType::StopLossMarket}) {
    const std::vector<broker_exec::domain::Order> book = {
        order_row("fine", OrderType::Limit, OrderState::Acknowledged, false),
        order_row("LEGACY-1", type, OrderState::Acknowledged, /*armed=*/false),
    };
    const Result<broker_exec::ports::Ok> r = broker_exec::session::require_no_legacy_stops(book);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().category == ErrorCategory::Validation);
    // It must HALT, not merely alert: the operator has to act before trading.
    CHECK(r.error().action == SuggestedAction::BlockStrategy);
    // The message names the offender so the operator can go cancel it, and says
    // what to do — but carries no price/quantity (redaction-safe).
    CHECK(r.error().message.find("LEGACY-1") != std::string::npos);
    CHECK(r.error().message.find("docs/upgrade-imp-11-stops.md") != std::string::npos);
    CHECK(r.error().message.find("120") == std::string::npos);
  }
}

TEST_CASE("legacy-stop guard: TERMINAL trigger-less stops are history, not a block",
          "[session][safe-start][IMP-11]") {
  using broker_exec::domain::OrderState;
  using broker_exec::domain::OrderType;

  // A filled/cancelled/rejected stop cannot fire again, so it cannot be
  // duplicated. Blocking on it would wedge the gate permanently on any database
  // that merely REMEMBERS a pre-upgrade stop — a guard that can never be
  // satisfied is one operators learn to bypass.
  for (const OrderState state : {OrderState::Filled, OrderState::Cancelled, OrderState::Rejected}) {
    const std::vector<broker_exec::domain::Order> book = {
        order_row("old", OrderType::StopLoss, state, /*armed=*/false)};
    CHECK(broker_exec::session::require_no_legacy_stops(book).has_value());
    CHECK_FALSE(broker_exec::session::is_legacy_trigger_less_stop(book.front()));
  }

  // ...but an UNKNOWN one absolutely blocks: it may well be live.
  const std::vector<broker_exec::domain::Order> unknown = {
      order_row("maybe-live", OrderType::StopLoss, OrderState::Unknown, /*armed=*/false)};
  CHECK_FALSE(broker_exec::session::require_no_legacy_stops(unknown).has_value());
}

TEST_CASE("legacy-stop guard: the count is reported and the FIRST offender named",
          "[session][safe-start][IMP-11]") {
  using broker_exec::domain::OrderState;
  using broker_exec::domain::OrderType;

  const std::vector<broker_exec::domain::Order> book = {
      order_row("FIRST", OrderType::StopLoss, OrderState::Acknowledged, false),
      order_row("SECOND", OrderType::StopLossMarket, OrderState::Sent, false),
  };
  const Result<broker_exec::ports::Ok> r = broker_exec::session::require_no_legacy_stops(book);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().message.find("2 working stop order(s)") != std::string::npos);
  CHECK(r.error().message.find("FIRST") != std::string::npos);
}

// ── IMP-19: the strategy-name cold-boot guard ────────────────────────────────
//
// A strategy name is the FIRST SEGMENT of every client_ref it mints, and
// domain::is_provenance_id_shape admits a ref into an alert or ledger entry only
// if EVERY segment is homogeneous. A strategy called "S1" therefore makes every
// alert about its orders read `client_ref=***REDACTED***`. The name is
// CONFIGURATION, known at startup, so it is caught HERE — where a refusal costs a
// deploy — rather than at reserve()/place(), where refusing means refusing to
// place an order mid-session.

TEST_CASE("strategy-name guard: valid names start", "[session][safe-start][IMP-19]") {
  // An empty list passes vacuously: this check is about the names that exist. The
  // "you forgot to wire it" case is caught by the UNSET-check rule, not here.
  CHECK(require_valid_strategy_names({}).has_value());

  const std::vector<std::string> good = {
      "alpha", "S-1", "momentum-v-2", "atm-straddle-9-20", "IRON_CONDOR", "12345",
  };
  CHECK(require_valid_strategy_names(good).has_value());
}

TEST_CASE("strategy-name guard: each invalid name refuses the start, naming the offender",
          "[session][safe-start][IMP-19]") {
  struct Case {
    std::string name;
    const char* expect_in_message;
  };
  const std::vector<Case> cases = {
      // The subtlest and the whole reason this story exists: one letter, one digit.
      {"S1", "segment 'S1'"},
      // An ordinary options strategy name with spaces.
      {"iron condor v2", "iron?condor?v2"},
      // Empty.
      {"", "EMPTY"},
      // Separators only — legal by every other rule, and exactly as unattributable
      // as the empty name above.
      {"---", "no letter or digit"},
      // Over the bound (so a minted ref would exceed kMaxProvenanceIdChars).
      {std::string(kMaxStrategyNameChars + 1, 'a'), "maximum is"},
      // Ill-formed UTF-8: raw through the store/index, normalised only at the log
      // writer, where preimage and stored bytes then disagree.
      {std::string("alpha\x80"), "not valid UTF-8"},
  };

  for (const Case& c : cases) {
    INFO("name = " << c.name);
    const std::vector<std::string> names = {"alpha", c.name, "S-1"};
    const Result<broker_exec::ports::Ok> r = require_valid_strategy_names(names);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().category == ErrorCategory::Validation);
    // It must HALT, not merely alert: the operator has to fix a name before trading.
    CHECK(r.error().action == SuggestedAction::BlockStrategy);
    CHECK(r.error().message.find(c.expect_in_message) != std::string::npos);
    // The count is reported so the operator knows whether one name or all of them
    // need fixing, and the VALID names are never blamed.
    CHECK(r.error().message.find("1 of 3") != std::string::npos);
  }
}

TEST_CASE("strategy-name guard: the message teaches the fix", "[session][safe-start][IMP-19]") {
  const Result<broker_exec::ports::Ok> r = require_valid_strategy_names({"S1"});
  REQUIRE_FALSE(r.has_value());
  // The suggested spelling, and the warning that a rename moves the signal
  // signature (so it must happen between sessions, flat) — the operator gets both.
  CHECK(r.error().message.find("use 'S-1'") != std::string::npos);
  CHECK(r.error().message.find("BETWEEN sessions") != std::string::npos);
  // ...and the suggested spelling really is accepted.
  CHECK(require_valid_strategy_names({"S-1"}).has_value());
}

TEST_CASE("strategy-name guard: wired into the gate, it blocks naming strategy-names",
          "[session][safe-start][IMP-19]") {
  const std::vector<std::string> names = {"alpha", "S1"};

  const SafeStartGate gate;
  SafeStartContext ctx = make_all_passing();
  ctx.strategy_name_check = [&names] { return require_valid_strategy_names(names); };

  const Result<broker_exec::ports::Ok> r = gate.verify(ctx);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().message.find("safe-start: strategy-names") != std::string::npos);
  CHECK(r.error().category == ErrorCategory::Validation);  // inner category preserved
  CHECK(r.error().action == SuggestedAction::BlockStrategy);

  // It runs EARLY — before crypto keys, the clock or the broker session — because
  // the names are configuration and nothing later can be logged about safely.
  ctx.crypto_keys_check = fail_check(ErrorCategory::Internal, "no keys");
  const Result<broker_exec::ports::Ok> ordered = gate.verify(ctx);
  REQUIRE_FALSE(ordered.has_value());
  CHECK(ordered.error().message.find("strategy-names") != std::string::npos);
  CHECK(ordered.error().message.find("crypto-keys") == std::string::npos);
}

TEST_CASE("legacy-stop guard: wired into the gate, it blocks naming legacy-stops",
          "[session][safe-start][IMP-11]") {
  using broker_exec::domain::OrderState;
  using broker_exec::domain::OrderType;

  const std::vector<broker_exec::domain::Order> book = {
      order_row("LEGACY-9", OrderType::StopLoss, OrderState::Acknowledged, false)};

  const SafeStartGate gate;
  SafeStartContext ctx = make_all_passing();
  ctx.legacy_stop_check = [&book] { return broker_exec::session::require_no_legacy_stops(book); };

  const Result<broker_exec::ports::Ok> r = gate.verify(ctx);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().message.find("safe-start: legacy-stops") != std::string::npos);
  CHECK(r.error().category == ErrorCategory::Validation);  // inner category preserved
  CHECK(r.error().action == SuggestedAction::BlockStrategy);
}
