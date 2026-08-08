// broker_exec::composition — EngineAssembly tests (IMP-12).
//
// Four things are proven here, in this order:
//   1. THE BUILDER REFUSES (AC-2). Every input whose absence would make a gate
//      check vacuously PASS is named in its own typed refusal, and an explicitly
//      EXIT-ONLY assembly is the one way to build without funds/risk/calendar —
//      while still requiring kill + posture.
//   2. A WIRED GUARD IS NOT AN ARMED ONE. The two fail-open holes an adversarial
//      review probed against the built library — an all-zero margin quote and an
//      all-off RiskLimits — are closed, and the tests assert the *fail-open* case
//      would have passed (a Rs 1 balance, an unlimited order) so a regression
//      cannot quietly restore it.
//   3. THE CHAINS ARE ORDERED AND SELF-NAMING (AC-3/AC-4). Each stage can block
//      on its own and says which stage it was; stacking every failure at once and
//      peeling them off one at a time proves the ORDER, not just the membership.
//      A clean intent traverses all of them.
//   4. THE ASYMMETRY HOLDS. An exit is never blocked by an entry-only stage, is
//      clamped rather than refused out of band, and is still shape-validated.
//
// The final section is the integration smoke: REAL FundsView / TradingCalendar /
// RiskEngine / KillState / PostureCoordinator / ValidationGate / priceband /
// marginsafety, over fake clock + fetch seams only.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`, no float.

#include "broker_exec/composition/engine_assembly.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/marginsafety/margin_buffer.hpp"
#include "broker_exec/modes/killswitch.hpp"
#include "broker_exec/modes/posture.hpp"
#include "broker_exec/modifyguard/modify_guard.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/priceband/band_check.hpp"
#include "broker_exec/refdata/trading_calendar.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/risk/funds_view.hpp"
#include "broker_exec/risk/risk_engine.hpp"
#include "broker_exec/session/session_state.hpp"

namespace comp = broker_exec::composition;
namespace mgn = broker_exec::marginsafety;

namespace {

using broker_exec::Result;
using broker_exec::clock::TestClock;
using broker_exec::composition::EngineAssembly;
using broker_exec::composition::EngineDeps;
using broker_exec::composition::EngineMode;
using broker_exec::composition::EngineOptions;
using broker_exec::composition::make_engine;
using broker_exec::composition::ModifyContext;
using broker_exec::composition::PreflightInputs;
using broker_exec::composition::SessionSnapshot;
using broker_exec::composition::StageResult;
using broker_exec::domain::Instrument;
using broker_exec::domain::Money;
using broker_exec::domain::OrderIntent;
using broker_exec::domain::OrderState;
using broker_exec::domain::OrderType;
using broker_exec::domain::Price;
using broker_exec::domain::Product;
using broker_exec::domain::Quantity;
using broker_exec::domain::Side;
using broker_exec::errors::Error;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::make_error;
using broker_exec::errors::SuggestedAction;
using broker_exec::modes::DetectorSignal;
using broker_exec::modes::KillCommand;
using broker_exec::modes::KillState;
using broker_exec::modes::KillType;
using broker_exec::modes::Posture;
using broker_exec::modes::PostureCoordinator;
using broker_exec::ports::FundsSnapshot;
using broker_exec::priceband::BandVerdict;
using broker_exec::priceband::PriceBand;
using broker_exec::refdata::TradingCalendar;
using broker_exec::risk::FundsView;
using broker_exec::risk::GateOutcome;
using broker_exec::risk::RiskEngine;
using broker_exec::risk::RiskLimits;
using broker_exec::session::SessionState;

// ── Fixtures ────────────────────────────────────────────────────────────────

// A unique temp directory, cleaned up on destruction (RAII). The trading
// calendar date-versions its cache to disk; nothing else here touches the FS.
struct TempDir {
  std::filesystem::path path;
  TempDir() {
    std::random_device rd;
    path = std::filesystem::temp_directory_path() /
           ("brexec_engine_" + std::to_string(rd()) + "_" + std::to_string(rd()));
    std::filesystem::create_directories(path);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

[[nodiscard]] std::chrono::system_clock::time_point utc_at(int year, unsigned month, unsigned day,
                                                           int hour, int minute) {
  const std::chrono::sys_days d{std::chrono::year{year} / std::chrono::month{month} /
                                std::chrono::day{day}};
  return std::chrono::system_clock::time_point(d) + std::chrono::hours(hour) +
         std::chrono::minutes(minute);
}

// Calendar: open 09:15 / entry_cutoff 15:00 / square_off 15:20 / close 15:30 IST.
// 2026-06-29 is a Monday.
constexpr const char* kCalendarJson =
    R"({"holidays":[],"special_sessions":[],)"
    R"("windows":{"open":"09:15","entry_cutoff":"15:00","square_off":"15:20","close":"15:30"}})";

constexpr const char* kSymbol = "NIFTY26JUL24000CE";

// NFO option: lot 50, tick 5 paise, freeze 1800.
[[nodiscard]] Instrument make_instrument() {
  return Instrument{.symbol = kSymbol,
                    .token = 123456,
                    .exchange = "NFO",
                    .lot_size = Quantity::of(50),
                    .tick_size = Price::from_paise(5),
                    .freeze_qty = Quantity::of(1800),
                    .expiry = "2026-07-30"};
}

// A clean limit ENTRY: lot-aligned qty 50, tick-aligned 100.00 (10 000 paise).
[[nodiscard]] OrderIntent make_entry(std::int64_t price_paise = 10'000,
                                     std::string_view strategy = "alpha") {
  return OrderIntent{.client_ref = std::string(strategy) + "-1a2b3c4d-uuid",
                     .symbol = kSymbol,
                     .side = Side::Buy,
                     .quantity = Quantity::of(50),
                     .price = Price::from_paise(price_paise),
                     .order_type = OrderType::Limit,
                     .product = Product::Intraday,
                     .strategy = std::string(strategy)};
}

// A protective SELL stop-loss LIMIT exit: limit <= trigger (the side-ordering the
// gate enforces), both tick-aligned.
[[nodiscard]] OrderIntent make_stop_exit(std::int64_t limit_paise, std::int64_t trigger_paise) {
  OrderIntent intent{.client_ref = "alpha-1a2b3c4d-uuid#exit",
                     .symbol = kSymbol,
                     .side = Side::Sell,
                     .quantity = Quantity::of(50),
                     .price = Price::from_paise(limit_paise),
                     .order_type = OrderType::StopLoss,
                     .product = Product::Intraday,
                     .strategy = "alpha"};
  intent.trigger_price = Price::from_paise(trigger_paise);
  return intent;
}

// The whole world an EngineAssembly is wired into: REAL modules, with only the
// clock and the two fetch seams faked. Every knob a test flips is a plain member
// declared BEFORE the modules that read it, so member-initialization order is
// never in question. Non-copyable: the dependency lambdas capture `this`.
struct Rig {
  TempDir dir;
  // IST 09:20 on Monday 2026-06-29 — inside the entry window.
  TestClock clk{std::chrono::steady_clock::time_point{}, utc_at(2026, 6, 29, 3, 50)};

  // ── knobs ──
  std::int64_t funds_available_paise = 6'000'000;  // Rs 60 000
  bool funds_fetch_fails = false;
  std::vector<DetectorSignal> signals;
  SessionSnapshot session{SessionState::Healthy, ""};
  bool unknown_pause = false;
  bool duplicate = false;
  bool duplicate_probe_fails = false;
  mgn::MarginInputs margin;
  bool margin_quote_fails = false;
  bool hedge_blocks = false;
  bool wire_hedge = false;
  bool wire_is_reducing = false;
  bool intent_is_reducing = true;

  // ── real modules ──
  TradingCalendar calendar;
  FundsView funds;
  RiskEngine risk;
  KillState kill_state;
  PostureCoordinator coordinator;

  Rig()
      : calendar([]() -> Result<std::string> { return std::string(kCalendarJson); }, clk, dir.path,
                 "kite"),
        funds(
            clk,
            [this]() -> Result<FundsSnapshot> {
              if (funds_fetch_fails) {
                return broker_exec::fail(
                    make_error(ErrorCategory::Network, "fake transport is down"));
              }
              FundsSnapshot snapshot;
              snapshot.available_margin = Money::from_paise(funds_available_paise);
              return snapshot;
            },
            std::chrono::seconds(30)) {
    margin.api_required = Money::from_paise(5'000'000);  // Rs 50 000
    REQUIRE(calendar.refresh().has_value());
  }

  Rig(const Rig&) = delete;
  Rig& operator=(const Rig&) = delete;

  // A fully-wired EntryCapable dependency set. Tests then null out exactly one
  // field to drive one refusal.
  [[nodiscard]] EngineDeps deps() {
    EngineDeps d;
    d.kill_state = &kill_state;
    d.posture = &coordinator;
    d.detector_signals = [this]() { return signals; };
    d.session = [this]() { return session; };
    d.funds_view = &funds;
    d.calendar = &calendar;
    d.risk_engine = &risk;
    d.unknown_pause = [this]() { return unknown_pause; };
    d.duplicate_probe = [this](const OrderIntent&) -> Result<bool> {
      if (duplicate_probe_fails) {
        return broker_exec::fail(make_error(ErrorCategory::Internal, "projection read failed"));
      }
      return duplicate;
    };
    d.margin_inputs = [this](const OrderIntent&) -> Result<mgn::MarginInputs> {
      if (margin_quote_fails) {
        return broker_exec::fail(
            make_error(ErrorCategory::RateLimited, "margin API returned 429", "429"));
      }
      return margin;
    };
    if (wire_hedge) {
      d.hedge_check = [this](const OrderIntent&) -> Result<broker_exec::ports::Ok> {
        if (hedge_blocks) {
          return broker_exec::fail(
              make_error(ErrorCategory::RiskRejected, "naked short leg would be created"));
        }
        return broker_exec::ports::ok();
      };
    }
    if (wire_is_reducing) {
      d.is_reducing = [this](const OrderIntent&) { return intent_is_reducing; };
    }
    return d;
  }

  [[nodiscard]] PreflightInputs inputs() const {
    PreflightInputs in;
    in.instrument = make_instrument();
    in.band = PriceBand{Money::from_paise(9'000), Money::from_paise(11'000), true};
    // An ARMED but non-binding limit. The entry path REFUSES an all-off
    // RiskLimits (it would hand the gate a check that enforces nothing), so the
    // default rig arms one that a clean order comfortably satisfies — otherwise
    // every entry test here would be exercising the declared-off escape hatch
    // instead of the real risk path.
    in.risk_limits.max_open_positions = 10;
    in.risk_state.open_positions = 0;
    return in;
  }
};

[[nodiscard]] EngineOptions entry_options() {
  EngineOptions options;
  options.mode = EngineMode::EntryCapable;
  options.allowed_exchanges = {"NFO"};
  options.allowed_products = {Product::Intraday};
  options.declares_no_hedge_check = true;
  return options;
}

[[nodiscard]] EngineOptions exit_only_options() {
  EngineOptions options = entry_options();
  options.mode = EngineMode::ExitOnly;
  return options;
}

[[nodiscard]] bool says(const Error& err, std::string_view needle) {
  return err.message.find(needle) != std::string::npos;
}

[[nodiscard]] bool says(const StageResult& result, std::string_view needle) {
  return !result.ok() && says(result.result.error(), needle);
}

// Every refusal in this module is the same typed shape; assert it once.
[[nodiscard]] bool is_wiring_refusal(const Error& err) {
  return err.category == ErrorCategory::Validation && err.action == SuggestedAction::DoNotRetry;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// AC-2 — the fail-closed builder.
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("engine: a fully-wired EntryCapable assembly builds", "[composition][engine][AC2]") {
  Rig rig;
  auto engine = make_engine(rig.deps(), entry_options());
  REQUIRE(engine.has_value());
  CHECK(engine.value().mode() == EngineMode::EntryCapable);
  CHECK(engine.value().is_entry_capable());
}

TEST_CASE("engine: EngineOptions defaults to the SAFE mode", "[composition][engine][AC2]") {
  // A caller who forgets `mode` gets an engine that cannot place an entry — never
  // one that can. Every declaration escape hatch likewise defaults to "not
  // declared", so silence never buys a relaxation.
  const EngineOptions defaults{};
  CHECK(defaults.mode == EngineMode::ExitOnly);
  CHECK_FALSE(defaults.declares_no_hedge_check);
  CHECK_FALSE(defaults.declares_no_risk_limits);
  CHECK_FALSE(defaults.risk_gates_exits);
  CHECK_FALSE(defaults.block_entry_on_unknown_band);
}

TEST_CASE("engine: an entry-capable assembly REFUSES each missing piece, by name",
          "[composition][engine][AC2]") {
  Rig rig;

  SECTION("funds_view") {
    EngineDeps deps = rig.deps();
    deps.funds_view = nullptr;
    auto engine = make_engine(deps, entry_options());
    REQUIRE_FALSE(engine.has_value());
    CHECK(is_wiring_refusal(engine.error()));
    CHECK(says(engine.error(), "EngineDeps::funds_view"));
    // The refusal explains WHY a default was not substituted.
    CHECK(says(engine.error(), "PASS"));
  }
  SECTION("calendar") {
    EngineDeps deps = rig.deps();
    deps.calendar = nullptr;
    auto engine = make_engine(deps, entry_options());
    REQUIRE_FALSE(engine.has_value());
    CHECK(is_wiring_refusal(engine.error()));
    CHECK(says(engine.error(), "EngineDeps::calendar"));
  }
  SECTION("risk_engine") {
    EngineDeps deps = rig.deps();
    deps.risk_engine = nullptr;
    auto engine = make_engine(deps, entry_options());
    REQUIRE_FALSE(engine.has_value());
    CHECK(says(engine.error(), "EngineDeps::risk_engine"));
  }
  SECTION("kill_state") {
    EngineDeps deps = rig.deps();
    deps.kill_state = nullptr;
    auto engine = make_engine(deps, entry_options());
    REQUIRE_FALSE(engine.has_value());
    CHECK(says(engine.error(), "EngineDeps::kill_state"));
  }
  SECTION("posture coordinator") {
    EngineDeps deps = rig.deps();
    deps.posture = nullptr;
    auto engine = make_engine(deps, entry_options());
    REQUIRE_FALSE(engine.has_value());
    CHECK(says(engine.error(), "EngineDeps::posture"));
  }
  SECTION("detector signal source") {
    EngineDeps deps = rig.deps();
    deps.detector_signals = nullptr;
    auto engine = make_engine(deps, entry_options());
    REQUIRE_FALSE(engine.has_value());
    CHECK(says(engine.error(), "EngineDeps::detector_signals"));
  }
  SECTION("session source") {
    EngineDeps deps = rig.deps();
    deps.session = nullptr;
    auto engine = make_engine(deps, entry_options());
    REQUIRE_FALSE(engine.has_value());
    CHECK(says(engine.error(), "EngineDeps::session"));
  }
  SECTION("unknown-pause source") {
    EngineDeps deps = rig.deps();
    deps.unknown_pause = nullptr;
    auto engine = make_engine(deps, entry_options());
    REQUIRE_FALSE(engine.has_value());
    CHECK(says(engine.error(), "EngineDeps::unknown_pause"));
  }
  SECTION("duplicate probe") {
    EngineDeps deps = rig.deps();
    deps.duplicate_probe = nullptr;
    auto engine = make_engine(deps, entry_options());
    REQUIRE_FALSE(engine.has_value());
    CHECK(says(engine.error(), "EngineDeps::duplicate_probe"));
  }
  SECTION("margin input source") {
    EngineDeps deps = rig.deps();
    deps.margin_inputs = nullptr;
    auto engine = make_engine(deps, entry_options());
    REQUIRE_FALSE(engine.has_value());
    CHECK(says(engine.error(), "EngineDeps::margin_inputs"));
  }
}

TEST_CASE("engine: an unwired hedge check must be DECLARED, not assumed",
          "[composition][engine][AC2]") {
  Rig rig;
  EngineOptions options = entry_options();
  options.declares_no_hedge_check = false;  // silence

  auto refused = make_engine(rig.deps(), options);
  REQUIRE_FALSE(refused.has_value());
  CHECK(is_wiring_refusal(refused.error()));
  CHECK(says(refused.error(), "EngineDeps::hedge_check"));
  CHECK(says(refused.error(), "declares_no_hedge_check"));

  // Saying so out loud is enough…
  options.declares_no_hedge_check = true;
  CHECK(make_engine(rig.deps(), options).has_value());

  // …and so is actually wiring one.
  rig.wire_hedge = true;
  options.declares_no_hedge_check = false;
  CHECK(make_engine(rig.deps(), options).has_value());
}

TEST_CASE("engine: risk_gates_exits without a risk engine is a WIRING error",
          "[composition][engine][AC2][MEDIUM3]") {
  // Not a harmless no-op: the caller explicitly opted IN to the stricter posture
  // and would silently receive the permissive one.
  Rig rig;
  EngineOptions options = exit_only_options();
  options.risk_gates_exits = true;

  EngineDeps deps = rig.deps();
  deps.risk_engine = nullptr;
  auto refused = make_engine(deps, options);
  REQUIRE_FALSE(refused.has_value());
  CHECK(is_wiring_refusal(refused.error()));
  CHECK(says(refused.error(), "risk_gates_exits"));
  CHECK(says(refused.error(), "EngineDeps::risk_engine"));

  // With an engine wired it composes fine, in either mode.
  CHECK(make_engine(rig.deps(), options).has_value());
}

TEST_CASE("engine: an EXIT-ONLY assembly builds without funds/risk/calendar",
          "[composition][engine][AC2]") {
  Rig rig;
  EngineDeps deps = rig.deps();
  // Everything the entry pipeline needs, gone.
  deps.funds_view = nullptr;
  deps.calendar = nullptr;
  deps.risk_engine = nullptr;
  deps.unknown_pause = nullptr;
  deps.duplicate_probe = nullptr;
  deps.margin_inputs = nullptr;

  auto engine = make_engine(deps, exit_only_options());
  REQUIRE(engine.has_value());
  CHECK(engine.value().mode() == EngineMode::ExitOnly);
  CHECK_FALSE(engine.value().is_entry_capable());
}

TEST_CASE("engine: an EXIT-ONLY assembly still requires kill + posture",
          "[composition][engine][AC2]") {
  Rig rig;

  SECTION("kill_state") {
    EngineDeps deps = rig.deps();
    deps.kill_state = nullptr;
    auto engine = make_engine(deps, exit_only_options());
    REQUIRE_FALSE(engine.has_value());
    CHECK(says(engine.error(), "EngineDeps::kill_state"));
    CHECK(says(engine.error(), "every mode"));
  }
  SECTION("posture coordinator") {
    EngineDeps deps = rig.deps();
    deps.posture = nullptr;
    auto engine = make_engine(deps, exit_only_options());
    REQUIRE_FALSE(engine.has_value());
    CHECK(says(engine.error(), "EngineDeps::posture"));
  }
  SECTION("detector signals") {
    EngineDeps deps = rig.deps();
    deps.detector_signals = nullptr;
    auto engine = make_engine(deps, exit_only_options());
    REQUIRE_FALSE(engine.has_value());
    CHECK(says(engine.error(), "EngineDeps::detector_signals"));
  }
  SECTION("session source") {
    EngineDeps deps = rig.deps();
    deps.session = nullptr;
    auto engine = make_engine(deps, exit_only_options());
    REQUIRE_FALSE(engine.has_value());
    CHECK(says(engine.error(), "EngineDeps::session"));
  }
}

TEST_CASE("engine: EngineMode names are the stable observability contract",
          "[composition][engine][AC2]") {
  CHECK(comp::to_string(EngineMode::EntryCapable) == "EntryCapable");
  CHECK(comp::to_string(EngineMode::ExitOnly) == "ExitOnly");
}

// ═══════════════════════════════════════════════════════════════════════════
// A WIRED GUARD IS NOT AN ARMED ONE — the two probed fail-open holes.
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("engine: a margin source that CANNOT quote blocks the entry",
          "[composition][engine][HIGH1]") {
  Rig rig;
  auto engine = make_engine(rig.deps(), entry_options());
  REQUIRE(engine.has_value());
  const PreflightInputs in = rig.inputs();

  SECTION("an erroring source fails the margin stage, fail-closed") {
    rig.margin_quote_fails = true;
    const StageResult r = engine.value().preflight_entry(make_entry(), in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kMargin);
    // Normalized to the fail-closed verdict whatever the transport-level cause…
    CHECK(r.result.error().category == ErrorCategory::DataStale);
    CHECK(r.result.error().action == SuggestedAction::BlockStrategy);
    // …while the underlying cause stays legible for diagnosis.
    CHECK(says(r, "could not be obtained"));
    CHECK(says(r, "RateLimited"));
  }

  SECTION("a ZERO quote is refused — the shape a swallowed failure takes") {
    // THE ORIGINAL FAIL-OPEN: a by-value source can only answer a dead margin API
    // with zeros, and zero passes the funds check against ANY balance (Rs 1 here)
    // and passes the buffered stage too, because zero plus five percent is zero.
    rig.margin.api_required = Money::from_paise(0);
    rig.funds_available_paise = 100;  // Rs 1
    const StageResult r = engine.value().preflight_entry(make_entry(), in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kMargin);
    CHECK(r.result.error().category == ErrorCategory::DataStale);
    CHECK(says(r, "non-positive"));
  }

  SECTION("a NEGATIVE quote is refused too") {
    rig.margin.api_required = Money::from_paise(-1);
    const StageResult r = engine.value().preflight_entry(make_entry(), in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kMargin);
  }

  SECTION("an honest quote still passes") {
    const StageResult r = engine.value().preflight_entry(make_entry(), in);
    REQUIRE(r.ok());
    CHECK(r.result.value().margin_checked);
  }
}

TEST_CASE("engine: an all-off RiskLimits is refused unless it is DECLARED",
          "[composition][engine][HIGH2]") {
  Rig rig;
  auto engine = make_engine(rig.deps(), entry_options());
  REQUIRE(engine.has_value());

  PreflightInputs bare = rig.inputs();
  bare.risk_limits = RiskLimits{};  // every field at its "off" sentinel

  SECTION("undeclared: refused at the risk-limits stage") {
    // THE ORIGINAL FAIL-OPEN: make_risk_check still produces a real, non-empty
    // predicate, the gate still calls it, and it says ok() for every order at
    // every size and every loss. From the outside the guard looks wired.
    const StageResult r = engine.value().preflight_entry(make_entry(), bare);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kRiskLimits);
    CHECK(is_wiring_refusal(r.result.error()));
    CHECK(says(r, "arms NOTHING"));
    CHECK(says(r, "declares_no_risk_limits"));
  }

  SECTION("declared: a pure-execution component may say it meant it") {
    EngineOptions options = entry_options();
    options.declares_no_risk_limits = true;
    auto declared = make_engine(rig.deps(), options);
    REQUIRE(declared.has_value());
    CHECK(declared.value().preflight_entry(make_entry(), bare).ok());
  }

  SECTION("one armed field is enough") {
    bare.risk_limits.block_market_orders = true;  // the non-numeric sentinel
    CHECK(engine.value().preflight_entry(make_entry(), bare).ok());
  }

  SECTION("the exit chain has no risk-limits stage at all") {
    // An exit is never asked to justify its limits — it reduces risk by
    // construction, and refusing one over a config declaration would strand it.
    CHECK(engine.value().preflight_exit(make_stop_exit(9'500, 9'600), bare).ok());
  }
}

TEST_CASE("engine: any_risk_limit_armed sees every field", "[composition][engine][HIGH2]") {
  CHECK_FALSE(comp::any_risk_limit_armed(RiskLimits{}));

  const auto armed = [](auto&& arm) {
    RiskLimits limits;
    arm(limits);
    return comp::any_risk_limit_armed(limits);
  };
  CHECK(armed([](RiskLimits& l) { l.daily_loss_limit_paise = 1; }));
  CHECK(armed([](RiskLimits& l) { l.max_open_positions = 1; }));
  CHECK(armed([](RiskLimits& l) { l.max_account_margin_paise = 1; }));
  CHECK(armed([](RiskLimits& l) { l.max_order_value_paise = 1; }));
  CHECK(armed([](RiskLimits& l) { l.block_market_orders = true; }));
  CHECK(armed([](RiskLimits& l) { l.max_slippage_bps = 1; }));
  CHECK(armed([](RiskLimits& l) { l.max_lots_per_strategy = 1; }));
  CHECK(armed([](RiskLimits& l) { l.max_lots_per_instrument = 1; }));
  CHECK(armed([](RiskLimits& l) { l.strategy_daily_loss_limit_paise = 1; }));
}

TEST_CASE("engine: an unpopulated order value is NOT a known order value",
          "[composition][engine][HIGH2]") {
  // RiskState defaults to `order_value_known = true, order_value_paise = 0`, which
  // made RiskEngine's fail-closed "cannot verify order value" branch UNREACHABLE
  // while max-order-value and max-account-margin compared against zero and always
  // passed. The engine now ANDs the flag with an actually-positive value.
  Rig rig;
  auto engine = make_engine(rig.deps(), entry_options());
  REQUIRE(engine.has_value());

  PreflightInputs in = rig.inputs();
  in.risk_limits.max_order_value_paise = 1'000;  // armed, and tiny
  in.risk_state.order_value_paise = 0;           // …but never populated
  in.risk_state.order_value_known = true;        // the misleading default

  const StageResult r = engine.value().preflight_entry(make_entry(), in);
  REQUIRE_FALSE(r.ok());
  CHECK(r.stage == comp::stage::kGate);
  CHECK(says(r, "gate: risk check failed"));
  CHECK(says(r, "cannot verify order value"));

  SECTION("a populated value is evaluated normally") {
    PreflightInputs populated = in;
    populated.risk_state.order_value_paise = 500;  // under the 1 000 ceiling
    CHECK(engine.value().preflight_entry(make_entry(), populated).ok());
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// AC-4 — posture, with the kill switch as operator floor (and its SCOPE).
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("engine: the kill switch is the posture operator FLOOR",
          "[composition][engine][AC4]") {
  Rig rig;
  auto engine = make_engine(rig.deps(), entry_options());
  REQUIRE(engine.has_value());
  const EngineAssembly& eng = engine.value();

  CHECK(eng.evaluate_posture() == Posture::Normal);

  // A detector signal alone lifts the posture…
  rig.signals = {DetectorSignal::StaleData};
  CHECK(eng.evaluate_posture() == Posture::BlockEntries);

  // …and the kill switch raises the FLOOR beneath it: the severest wins.
  rig.kill_state.apply(KillCommand{KillType::Soft, ""});
  CHECK(eng.evaluate_posture() == Posture::SoftKill);

  rig.kill_state.apply(KillCommand{KillType::Panic, ""});
  CHECK(eng.evaluate_posture() == Posture::Panic);

  // The explicit-signals overload uses the same floor (it is not a way around it).
  CHECK(eng.evaluate_posture(std::vector<DetectorSignal>{}) == Posture::Panic);
}

TEST_CASE("engine: a Strategy kill is SCOPED — it must not freeze other strategies",
          "[composition][engine][AC4][MEDIUM4]") {
  Rig rig;
  auto engine = make_engine(rig.deps(), entry_options());
  REQUIRE(engine.has_value());
  const EngineAssembly& eng = engine.value();
  const PreflightInputs in = rig.inputs();

  SECTION("killing 'beta' leaves 'alpha' trading") {
    rig.kill_state.apply(KillCommand{KillType::Strategy, "beta"});

    // The PROCESS-WIDE posture is degraded (a kill is active) …
    CHECK(eng.evaluate_posture() == Posture::SoftKill);
    // … while the posture AS IT APPLIES TO 'alpha' is untouched.
    CHECK(eng.evaluate_posture_for_strategy("alpha") == Posture::Normal);
    CHECK(eng.evaluate_posture_for_strategy("beta") == Posture::SoftKill);

    CHECK(eng.preflight_entry(make_entry(10'000, "alpha"), in).ok());

    const StageResult killed = eng.preflight_entry(make_entry(10'000, "beta"), in);
    REQUIRE_FALSE(killed.ok());
    CHECK(killed.stage == comp::stage::kPosture);
  }

  SECTION("a Soft kill is BROAD and stops every strategy") {
    rig.kill_state.apply(KillCommand{KillType::Soft, ""});
    CHECK(eng.evaluate_posture_for_strategy("alpha") == Posture::SoftKill);
    CHECK_FALSE(eng.preflight_entry(make_entry(10'000, "alpha"), in).ok());
    CHECK_FALSE(eng.preflight_entry(make_entry(10'000, "beta"), in).ok());
  }

  SECTION("an Account kill is BROAD and stops every strategy") {
    rig.kill_state.apply(KillCommand{KillType::Account, "acct-1"});
    CHECK_FALSE(eng.preflight_entry(make_entry(10'000, "alpha"), in).ok());
    CHECK_FALSE(eng.preflight_entry(make_entry(10'000, "gamma"), in).ok());
  }

  SECTION("PANIC is broad and closes even the exit gate, for every strategy") {
    rig.kill_state.apply(KillCommand{KillType::Panic, ""});
    CHECK(eng.evaluate_posture_for_strategy("alpha") == Posture::Panic);
    CHECK_FALSE(eng.preflight_entry(make_entry(10'000, "alpha"), in).ok());
    CHECK_FALSE(eng.preflight_exit(make_stop_exit(9'500, 9'600), in).ok());
  }

  SECTION("detector signals are process-wide and are NOT scoped away") {
    rig.kill_state.apply(KillCommand{KillType::Strategy, "beta"});
    rig.signals = {DetectorSignal::StaleData};
    // 'alpha' escapes the kill floor but not the degraded feed.
    CHECK(eng.evaluate_posture_for_strategy("alpha") == Posture::BlockEntries);
    CHECK_FALSE(eng.preflight_entry(make_entry(10'000, "alpha"), in).ok());
  }
}

TEST_CASE("engine: a kill switch blocks entries END TO END, at the posture stage",
          "[composition][engine][AC4][AC5]") {
  Rig rig;
  auto engine = make_engine(rig.deps(), entry_options());
  REQUIRE(engine.has_value());

  const OrderIntent intent = make_entry();
  const PreflightInputs in = rig.inputs();
  REQUIRE(engine.value().preflight_entry(intent, in).ok());

  rig.kill_state.apply(KillCommand{KillType::Soft, ""});
  const StageResult blocked = engine.value().preflight_entry(intent, in);
  REQUIRE_FALSE(blocked.ok());
  CHECK(blocked.stage == comp::stage::kPosture);
  CHECK(says(blocked, "posture"));
  // The posture — not the gate's kill_entry_block — is what spoke: the kill
  // reaches the entry through ONE authority, before any other stage runs.
  CHECK_FALSE(says(blocked, "gate:"));

  // Exits stay open under a soft kill. That is the whole point of a soft kill.
  const OrderIntent exit_intent = make_stop_exit(9'500, 9'600);
  CHECK(engine.value().preflight_exit(exit_intent, in).ok());
}

TEST_CASE("engine: PANIC closes the normal exit gate (emergency runs out-of-band)",
          "[composition][engine][AC4]") {
  Rig rig;
  auto engine = make_engine(rig.deps(), entry_options());
  REQUIRE(engine.has_value());
  rig.kill_state.apply(KillCommand{KillType::Panic, ""});

  const StageResult blocked =
      engine.value().preflight_exit(make_stop_exit(9'500, 9'600), rig.inputs());
  REQUIRE_FALSE(blocked.ok());
  CHECK(blocked.stage == comp::stage::kPosture);
  CHECK(blocked.result.error().action == SuggestedAction::SquareOff);
  CHECK(says(blocked, "OUT OF BAND"));
}

// ═══════════════════════════════════════════════════════════════════════════
// AC-3 — preflight_entry: every stage blocks on its own, and names itself.
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("engine: an ExitOnly assembly REFUSES an entry at the engine-mode stage",
          "[composition][engine][AC3]") {
  Rig rig;
  EngineDeps deps = rig.deps();
  deps.funds_view = nullptr;
  deps.calendar = nullptr;
  deps.risk_engine = nullptr;
  deps.unknown_pause = nullptr;
  deps.duplicate_probe = nullptr;
  deps.margin_inputs = nullptr;
  auto engine = make_engine(deps, exit_only_options());
  REQUIRE(engine.has_value());

  SECTION("with a clean posture") {
    const StageResult refused = engine.value().preflight_entry(make_entry(), rig.inputs());
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.stage == comp::stage::kEngineMode);
    CHECK(refused.result.error().category == ErrorCategory::NotSupported);
    CHECK(says(refused, "ExitOnly"));
    // It must NOT claim the posture blocked it — the posture here is Normal.
    CHECK(engine.value().evaluate_posture() == Posture::Normal);
    CHECK_FALSE(says(refused, "posture"));
  }

  SECTION("and STILL at engine-mode when the posture is ALSO blocking") {
    // The refusal order matters: an ExitOnly engine has no entry pipeline for a
    // posture verdict to even be about, so "engine-mode" is the honest answer
    // whether or not the posture would have blocked anyway.
    rig.kill_state.apply(KillCommand{KillType::Panic, ""});
    rig.signals = {DetectorSignal::BrokerDown};
    REQUIRE(engine.value().evaluate_posture() == Posture::Panic);

    const StageResult refused = engine.value().preflight_entry(make_entry(), rig.inputs());
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.stage == comp::stage::kEngineMode);
  }
}

TEST_CASE("engine: preflight_entry — each stage blocks individually and names itself",
          "[composition][engine][AC3][AC5]") {
  Rig rig;
  auto engine = make_engine(rig.deps(), entry_options());
  REQUIRE(engine.has_value());
  const EngineAssembly& eng = engine.value();
  const PreflightInputs in = rig.inputs();

  SECTION("posture") {
    rig.signals = {DetectorSignal::MuteFeed};
    const StageResult r = eng.preflight_entry(make_entry(), in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kPosture);
    CHECK(says(r, "preflight[posture]"));
  }

  SECTION("session — an explicit NeedsReauth state") {
    rig.session = SessionSnapshot{SessionState::NeedsReauth, ""};
    const StageResult r = eng.preflight_entry(make_entry(), in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kSession);
    CHECK(r.result.error().category == ErrorCategory::SessionExpired);
    CHECK(r.result.error().action == SuggestedAction::ReEstablishSession);
  }

  SECTION("session — a token that died MID-session overrides a Healthy caller") {
    // The caller still believes the session is Healthy; the broker text says
    // otherwise, and the classifier is the single authority on that.
    rig.session = SessionSnapshot{SessionState::Healthy, "Token is invalid or has expired"};
    const StageResult r = eng.preflight_entry(make_entry(), in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kSession);
    // The UNTRUSTED broker text is never echoed back out.
    CHECK_FALSE(says(r, "Token is invalid"));
  }

  SECTION("instrument — reference data that describes a DIFFERENT contract") {
    PreflightInputs wrong = in;
    wrong.instrument.symbol = "NIFTY26JUL24500PE";
    const StageResult r = eng.preflight_entry(make_entry(), wrong);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kInstrument);
    CHECK(is_wiring_refusal(r.result.error()));
    CHECK(says(r, "wrong contract"));
  }

  SECTION("gate — the duplicate check") {
    rig.duplicate = true;
    const StageResult r = eng.preflight_entry(make_entry(), in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kGate);
    CHECK(r.result.error().category == ErrorCategory::DuplicateOrder);
    // BOTH names survive: the engine stage AND the gate's own check name.
    CHECK(says(r, "preflight[gate]"));
    CHECK(says(r, "gate: duplicate check failed"));
  }

  SECTION("gate — the UNKNOWN pause") {
    rig.unknown_pause = true;
    const StageResult r = eng.preflight_entry(make_entry(), in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kGate);
    CHECK(says(r, "UNKNOWN-pause"));
  }

  SECTION("gate — the REAL calendar's entry window") {
    rig.clk.set_wall(utc_at(2026, 6, 29, 10, 0));  // IST 15:30, past the 15:00 cutoff
    const StageResult r = eng.preflight_entry(make_entry(), in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kGate);
    CHECK(r.result.error().category == ErrorCategory::MarketClosed);
    CHECK(says(r, "gate: time-window check failed"));
  }

  SECTION("gate — the REAL funds view, fail-closed on stale") {
    rig.funds_fetch_fails = true;
    const StageResult r = eng.preflight_entry(make_entry(), in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kGate);
    CHECK(r.result.error().category == ErrorCategory::DataStale);
    CHECK(says(r, "gate: funds check failed"));
  }

  SECTION("gate — the REAL risk engine") {
    PreflightInputs risky = in;
    risky.risk_limits.max_open_positions = 1;
    risky.risk_state.open_positions = 1;
    const StageResult r = eng.preflight_entry(make_entry(), risky);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kGate);
    CHECK(r.result.error().category == ErrorCategory::RiskRejected);
    CHECK(says(r, "gate: risk check failed"));
  }

  SECTION("gate — the hedge check, when one is wired") {
    rig.wire_hedge = true;
    rig.hedge_blocks = true;
    EngineOptions options = entry_options();
    options.declares_no_hedge_check = false;
    auto hedged = make_engine(rig.deps(), options);
    REQUIRE(hedged.has_value());
    const StageResult r = hedged.value().preflight_entry(make_entry(), in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kGate);
    CHECK(says(r, "gate: hedge check failed"));
  }

  SECTION("duplicate-probe — a probe that cannot ANSWER blocks, with its own error") {
    rig.duplicate_probe_fails = true;
    const StageResult r = eng.preflight_entry(make_entry(), in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kDuplicateProbe);
    // The store's REAL error survives; it is not laundered into a fabricated
    // "already seen" DuplicateOrder verdict.
    CHECK(r.result.error().category == ErrorCategory::Internal);
    CHECK(says(r, "projection read failed"));
    CHECK_FALSE(says(r, "has already been seen"));
  }

  SECTION("duplicate-probe BEATS a gate check that would fail later") {
    // The gate short-circuits at its duplicate check (#3), so the funds failure
    // (#11) never runs — but even the duplicate verdict the gate DID produce is
    // discarded in favour of the probe's real reason.
    rig.duplicate_probe_fails = true;
    rig.funds_fetch_fails = true;
    const StageResult r = eng.preflight_entry(make_entry(), in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kDuplicateProbe);
    CHECK(r.result.error().category == ErrorCategory::Internal);
    CHECK_FALSE(says(r, "funds check"));
  }

  SECTION("a gate check that fires EARLIER than the probe still wins") {
    // UNKNOWN-pause is check #2; the duplicate probe is #3 and is never called,
    // so there is no probe error to prefer and the gate's own verdict stands.
    rig.duplicate_probe_fails = true;
    rig.unknown_pause = true;
    const StageResult r = eng.preflight_entry(make_entry(), in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kGate);
    CHECK(says(r, "UNKNOWN-pause"));
  }

  SECTION("price-band — an out-of-band ENTRY is BLOCKED") {
    const StageResult r = eng.preflight_entry(make_entry(/*price_paise=*/20'000), in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kPriceBand);
    CHECK(says(r, "preflight[price-band]"));
  }

  SECTION("margin — the BUFFER blocks what the raw broker figure would have allowed") {
    // Raw requirement 50 000; available 51 000 — the gate's funds check PASSES.
    // The buffered requirement is 52 500, so the margin stage blocks. This is the
    // whole reason marginsafety exists, proven through the composed engine.
    rig.funds_available_paise = 5'100'000;
    const StageResult r = eng.preflight_entry(make_entry(), in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kMargin);
    CHECK(r.result.error().category == ErrorCategory::RiskRejected);
  }

  SECTION("margin — the FRESH funds view overrides a rosy 'available' from the source") {
    rig.funds_available_paise = 5'100'000;
    rig.margin.available = Money::from_paise(999'000'000);  // a lie the source told
    const StageResult r = eng.preflight_entry(make_entry(), in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kMargin);
  }
}

TEST_CASE("engine: preflight_entry stage ORDER — the earliest armed stage wins",
          "[composition][engine][AC3][AC5]") {
  Rig rig;
  auto engine = make_engine(rig.deps(), entry_options());
  REQUIRE(engine.has_value());
  const EngineAssembly& eng = engine.value();

  // Arm EVERY stage at once, then disarm them one at a time. Each step must
  // surface the NEXT stage in the documented order — membership alone would not
  // prove this.
  rig.signals = {DetectorSignal::StaleData};                           // posture
  rig.session = SessionSnapshot{SessionState::NeedsReauth, ""};        // session
  rig.margin_quote_fails = true;                                       // margin (quote)
  rig.duplicate = true;                                                // gate
  rig.funds_available_paise = 5'100'000;                               // margin (buffered)
  PreflightInputs in = rig.inputs();
  in.instrument.symbol = "NIFTY26JUL24500PE";                          // instrument
  in.risk_limits = RiskLimits{};                                       // risk-limits
  const OrderIntent out_of_band = make_entry(/*price_paise=*/20'000);  // price-band

  CHECK(eng.preflight_entry(out_of_band, in).stage == comp::stage::kPosture);

  rig.signals.clear();
  CHECK(eng.preflight_entry(out_of_band, in).stage == comp::stage::kSession);

  rig.session = SessionSnapshot{SessionState::Healthy, ""};
  CHECK(eng.preflight_entry(out_of_band, in).stage == comp::stage::kInstrument);

  in.instrument.symbol = kSymbol;
  CHECK(eng.preflight_entry(out_of_band, in).stage == comp::stage::kRiskLimits);

  in.risk_limits.max_open_positions = 10;
  {
    // The margin QUOTE precedes the gate: it is the number the gate's funds check
    // is sized against, so a quote that cannot be obtained fails before the gate
    // can even be assembled.
    const StageResult r = eng.preflight_entry(out_of_band, in);
    CHECK(r.stage == comp::stage::kMargin);
    CHECK(says(r, "could not be obtained"));
  }

  rig.margin_quote_fails = false;
  CHECK(eng.preflight_entry(out_of_band, in).stage == comp::stage::kGate);

  rig.duplicate = false;
  CHECK(eng.preflight_entry(out_of_band, in).stage == comp::stage::kPriceBand);

  in.band = PriceBand{Money::from_paise(9'000), Money::from_paise(30'000), true};
  {
    const StageResult r = eng.preflight_entry(out_of_band, in);
    CHECK(r.stage == comp::stage::kMargin);
    // The BUFFERED half of the margin stage this time, not the quote.
    CHECK_FALSE(says(r, "could not be obtained"));
  }

  rig.funds_available_paise = 6'000'000;
  // The funds view has already CACHED the old figure and its cadence has not
  // elapsed (the test clock never advances), so a refetch has to be asked for —
  // exactly what the runtime does after a fill.
  rig.funds.invalidate();
  const StageResult clean = eng.preflight_entry(out_of_band, in);
  REQUIRE(clean.ok());
  CHECK(clean.stage == comp::stage::kMargin);  // the LAST stage to run, on success
}

TEST_CASE("engine: a clean entry traverses every stage and reports the evidence",
          "[composition][engine][AC3][AC5]") {
  Rig rig;
  auto engine = make_engine(rig.deps(), entry_options());
  REQUIRE(engine.has_value());

  const StageResult r = engine.value().preflight_entry(make_entry(), rig.inputs());
  REQUIRE(r.ok());
  const comp::PreflightOutcome& out = r.result.value();

  CHECK(out.gate_outcome == GateOutcome::Allow);
  CHECK_FALSE(out.session_alert);
  CHECK(out.band_checked);
  CHECK(out.band.verdict == BandVerdict::WithinBand);
  CHECK_FALSE(out.band_unvalidated);
  CHECK_FALSE(out.exit_clamped);
  CHECK(out.margin_checked);
  CHECK_FALSE(out.margin.blocked);
  // 50 000 + 5% = 52 500 — the buffer was actually applied, not skipped.
  CHECK(out.margin.effective_required == Money::from_paise(5'250'000));
  CHECK_FALSE(out.modify_checked);
}

TEST_CASE("engine: an over-freeze entry survives as AllowWithSlicing",
          "[composition][engine][AC3]") {
  Rig rig;
  auto engine = make_engine(rig.deps(), entry_options());
  REQUIRE(engine.has_value());

  OrderIntent big = make_entry();
  big.quantity = Quantity::of(2'000);  // freeze is 1 800, lot-aligned at 50
  const StageResult r = engine.value().preflight_entry(big, rig.inputs());
  REQUIRE(r.ok());
  CHECK(r.result.value().gate_outcome == GateOutcome::AllowWithSlicing);
}

TEST_CASE("engine: a missing price band flags, and blocks only when configured to",
          "[composition][engine][AC3][LOW]") {
  Rig rig;
  PreflightInputs in = rig.inputs();
  in.band = PriceBand{};  // known == false

  SECTION("default: a band-feed outage must not become a trading freeze") {
    auto engine = make_engine(rig.deps(), entry_options());
    REQUIRE(engine.has_value());
    const StageResult r = engine.value().preflight_entry(make_entry(), in);
    REQUIRE(r.ok());
    CHECK(r.result.value().band_unvalidated);
    CHECK(r.result.value().band.verdict == BandVerdict::BandUnknown);
  }

  SECTION("block_entry_on_unknown_band: an unvalidated entry price is refused") {
    EngineOptions options = entry_options();
    options.block_entry_on_unknown_band = true;
    auto engine = make_engine(rig.deps(), options);
    REQUIRE(engine.has_value());

    const StageResult r = engine.value().preflight_entry(make_entry(), in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kPriceBand);
    CHECK(r.result.error().category == ErrorCategory::DataStale);
    CHECK(says(r, "block_entry_on_unknown_band"));

    // EXITS ARE NEVER BLOCKED BY IT — the flag is entry-only by construction.
    CHECK(engine.value().preflight_exit(make_stop_exit(9'500, 9'600), in).ok());
  }
}

TEST_CASE("engine: a degraded session is SURFACED even when it does not block",
          "[composition][engine][LOW]") {
  Rig rig;
  auto engine = make_engine(rig.deps(), entry_options());
  REQUIRE(engine.has_value());
  const PreflightInputs in = rig.inputs();

  // A dead token never blocks an exit — but swallowing the alert would lose the
  // one signal that says "re-establish soon".
  rig.session = SessionSnapshot{SessionState::NeedsReauth, ""};
  const StageResult exited = engine.value().preflight_exit(make_stop_exit(9'500, 9'600), in);
  REQUIRE(exited.ok());
  CHECK(exited.result.value().session_alert);

  // A modify travels the same chain and reports it too.
  const OrderIntent current = make_stop_exit(9'500, 9'600);
  OrderIntent amended = current;
  amended.price = Price::from_paise(9'400);
  ModifyContext mods;
  mods.current_state = {OrderState::Acknowledged, 0, 50};
  const StageResult modified = engine.value().preflight_modify(current, amended, mods, in);
  REQUIRE(modified.ok());
  CHECK(modified.result.value().session_alert);

  // A Healthy session raises nothing.
  rig.session = SessionSnapshot{SessionState::Healthy, ""};
  const StageResult entered = engine.value().preflight_entry(make_entry(), in);
  REQUIRE(entered.ok());
  CHECK_FALSE(entered.result.value().session_alert);
}

// ═══════════════════════════════════════════════════════════════════════════
// AC-3 — preflight_exit: never blocked by an entry-only stage.
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("engine: an exit is never blocked by ANY entry-only stage",
          "[composition][engine][AC3][AC5]") {
  Rig rig;
  auto engine = make_engine(rig.deps(), entry_options());
  REQUIRE(engine.has_value());

  // Arm every entry-only block simultaneously.
  rig.kill_state.apply(KillCommand{KillType::Account, "acct-1"});  // kill: entries blocked
  rig.session = SessionSnapshot{SessionState::NeedsReauth, ""};    // dead token
  rig.unknown_pause = true;                                        // UNKNOWN pause
  rig.duplicate = true;                                            // duplicate signal
  rig.funds_available_paise = 0;                                   // no funds at all
  rig.funds_fetch_fails = true;                                    // and the view is stale
  rig.margin_quote_fails = true;                                   // and no margin quote
  rig.clk.set_wall(utc_at(2026, 6, 29, 10, 0));                    // past the entry cutoff

  PreflightInputs in = rig.inputs();
  in.risk_limits = RiskLimits{};  // and an all-off limit set

  const OrderIntent exit_intent = make_stop_exit(9'500, 9'600);
  const StageResult r = engine.value().preflight_exit(exit_intent, in);
  REQUIRE(r.ok());
  CHECK(r.result.value().gate_outcome == GateOutcome::Allow);
  // No margin stage ran, and the caller can TELL (the default is blocked==true).
  CHECK_FALSE(r.result.value().margin_checked);

  // The same conditions refuse an entry — the asymmetry is real, not incidental.
  CHECK_FALSE(engine.value().preflight_entry(make_entry(), in).ok());
}

TEST_CASE("engine: an exit is still SHAPE-validated", "[composition][engine][AC3][AC5]") {
  Rig rig;
  auto engine = make_engine(rig.deps(), entry_options());
  REQUIRE(engine.has_value());

  SECTION("a stop with no trigger is a broker rejection, not protection") {
    OrderIntent malformed = make_stop_exit(9'500, 9'600);
    malformed.trigger_price.reset();
    const StageResult r = engine.value().preflight_exit(malformed, rig.inputs());
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kGate);
    CHECK(says(r, "gate: order-shape check failed"));
  }

  SECTION("a SELL stop whose limit sits ABOVE its trigger would never fill") {
    const StageResult r =
        engine.value().preflight_exit(make_stop_exit(9'700, 9'600), rig.inputs());
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kGate);
    CHECK(says(r, "gate: order-shape check failed"));
  }

  SECTION("a mis-ticked exit is still refused") {
    const StageResult r =
        engine.value().preflight_exit(make_stop_exit(9'503, 9'600), rig.inputs());
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kGate);
    CHECK(says(r, "gate: tick check failed"));
  }

  SECTION("and instrument-checked: an exit against the wrong contract is refused") {
    PreflightInputs wrong = rig.inputs();
    wrong.instrument.symbol = "NIFTY26JUL24500PE";
    const StageResult r = engine.value().preflight_exit(make_stop_exit(9'500, 9'600), wrong);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kInstrument);
  }
}

TEST_CASE("engine: an out-of-band EXIT is CLAMPED, never blocked",
          "[composition][engine][AC3][AC5]") {
  Rig rig;
  auto engine = make_engine(rig.deps(), entry_options());
  REQUIRE(engine.has_value());

  // Band [90.00, 110.00]; the protective stop is priced well below the floor.
  OrderIntent stop = make_stop_exit(8'000, 8'500);
  const StageResult r = engine.value().preflight_exit(stop, rig.inputs());
  REQUIRE(r.ok());
  const comp::PreflightOutcome& out = r.result.value();
  CHECK(out.band.verdict == BandVerdict::OutsideBandExitClamped);
  CHECK(out.exit_clamped);
  // BOTH prices of a stop-limit are pulled inside — clamping only the limit would
  // still earn the "price out of LPP range" rejection on the trigger.
  REQUIRE(out.band.has_suggestion);
  REQUIRE(out.band.has_trigger_suggestion);
  CHECK(out.band.suggested_limit == Money::from_paise(9'000));
  CHECK(out.band.suggested_trigger == Money::from_paise(9'000));

  // The clamp is an OBLIGATION, and apply_clamp discharges BOTH halves of it.
  REQUIRE(comp::apply_clamp(stop, out));
  CHECK(stop.price == Price::from_paise(9'000));
  REQUIRE(stop.trigger_price.has_value());
  CHECK(*stop.trigger_price == Price::from_paise(9'000));

  // The clamped order now passes its own band check unchanged.
  const StageResult again = engine.value().preflight_exit(stop, rig.inputs());
  REQUIRE(again.ok());
  CHECK(again.result.value().band.verdict == BandVerdict::WithinBand);
  CHECK_FALSE(again.result.value().exit_clamped);
}

TEST_CASE("engine: apply_clamp is a safe no-op when there is nothing to apply",
          "[composition][engine][LOW]") {
  Rig rig;
  auto engine = make_engine(rig.deps(), entry_options());
  REQUIRE(engine.has_value());

  OrderIntent stop = make_stop_exit(9'500, 9'600);
  const OrderIntent before = stop;
  const StageResult r = engine.value().preflight_exit(stop, rig.inputs());
  REQUIRE(r.ok());
  CHECK_FALSE(comp::apply_clamp(stop, r.result.value()));
  CHECK(stop == before);

  // …and on an outcome no band stage ever populated.
  const comp::PreflightOutcome untouched{};
  CHECK_FALSE(comp::apply_clamp(stop, untouched));
  CHECK(stop == before);
}

TEST_CASE("engine: an EXIT-ONLY assembly runs the exit chain",
          "[composition][engine][AC3]") {
  Rig rig;
  EngineDeps deps = rig.deps();
  deps.funds_view = nullptr;
  deps.calendar = nullptr;
  deps.risk_engine = nullptr;
  deps.unknown_pause = nullptr;
  deps.duplicate_probe = nullptr;
  deps.margin_inputs = nullptr;
  auto engine = make_engine(deps, exit_only_options());
  REQUIRE(engine.has_value());

  CHECK(engine.value().preflight_exit(make_stop_exit(9'500, 9'600), rig.inputs()).ok());
}

TEST_CASE("engine: the exit claim is asserted by default and CHECKED when wired",
          "[composition][engine][MEDIUM5]") {
  Rig rig;
  // An OPENING order, handed to preflight_exit. It would collect every entry-only
  // exemption the exit chain grants.
  const OrderIntent opening = make_entry();

  SECTION("absent classifier: the claim is a documented caller assertion") {
    auto engine = make_engine(rig.deps(), entry_options());
    REQUIRE(engine.has_value());
    CHECK(engine.value().preflight_exit(opening, rig.inputs()).ok());
  }

  SECTION("wired and false: refused at the exit-class stage") {
    rig.wire_is_reducing = true;
    rig.intent_is_reducing = false;
    auto engine = make_engine(rig.deps(), entry_options());
    REQUIRE(engine.has_value());

    const StageResult r = engine.value().preflight_exit(opening, rig.inputs());
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kExitClass);
    CHECK(is_wiring_refusal(r.result.error()));
    CHECK(says(r, "does NOT reduce"));
    CHECK(says(r, "preflight_entry"));
  }

  SECTION("wired and true: a genuine exit passes") {
    rig.wire_is_reducing = true;
    rig.intent_is_reducing = true;
    auto engine = make_engine(rig.deps(), entry_options());
    REQUIRE(engine.has_value());
    CHECK(engine.value().preflight_exit(make_stop_exit(9'500, 9'600), rig.inputs()).ok());
  }

  SECTION("exit-class runs FIRST — before the posture that would also block") {
    rig.wire_is_reducing = true;
    rig.intent_is_reducing = false;
    auto engine = make_engine(rig.deps(), entry_options());
    REQUIRE(engine.has_value());
    rig.kill_state.apply(KillCommand{KillType::Panic, ""});

    const StageResult r = engine.value().preflight_exit(opening, rig.inputs());
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kExitClass);
  }

  SECTION("a MODIFY is not required to reduce — re-pricing a working entry is legal") {
    rig.wire_is_reducing = true;
    rig.intent_is_reducing = false;
    auto engine = make_engine(rig.deps(), entry_options());
    REQUIRE(engine.has_value());

    OrderIntent amended = opening;
    amended.price = Price::from_paise(10'500);
    ModifyContext mods;
    mods.current_state = {OrderState::Acknowledged, 0, 50};
    CHECK(engine.value().preflight_modify(opening, amended, mods, rig.inputs()).ok());
  }
}

TEST_CASE("engine: risk gates an exit ONLY when the operator arms it",
          "[composition][engine][AC3]") {
  Rig rig;
  PreflightInputs in = rig.inputs();
  in.risk_limits.daily_loss_limit_paise = 1'000;
  in.risk_state.account_pnl_paise = -50'000;  // the limit is breached
  const OrderIntent exit_intent = make_stop_exit(9'500, 9'600);

  SECTION("default: a breached risk limit does NOT strand the protective leg") {
    auto engine = make_engine(rig.deps(), entry_options());
    REQUIRE(engine.has_value());
    CHECK(engine.value().preflight_exit(exit_intent, in).ok());
  }

  SECTION("opt-in: risk_gates_exits presents the check to the gate") {
    EngineOptions options = entry_options();
    options.risk_gates_exits = true;
    auto engine = make_engine(rig.deps(), options);
    REQUIRE(engine.has_value());
    const StageResult r = engine.value().preflight_exit(exit_intent, in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kGate);
    CHECK(says(r, "gate: risk check failed"));
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// AC-3 — preflight_modify delegates to modifyguard.
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("engine: preflight_modify delegates to the modify guard",
          "[composition][engine][AC3][AC5]") {
  Rig rig;
  auto engine = make_engine(rig.deps(), entry_options());
  REQUIRE(engine.has_value());
  const EngineAssembly& eng = engine.value();
  const PreflightInputs in = rig.inputs();

  const OrderIntent current = make_stop_exit(9'500, 9'600);

  SECTION("a QUANTITY modify on a PARTIALLY-FILLED order is refused") {
    OrderIntent amended = current;
    amended.quantity = Quantity::of(100);
    ModifyContext mods;
    mods.current_state = {OrderState::PartiallyFilled, 25, 50};
    mods.observed_filled_qty = 25;

    const StageResult r = eng.preflight_modify(current, amended, mods, in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kModifyGuard);
    // The module's own stable verdict name survives into the error message.
    CHECK(says(r, "reject_qty_on_partial"));
    CHECK(r.result.error().category == ErrorCategory::RiskRejected);
    CHECK(r.result.error().action == SuggestedAction::DoNotRetry);
  }

  SECTION("shrinking the TOTAL to at-or-below the filled qty is refused") {
    OrderIntent amended = current;
    amended.quantity = Quantity::of(20);
    ModifyContext mods;
    mods.current_state = {OrderState::Acknowledged, 25, 50};
    mods.observed_filled_qty = 25;

    const StageResult r = eng.preflight_modify(current, amended, mods, in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kModifyGuard);
    CHECK(says(r, "reject_shrink_below_filled"));
  }

  SECTION("a fill that RACED IN since the decision forces a reconcile") {
    OrderIntent amended = current;
    amended.price = Price::from_paise(9'400);
    ModifyContext mods;
    mods.current_state = {OrderState::Acknowledged, 25, 50};
    mods.observed_filled_qty = 0;  // the caller decided when nothing was filled

    const StageResult r = eng.preflight_modify(current, amended, mods, in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kModifyGuard);
    CHECK(says(r, "reject_raced_fill"));
    CHECK(r.result.error().action == SuggestedAction::ReconcileFirst);
  }

  SECTION("a terminal order maps to OrderNotFound") {
    OrderIntent amended = current;
    amended.price = Price::from_paise(9'400);
    ModifyContext mods;
    mods.current_state = {OrderState::Filled, 50, 50};
    mods.observed_filled_qty = 50;

    const StageResult r = eng.preflight_modify(current, amended, mods, in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kModifyGuard);
    CHECK(r.result.error().category == ErrorCategory::OrderNotFound);
  }

  SECTION("a TRIGGER-only move is DIFFED out, not hand-asserted") {
    // Pre-IMP-11 a caller who moved only the trigger could present a request with
    // neither flag set and have the guard Allow a modify it should refuse. The
    // diff computes changes_price from the trigger, so the partial is protected.
    OrderIntent amended = current;
    amended.trigger_price = Price::from_paise(9'700);
    ModifyContext mods;
    mods.current_state = {OrderState::PartiallyFilled, 25, 50};
    mods.observed_filled_qty = 25;

    // Price-only on a partial IS legal — the guard only refuses QUANTITY there.
    const StageResult r = eng.preflight_modify(current, amended, mods, in);
    REQUIRE(r.ok());
    CHECK(r.result.value().modify_checked);
    CHECK(r.result.value().modify.verdict == broker_exec::modifyguard::ModifyVerdict::Allow);
  }

  SECTION("an ALLOWED modify still runs the exit-legal chain on the AMENDED intent") {
    // The amended order is malformed (a SELL stop whose limit sits above its
    // trigger). The guard allows the modify; the chain must still refuse it.
    OrderIntent amended = current;
    amended.price = Price::from_paise(9'700);
    ModifyContext mods;
    mods.current_state = {OrderState::Acknowledged, 0, 50};
    mods.observed_filled_qty = 0;

    const StageResult r = eng.preflight_modify(current, amended, mods, in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.stage == comp::stage::kGate);
    CHECK(says(r, "gate: order-shape check failed"));
  }

  SECTION("an allowed, well-formed modify passes and carries both evidence trails") {
    OrderIntent amended = current;
    amended.price = Price::from_paise(9'400);
    amended.trigger_price = Price::from_paise(9'450);
    ModifyContext mods;
    mods.current_state = {OrderState::Acknowledged, 0, 50};
    mods.observed_filled_qty = 0;

    const StageResult r = eng.preflight_modify(current, amended, mods, in);
    REQUIRE(r.ok());
    CHECK(r.result.value().modify_checked);
    CHECK(r.result.value().band_checked);
    CHECK(r.result.value().band.verdict == BandVerdict::WithinBand);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// AC-5 — the integration smoke.
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("engine: integration smoke — one entry intent end-to-end over real modules",
          "[composition][engine][AC5][integration]") {
  // Everything below is the REAL implementation. The ONLY fakes are the injected
  // ClockPort and the two fetch seams (calendar JSON, funds snapshot) — exactly
  // the seams the production wiring binds to a broker.
  TempDir dir;
  TestClock clk{std::chrono::steady_clock::time_point{}, utc_at(2026, 6, 29, 3, 50)};

  TradingCalendar calendar([]() -> Result<std::string> { return std::string(kCalendarJson); }, clk,
                           dir.path, "kite");
  REQUIRE(calendar.refresh().has_value());

  int funds_fetches = 0;
  FundsView funds(
      clk,
      [&funds_fetches]() -> Result<FundsSnapshot> {
        ++funds_fetches;
        FundsSnapshot snapshot;
        snapshot.available_margin = Money::from_paise(6'000'000);
        return snapshot;
      },
      std::chrono::seconds(30));

  const RiskEngine risk{};
  KillState kill_state;
  const PostureCoordinator coordinator{};

  int probes = 0;
  int margin_quotes = 0;

  EngineDeps deps;
  deps.kill_state = &kill_state;
  deps.posture = &coordinator;
  deps.detector_signals = []() { return std::vector<DetectorSignal>{}; };
  deps.session = []() { return SessionSnapshot{SessionState::Healthy, ""}; };
  deps.funds_view = &funds;
  deps.calendar = &calendar;
  deps.risk_engine = &risk;
  deps.unknown_pause = []() { return false; };
  deps.duplicate_probe = [&probes](const OrderIntent&) -> Result<bool> {
    ++probes;
    return false;
  };
  deps.margin_inputs = [&margin_quotes](const OrderIntent&) -> Result<mgn::MarginInputs> {
    ++margin_quotes;
    mgn::MarginInputs quote;
    quote.api_required = Money::from_paise(5'000'000);
    return quote;
  };

  EngineOptions options;
  options.mode = EngineMode::EntryCapable;
  options.allowed_exchanges = {"NFO"};
  options.allowed_products = {Product::Intraday};
  options.declares_no_hedge_check = true;

  auto engine = make_engine(deps, options);
  REQUIRE(engine.has_value());

  PreflightInputs in;
  in.instrument = make_instrument();
  in.band = PriceBand{Money::from_paise(9'000), Money::from_paise(11'000), true};
  in.risk_limits.max_order_value_paise = 1'000'000;
  in.risk_state.order_value_paise = 500'000;
  in.risk_state.order_value_known = true;

  const StageResult r = engine.value().preflight_entry(make_entry(), in);
  REQUIRE(r.ok());

  const comp::PreflightOutcome& out = r.result.value();
  CHECK(out.gate_outcome == GateOutcome::Allow);
  CHECK(out.band_checked);
  CHECK(out.band.verdict == BandVerdict::WithinBand);
  CHECK(out.margin_checked);
  CHECK_FALSE(out.margin.blocked);
  CHECK(out.margin.effective_required == Money::from_paise(5'250'000));

  // The seams were each consulted exactly once for this intent — no stage
  // double-quotes the broker, and the funds view's cadence cache means the gate's
  // funds check and the margin stage share ONE fetch.
  CHECK(probes == 1);
  CHECK(margin_quotes == 1);
  CHECK(funds_fetches == 1);

  // Same wiring, one flipped operator switch: the entry is refused end-to-end
  // while the protective exit still goes through. A Strategy kill is SCOPED, so
  // it stops 'alpha' and leaves 'beta' trading.
  kill_state.apply(KillCommand{KillType::Strategy, "alpha"});
  CHECK_FALSE(engine.value().preflight_entry(make_entry(10'000, "alpha"), in).ok());
  CHECK(engine.value().preflight_entry(make_entry(10'000, "beta"), in).ok());
  CHECK(engine.value().preflight_exit(make_stop_exit(9'500, 9'600), in).ok());
}
