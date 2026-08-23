// broker_exec::boot unit tests (IMP-20).
//
// WHAT IS UNDER TEST: the boot LOGIC, not the binary. `main()` is a thin shell
// over `boot()`, so the sequence, the fail-closed short-circuiting, the ten-check
// wiring and the exit-code contract are all exercised here as ordinary values.
//
// THE COMPONENTS ARE REAL. The store is a real `store::Store`, the ledger a real
// `ledger::Ledger` over a temp file, the instrument master and calendar are real
// `refdata` objects driven through their genuine fetch seams, the clock check
// runs a real `clock::SkewStallDetector`, and the broker/engine assemblies come
// out of the real IMP-12 composition root. Only the OUTERMOST seams are faked —
// the HTTP transport, the secret provider, the alert sink, the session probe, the
// health-endpoint bind and the run phase — which is exactly the boundary main()
// wires differently in production. A test that stubbed the components would only
// prove the stubs work.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`, no float.

#include "broker_exec/boot/boot.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "broker_exec/adapters/kite/http_client.hpp"
#include "broker_exec/capabilities/capabilities.hpp"
#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/modes/killswitch.hpp"
#include "broker_exec/modes/posture.hpp"
#include "broker_exec/supervisor/supervisor_policy.hpp"

namespace fs = std::filesystem;
namespace boot = broker_exec::boot;
namespace comp = broker_exec::composition;
namespace caps = broker_exec::capabilities;

using broker_exec::Result;
using broker_exec::boot::BootDeps;
using broker_exec::boot::BootOutcome;
using broker_exec::boot::BootStep;
using broker_exec::boot::ExitClass;
using broker_exec::boot::SafeCheckId;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::SuggestedAction;

namespace {

// ── Scaffolding ─────────────────────────────────────────────────────────────

struct TempDir {
  fs::path path;
  TempDir() {
    std::random_device rd;
    path = fs::temp_directory_path() /
           ("brexec_boot_" + std::to_string(rd()) + "_" + std::to_string(rd()));
    fs::create_directories(path);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

// A transport that COUNTS what reaches it. Composing a broker must not issue a
// single request; if this moves during a boot, something started talking to a
// broker before the world was verified.
class CountingHttpClient final : public broker_exec::adapters::kite::HttpClient {
 public:
  mutable int calls = 0;

  [[nodiscard]] Result<broker_exec::adapters::kite::HttpResponse> send(
      const broker_exec::adapters::kite::HttpRequest&) const override {
    ++calls;
    broker_exec::adapters::kite::HttpResponse response;
    response.status_code = 503;
    response.body = R"({"status":"error","message":"stub transport"})";
    return response;
  }
};

// A secret provider over an explicit map. Nothing here is a real credential:
// the token key is 32 filler bytes, which is what the crypto-keys check measures.
class MapSecrets final : public broker_exec::ports::SecretProvider {
 public:
  std::map<std::string, std::string> values;

  [[nodiscard]] Result<std::string> get(std::string_view key) const override {
    const auto it = values.find(std::string(key));
    if (it == values.end()) {
      return broker_exec::fail(
          broker_exec::errors::make_error(ErrorCategory::Validation, "test secrets: no such key"));
    }
    return it->second;
  }
};

// Records every alert. The absence alarm is asserted against this.
class SpyAlerts final : public broker_exec::ports::AlertSink {
 public:
  struct Sent {
    broker_exec::ports::AlertLevel level;
    std::string message;
  };
  std::vector<Sent> sent;
  bool deliver = true;

  [[nodiscard]] Result<broker_exec::ports::Ok> send(broker_exec::ports::AlertLevel level,
                                                    const std::string& message) override {
    sent.push_back(Sent{level, message});
    if (!deliver) {
      return broker_exec::fail(
          broker_exec::errors::make_error(ErrorCategory::Network, "test sink: every channel down"));
    }
    return broker_exec::ports::ok();
  }

  [[nodiscard]] Result<broker_exec::ports::Ok> send_test_alert() override {
    return broker_exec::ports::ok();
  }

  [[nodiscard]] int critical_count() const {
    int count = 0;
    for (const Sent& entry : sent) {
      if (entry.level == broker_exec::ports::AlertLevel::Critical) {
        ++count;
      }
    }
    return count;
  }
};

[[nodiscard]] std::string valid_instruments_csv() {
  return "instrument_token,tradingsymbol,exchange,expiry,tick_size,lot_size\n"
         "12345,NIFTY99JUN24000CE,NFO,2099-06-27,0.05,50\n";
}

[[nodiscard]] std::string valid_calendar_json() {
  return R"({"holidays":[],"special_sessions":[],)"
         R"("windows":{"open":"09:15","entry_cutoff":"15:00",)"
         R"("square_off":"15:15","close":"15:30"}})";
}

// 32 bytes of Ed25519-public-key-shaped hex. It is never used to verify a
// signature here (there is no checkpoint), only to prove the anchor EXISTS —
// which is precisely what the crypto-keys check asserts.
[[nodiscard]] std::string pinned_key_hex() {
  return "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
}

void write_text(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

[[nodiscard]] broker_exec::domain::Order make_order(broker_exec::domain::OrderState state,
                                                    broker_exec::domain::OrderType type,
                                                    const std::string& client_ref) {
  broker_exec::domain::Order order;
  order.intent.client_ref = client_ref;
  order.intent.symbol = "NIFTY99JUN24000CE";
  order.intent.side = broker_exec::domain::Side::Buy;
  order.intent.quantity = broker_exec::domain::Quantity::of(50);
  order.intent.price = broker_exec::domain::Price::from_rupees(100, 0);
  order.intent.order_type = type;
  order.intent.product = broker_exec::domain::Product::Intraday;
  order.intent.strategy = "boot";
  order.state = state;
  return order;
}

// ── The fixture: a world in which a boot SUCCEEDS ───────────────────────────
//
// Every test starts from a passing world and breaks exactly one thing, so a
// failure names the thing that was broken rather than a setup accident.
struct World {
  static constexpr const char* kAccount = "acct-1";

  TempDir data_root;
  TempDir refdata_cache;

  broker_exec::clock::TestClock test_clock;
  broker_exec::clock::SkewStallDetector detector;
  MapSecrets secrets;
  SpyAlerts alerts;
  broker_exec::cli::HealthState health;
  CountingHttpClient kite_http;
  broker_exec::modes::KillState kill_state;
  broker_exec::modes::PostureCoordinator posture;

  std::map<std::string, std::string> env_map;
  std::optional<broker_exec::store::Store> store;
  std::unique_ptr<broker_exec::ledger::Ledger> ledger;
  std::unique_ptr<broker_exec::refdata::InstrumentMaster> instruments;
  std::unique_ptr<broker_exec::refdata::TradingCalendar> calendar;

  // Seam behaviour the tests bend.
  Result<broker_exec::session::SessionState> session_answer =
      broker_exec::session::SessionState::Healthy;
  Result<broker_exec::ports::Ok> endpoint_answer = broker_exec::ports::ok();
  Result<broker_exec::ports::Ok> run_answer = broker_exec::ports::ok();
  int session_probe_calls = 0;
  int endpoint_calls = 0;
  int run_calls = 0;
  int perms_calls = 0;

  World() {
    env_map["BROKER_EXEC_ENGINE_ACCOUNT_ID"] = kAccount;
    env_map["BROKER_EXEC_BROKER_NAME"] = "kite";
    env_map["BROKER_EXEC_PATHS_DATA_DIR"] = data_root.path.string();
    env_map[std::string(boot::kEgressIpEnv)] = "203.0.113.7";
    env_map[std::string(boot::kEgressAllowlistEnv)] = "198.51.100.4, 203.0.113.7";

    secrets.values[boot::token_key_secret_name(kAccount)] = std::string(32, 'k');

    // The account tree and the out-of-band pinned ledger key must exist before
    // boot runs — provisioning is an operator step, not something the process
    // invents for itself (ledger.hpp's trust model).
    const fs::path account_root = data_root.path / kAccount;
    fs::create_directories(account_root);
    write_text(account_root / "ledger_public_key.hex", pinned_key_hex());

    auto opened = broker_exec::store::Store::open(":memory:");
    REQUIRE(opened.has_value());
    store.emplace(std::move(opened).value());

    ledger =
        std::make_unique<broker_exec::ledger::Ledger>(test_clock, account_root / "ledger.jsonl");

    instruments = std::make_unique<broker_exec::refdata::InstrumentMaster>(
        []() -> Result<std::string> { return valid_instruments_csv(); }, test_clock,
        refdata_cache.path, "kite", "nfo");
    calendar = std::make_unique<broker_exec::refdata::TradingCalendar>(
        []() -> Result<std::string> { return valid_calendar_json(); }, test_clock,
        refdata_cache.path, "kite");
  }

  World(const World&) = delete;
  World& operator=(const World&) = delete;

  [[nodiscard]] BootDeps deps() {
    BootDeps d;
    d.args.account_id = kAccount;
    d.args.data_root = data_root.path;
    d.args.shared_refdata_root = refdata_cache.path;

    d.env = [this](std::string_view key) -> std::optional<std::string> {
      const auto it = env_map.find(std::string(key));
      if (it == env_map.end()) {
        return std::nullopt;
      }
      return it->second;
    };

    d.clock = &test_clock;
    d.secrets = &secrets;
    d.alerts = &alerts;
    d.clock_detector = &detector;
    d.store = &store.value();
    d.ledger = ledger.get();
    d.instruments = instruments.get();
    d.calendar = calendar.get();
    d.health = &health;

    d.session_probe = [this]() -> Result<broker_exec::session::SessionState> {
      ++session_probe_calls;
      return session_answer;
    };
    d.start_health_endpoint = [this]() -> Result<broker_exec::ports::Ok> {
      ++endpoint_calls;
      return endpoint_answer;
    };
    d.run_phase = [this]() -> Result<broker_exec::ports::Ok> {
      ++run_calls;
      return run_answer;
    };
    d.dir_permissions = [this](const fs::path&) {
      ++perms_calls;
      return true;
    };

    d.broker_deps.kite_http = &kite_http;
    d.broker_deps.kite_secrets = &secrets;
    d.broker_options.required_capabilities = {
        caps::Capability::PlaceOrder, caps::Capability::ModifyOrder, caps::Capability::CancelOrder,
        caps::Capability::SquareOff};

    d.engine_deps.kill_state = &kill_state;
    d.engine_deps.posture = &posture;
    d.engine_deps.detector_signals = []() {
      return std::vector<broker_exec::modes::DetectorSignal>{};
    };
    d.engine_deps.session = []() {
      comp::SessionSnapshot snapshot;
      snapshot.state = broker_exec::session::SessionState::Healthy;
      return snapshot;
    };
    // ExitOnly is EngineOptions' default and the only mode this boot path can
    // honestly declare: the entry-safety sources (funds view, risk engine,
    // duplicate probe, margin quote) belong to the trading loop that IMP-20 does
    // not build.
    d.engine_options.declares_no_hedge_check = true;
    return d;
  }
};

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

}  // namespace

// ── The exit-code contract ──────────────────────────────────────────────────

TEST_CASE("every exit class round-trips through supervisor::exit_reason_from_code",
          "[boot][exit-code]") {
  namespace sup = broker_exec::supervisor;

  CHECK(boot::exit_code_for(ExitClass::Clean) == sup::kExitClean);
  CHECK(boot::exit_code_for(ExitClass::FailClosedNeedsHuman) == sup::kExitFailClosedNeedsHuman);
  CHECK(boot::exit_code_for(ExitClass::FailClosedNeedsHuman) == 70);
  CHECK(boot::exit_code_for(ExitClass::Crash) != sup::kExitClean);
  CHECK(boot::exit_code_for(ExitClass::Crash) != sup::kExitFailClosedNeedsHuman);

  CHECK(sup::exit_reason_from_code(boot::exit_code_for(ExitClass::Clean)) ==
        sup::ExitReason::CleanShutdown);
  CHECK(sup::exit_reason_from_code(boot::exit_code_for(ExitClass::FailClosedNeedsHuman)) ==
        sup::ExitReason::FailClosedNeedsHuman);
  CHECK(sup::exit_reason_from_code(boot::exit_code_for(ExitClass::Crash)) ==
        sup::ExitReason::Crash);
}

TEST_CASE("70 means NO auto-restart and an absence alarm; the crash class restarts",
          "[boot][exit-code]") {
  // The committed systemd unit asserts RestartPreventExitStatus=70. This pins the
  // library half of that claim: 70 must decide NoRestartEscalate + alarm.
  namespace sup = broker_exec::supervisor;
  const sup::BackoffConfig cfg;

  const sup::SupervisorDecision fail_closed = sup::decide(
      sup::exit_reason_from_code(boot::exit_code_for(ExitClass::FailClosedNeedsHuman)), 1, cfg);
  CHECK(fail_closed.action == sup::SupervisorAction::NoRestartEscalate);
  CHECK(fail_closed.raise_absence_alarm);

  const sup::SupervisorDecision crash =
      sup::decide(sup::exit_reason_from_code(boot::exit_code_for(ExitClass::Crash)), 1, cfg);
  CHECK(crash.action == sup::SupervisorAction::RestartWithBackoff);
  CHECK_FALSE(crash.raise_absence_alarm);
}

TEST_CASE("transport failures are the crash class; everything else needs a human",
          "[boot][exit-code]") {
  const auto with = [](ErrorCategory category, SuggestedAction action) {
    broker_exec::errors::Error err = broker_exec::errors::make_error(category, "test");
    err.action = action;
    return boot::exit_class_for(err);
  };

  // Transport conditions take the restart path WHATEVER their action says — a
  // capped backoff is the right answer to a 30-second broker outage.
  CHECK(with(ErrorCategory::Network, SuggestedAction::BlockStrategy) == ExitClass::Crash);
  CHECK(with(ErrorCategory::Timeout, SuggestedAction::BlockStrategy) == ExitClass::Crash);
  CHECK(with(ErrorCategory::Transient, SuggestedAction::RaiseAlert) == ExitClass::Crash);
  CHECK(with(ErrorCategory::RateLimited, SuggestedAction::DoNotRetry) == ExitClass::Crash);

  CHECK(with(ErrorCategory::SessionExpired, SuggestedAction::ReEstablishSession) ==
        ExitClass::FailClosedNeedsHuman);
  CHECK(with(ErrorCategory::Validation, SuggestedAction::BlockStrategy) ==
        ExitClass::FailClosedNeedsHuman);
  CHECK(with(ErrorCategory::DataStale, SuggestedAction::BlockStrategy) ==
        ExitClass::FailClosedNeedsHuman);
  CHECK(with(ErrorCategory::Internal, SuggestedAction::RaiseAlert) ==
        ExitClass::FailClosedNeedsHuman);

  // Retryable actions on a non-transport category still restart.
  CHECK(with(ErrorCategory::Unknown, SuggestedAction::ReconcileFirst) == ExitClass::Crash);
  CHECK(with(ErrorCategory::Internal, SuggestedAction::RetrySafe) == ExitClass::Crash);
}

// ── The happy path ──────────────────────────────────────────────────────────

TEST_CASE("a clean boot reaches the run phase with ALL TEN checks actually invoked",
          "[boot][happy]") {
  World world;
  const BootOutcome outcome = boot::boot(world.deps());

  INFO(outcome.error.message);
  REQUIRE(outcome.ok);
  CHECK(outcome.step == BootStep::Run);
  CHECK(outcome.exit_class == ExitClass::Clean);
  CHECK(boot::exit_code_for(outcome.exit_class) == 0);

  // THE POINT OF THE STORY: every one of the ten checks delegated to a real
  // implementation. An unwired check leaves its counter at zero, so this fails
  // the moment any of them is dropped or replaced by an empty std::function.
  CHECK(outcome.audit.all_invoked());
  CHECK_FALSE(outcome.audit.first_missing().has_value());
  CHECK(outcome.audit.total() == static_cast<int>(broker_exec::boot::kSafeCheckCount));
  CHECK(outcome.audit.count(SafeCheckId::Config) == 1);
  CHECK(outcome.audit.count(SafeCheckId::StrategyNames) == 1);
  CHECK(outcome.audit.count(SafeCheckId::CryptoKeys) == 1);
  CHECK(outcome.audit.count(SafeCheckId::Clock) == 1);
  CHECK(outcome.audit.count(SafeCheckId::Session) == 1);
  CHECK(outcome.audit.count(SafeCheckId::EgressIp) == 1);
  CHECK(outcome.audit.count(SafeCheckId::InstrumentMaster) == 1);
  CHECK(outcome.audit.count(SafeCheckId::Calendar) == 1);
  CHECK(outcome.audit.count(SafeCheckId::LegacyStops) == 1);
  CHECK(outcome.audit.count(SafeCheckId::Reconciliation) == 1);

  // The whole sequence ran, in order, and produced its artifacts.
  CHECK(outcome.data_dir.has_value());
  CHECK(outcome.session_state.has_value());
  CHECK(outcome.broker.has_value());
  CHECK(outcome.engine.has_value());
  CHECK(outcome.health_published);
  CHECK(outcome.health_endpoint_started);
  CHECK(outcome.run_phase_entered);
  CHECK(world.run_calls == 1);

  // A clean boot raises NO alarm.
  CHECK_FALSE(outcome.absence_alarm_fired);
  CHECK(world.alerts.critical_count() == 0);

  // Composing a broker must not touch the transport (Story 6.3's structural
  // claim, re-asserted from the boot path).
  CHECK(world.kite_http.calls == 0);

  // The health state is genuinely readable and ready.
  const broker_exec::cli::HealthSnapshot published = world.health.latest();
  CHECK(published.session_state == broker_exec::session::SessionState::Healthy);
  CHECK(published.clock_sane);
  CHECK(published.replay_clean);
  CHECK(broker_exec::cli::is_ready(published, 15000));

  // The account directory really was created and tightened through the seam.
  CHECK(fs::exists(world.data_root.path / World::kAccount));
  CHECK(world.perms_calls >= 1);
}

TEST_CASE("the shipped run phase REFUSES rather than faking a trading loop", "[boot][run-phase]") {
  World world;
  BootDeps deps = world.deps();
  deps.run_phase = boot::unimplemented_run_phase();

  const BootOutcome outcome = boot::boot(deps);

  // Boot completed everything it owns...
  CHECK(outcome.audit.all_invoked());
  CHECK(outcome.broker.has_value());
  CHECK(outcome.engine.has_value());
  CHECK(outcome.health_published);
  CHECK(outcome.run_phase_entered);
  // ...and then said so honestly: 70, a human is required, no auto-restart.
  CHECK_FALSE(outcome.ok);
  CHECK(outcome.step == BootStep::Run);
  CHECK(outcome.exit_class == ExitClass::FailClosedNeedsHuman);
  CHECK(boot::exit_code_for(outcome.exit_class) == 70);
  CHECK(outcome.absence_alarm_fired);
}

// ── Fail-closed: each step, the right class, and NOTHING later runs ─────────

TEST_CASE("a bad configuration stops at load-config with 70 and runs nothing later",
          "[boot][fail-closed]") {
  World world;
  world.env_map.erase("BROKER_EXEC_BROKER_NAME");  // a required field

  const BootOutcome outcome = boot::boot(world.deps());

  CHECK_FALSE(outcome.ok);
  CHECK(outcome.step == BootStep::LoadConfig);
  CHECK(outcome.exit_class == ExitClass::FailClosedNeedsHuman);
  CHECK(outcome.audit.total() == 0);
  CHECK_FALSE(outcome.data_dir.has_value());
  CHECK_FALSE(outcome.broker.has_value());
  CHECK_FALSE(outcome.engine.has_value());
  CHECK_FALSE(outcome.health_published);
  CHECK_FALSE(outcome.run_phase_entered);
  CHECK(world.session_probe_calls == 0);
  CHECK(world.run_calls == 0);
  CHECK(outcome.absence_alarm_fired);
}

TEST_CASE("an invalid account id stops at resolve-data-dir with 70", "[boot][fail-closed]") {
  World world;
  BootDeps deps = world.deps();
  // Both the CLI and the configuration name the bad id, so the identity check
  // agrees and `AccountDataDir::create` is the thing that refuses it. It is never
  // sanitized into something "close enough".
  deps.args.account_id = "../escape";
  world.env_map["BROKER_EXEC_ENGINE_ACCOUNT_ID"] = "../escape";

  const BootOutcome outcome = boot::boot(deps);

  CHECK_FALSE(outcome.ok);
  CHECK(outcome.step == BootStep::ResolveDataDir);
  CHECK(outcome.exit_class == ExitClass::FailClosedNeedsHuman);
  CHECK(outcome.audit.total() == 0);
  CHECK_FALSE(outcome.broker.has_value());
  CHECK(world.run_calls == 0);
}

TEST_CASE("an account-identity mismatch is refused BEFORE any directory is created",
          "[boot][fail-closed]") {
  // The systemd `%i` / config footgun: this process would otherwise create and
  // tighten a tree for an account its configuration does not describe.
  World world;
  BootDeps deps = world.deps();
  deps.args.account_id = "acct-2";  // the config still says acct-1

  const BootOutcome outcome = boot::boot(deps);

  CHECK_FALSE(outcome.ok);
  CHECK(outcome.step == BootStep::ResolveDataDir);
  CHECK(outcome.exit_class == ExitClass::FailClosedNeedsHuman);
  CHECK_FALSE(outcome.data_dir.has_value());
  CHECK(world.perms_calls == 0);
  CHECK_FALSE(fs::exists(world.data_root.path / "acct-2"));
  CHECK(outcome.absence_alarm_fired);
}

TEST_CASE("a --data-root that disagrees with config paths.data_dir stops at resolve-data-dir",
          "[boot][fail-closed]") {
  World world;
  BootDeps deps = world.deps();
  deps.args.data_root = world.data_root.path / "somewhere-else";

  const BootOutcome outcome = boot::boot(deps);

  CHECK_FALSE(outcome.ok);
  CHECK(outcome.step == BootStep::ResolveDataDir);
  CHECK(outcome.exit_class == ExitClass::FailClosedNeedsHuman);
  CHECK(contains(outcome.error.message, "data-root"));
}

TEST_CASE("a missing pinned ledger public key stops at open-ledger with 70",
          "[boot][fail-closed][ledger]") {
  World world;
  std::error_code ec;
  fs::remove(world.data_root.path / World::kAccount / "ledger_public_key.hex", ec);

  const BootOutcome outcome = boot::boot(world.deps());

  CHECK_FALSE(outcome.ok);
  CHECK(outcome.step == BootStep::OpenLedger);
  CHECK(outcome.exit_class == ExitClass::FailClosedNeedsHuman);
  CHECK(outcome.audit.total() == 0);
  CHECK(world.session_probe_calls == 0);
  CHECK_FALSE(outcome.broker.has_value());
  CHECK(outcome.absence_alarm_fired);
}

TEST_CASE("a corrupt ledger chain stops at open-ledger and is never auto-restarted",
          "[boot][fail-closed][ledger]") {
  World world;
  // A line that is not a parseable ledger entry, followed by a second line so it
  // is not eligible for the torn-last-write leniency.
  write_text(world.data_root.path / World::kAccount / "ledger.jsonl",
             "this is not json\n{\"seq\":0}\n");

  const BootOutcome outcome = boot::boot(world.deps());

  CHECK_FALSE(outcome.ok);
  CHECK(outcome.step == BootStep::OpenLedger);
  CHECK(outcome.exit_class == ExitClass::FailClosedNeedsHuman);
  CHECK(world.session_probe_calls == 0);
}

TEST_CASE("a ledger-chain probe that CANNOT ANSWER is refused, not read as a first boot",
          "[boot][fail-closed][ledger]") {
  World world;

  // A POPULATED chain on disk. This is what makes the permissive reading
  // catastrophic rather than merely wrong: skipping load() leaves the in-memory
  // chain empty, and the first append then writes seq=0 with an empty prev_hash
  // on top of these two records — which every later boot fails verify_chain on,
  // for good.
  const fs::path chain = world.data_root.path / World::kAccount / "ledger.jsonl";
  {
    broker_exec::ledger::Ledger seed(world.test_clock, chain);
    REQUIRE(seed.append("boot-test: first entry").has_value());
    REQUIRE(seed.append("boot-test: second entry").has_value());
  }

  BootDeps deps = world.deps();
  int probe_calls = 0;
  // The injected fault stands in for the transient the real probe cannot be made
  // to produce from a test (see ChainPresenceFn): a bind/NFS mount that answers
  // EACCES/ESTALE for one syscall. `fs::exists` reports that as a plain `false`.
  deps.chain_present = [&probe_calls](const fs::path&) -> Result<bool> {
    ++probe_calls;
    return broker_exec::fail(broker_exec::errors::make_error(
        ErrorCategory::Internal, "test probe: the account tree could not be stat()ed"));
  };

  const BootOutcome outcome = boot::boot(deps);

  CHECK(probe_calls == 1);
  CHECK_FALSE(outcome.ok);
  CHECK(outcome.step == BootStep::OpenLedger);
  // A momentary mount fault restarts with backoff — it is not a 70 — but it is
  // still a refusal: the process does not trade on a ledger it never read.
  CHECK(outcome.exit_class == ExitClass::Crash);
  CHECK_FALSE(outcome.absence_alarm_fired);

  // NOTHING AFTER THE FAILING STEP RAN. The ledger stayed unloaded (size 0) and
  // boot did NOT carry that empty chain forward into the run phase, which is the
  // whole defect: the caller-owned Ledger outlives boot and would have appended
  // onto it.
  CHECK(world.ledger->size() == 0);
  CHECK(world.session_probe_calls == 0);
  CHECK(outcome.audit.total() == 0);
  CHECK_FALSE(outcome.broker.has_value());
  CHECK_FALSE(outcome.engine.has_value());
  CHECK_FALSE(outcome.health_published);
  CHECK(world.run_calls == 0);

  // And the on-disk chain is exactly as it was: two entries, still verifying.
  broker_exec::ledger::Ledger reopened(world.test_clock, chain);
  REQUIRE(reopened.load().has_value());
  CHECK(reopened.size() == 2);
  CHECK(reopened.verify_chain().has_value());
}

TEST_CASE("a TRANSPORT failure on the session probe is the CRASH class, not 70",
          "[boot][crash-class]") {
  World world;
  // This is the case that would be very expensive to get wrong: a 5xx on the
  // session probe must restart with backoff, not down the account until a human
  // notices.
  world.session_answer = broker_exec::fail(
      broker_exec::errors::make_error(ErrorCategory::Network, "probe: connection reset"));

  const BootOutcome outcome = boot::boot(world.deps());

  CHECK_FALSE(outcome.ok);
  CHECK(outcome.step == BootStep::EstablishSession);
  CHECK(outcome.exit_class == ExitClass::Crash);
  CHECK(boot::exit_code_for(outcome.exit_class) == broker_exec::boot::kExitCrash);
  CHECK(broker_exec::supervisor::exit_reason_from_code(boot::exit_code_for(outcome.exit_class)) ==
        broker_exec::supervisor::ExitReason::Crash);

  // A crash raises NO absence alarm — the supervisor restarts instead.
  CHECK_FALSE(outcome.absence_alarm_fired);
  CHECK(world.alerts.critical_count() == 0);

  // And nothing later ran.
  CHECK(outcome.audit.total() == 0);
  CHECK_FALSE(outcome.broker.has_value());
  CHECK_FALSE(outcome.engine.has_value());
  CHECK(world.run_calls == 0);
}

TEST_CASE("a DEAD daily session stops at establish-session with 70", "[boot][fail-closed]") {
  World world;
  world.session_answer = broker_exec::session::SessionState::NeedsReauth;

  const BootOutcome outcome = boot::boot(world.deps());

  CHECK_FALSE(outcome.ok);
  CHECK(outcome.step == BootStep::EstablishSession);
  CHECK(outcome.exit_class == ExitClass::FailClosedNeedsHuman);
  CHECK(outcome.error.category == ErrorCategory::SessionExpired);
  CHECK(outcome.absence_alarm_fired);
  CHECK(world.alerts.critical_count() == 1);
}

// ── The safe-start gate ─────────────────────────────────────────────────────

TEST_CASE("a safe-start refusal exits 70, fires the absence alarm, and NEVER builds the engine",
          "[boot][safe-start]") {
  World world;
  // Break the sixth check only. Everything before it must still have run.
  world.env_map.erase(std::string(boot::kEgressAllowlistEnv));

  const BootOutcome outcome = boot::boot(world.deps());

  CHECK_FALSE(outcome.ok);
  CHECK(outcome.step == BootStep::SafeStart);
  CHECK(outcome.exit_class == ExitClass::FailClosedNeedsHuman);
  CHECK(boot::exit_code_for(outcome.exit_class) == 70);
  CHECK(contains(outcome.error.message, "safe-start: egress-IP"));

  // THE SPY ASSERTION: nothing downstream of the gate was composed.
  CHECK_FALSE(outcome.broker.has_value());
  CHECK_FALSE(outcome.engine.has_value());
  CHECK_FALSE(outcome.health_published);
  CHECK_FALSE(outcome.health_endpoint_started);
  CHECK_FALSE(outcome.run_phase_entered);
  CHECK(world.endpoint_calls == 0);
  CHECK(world.run_calls == 0);
  CHECK(world.kite_http.calls == 0);

  // The gate short-circuits at the FIRST failure: checks 1-6 ran, 7-10 did not.
  CHECK(outcome.audit.count(SafeCheckId::Config) == 1);
  CHECK(outcome.audit.count(SafeCheckId::StrategyNames) == 1);
  CHECK(outcome.audit.count(SafeCheckId::CryptoKeys) == 1);
  CHECK(outcome.audit.count(SafeCheckId::Clock) == 1);
  CHECK(outcome.audit.count(SafeCheckId::Session) == 1);
  CHECK(outcome.audit.count(SafeCheckId::EgressIp) == 1);
  CHECK(outcome.audit.count(SafeCheckId::InstrumentMaster) == 0);
  CHECK(outcome.audit.count(SafeCheckId::Calendar) == 0);
  CHECK(outcome.audit.count(SafeCheckId::LegacyStops) == 0);
  CHECK(outcome.audit.count(SafeCheckId::Reconciliation) == 0);
  CHECK_FALSE(outcome.audit.all_invoked());

  // TO-5: the absence alarm fired before we exited.
  CHECK(outcome.absence_alarm_fired);
  CHECK(outcome.absence_alarm_delivered);
  REQUIRE(world.alerts.critical_count() == 1);
  CHECK(contains(world.alerts.sent.front().message, "FAILED CLOSED"));
  CHECK(contains(world.alerts.sent.front().message, "70"));
}

TEST_CASE("an undeliverable absence alarm does NOT change the exit code", "[boot][safe-start]") {
  World world;
  world.env_map.erase(std::string(boot::kEgressAllowlistEnv));
  world.alerts.deliver = false;  // every channel down

  const BootOutcome outcome = boot::boot(world.deps());

  CHECK(outcome.exit_class == ExitClass::FailClosedNeedsHuman);
  CHECK(outcome.absence_alarm_fired);
  CHECK_FALSE(outcome.absence_alarm_delivered);
  // The ABSENCE of the process is the alarm; delivery is best-effort.
  CHECK(boot::exit_code_for(outcome.exit_class) == 70);
}

TEST_CASE("a bad strategy name is caught by the real IMP-19 check, at the gate",
          "[boot][safe-start]") {
  World world;
  // "S1" is a single homogeneous run: it makes every client_ref it mints
  // unloggable. config::load itself refuses it, so the failure surfaces at the
  // gate's config check (which RE-PARSES the configuration on disk).
  world.env_map["BROKER_EXEC_STRATEGIES_NAMES"] = "S1";

  const BootOutcome outcome = boot::boot(world.deps());

  CHECK_FALSE(outcome.ok);
  // Either LoadConfig (the loader validates names) or SafeStart (the gate does);
  // both are fail-closed 70 and both name the offending rule. Pinning the CLASS
  // rather than the step keeps this honest about which layer catches it first.
  CHECK(outcome.exit_class == ExitClass::FailClosedNeedsHuman);
  CHECK_FALSE(outcome.engine.has_value());
}

TEST_CASE("an unresolved order in the projection blocks the start at the reconciliation check",
          "[boot][safe-start][reconciliation]") {
  World world;
  const auto inserted = world.store->insert_order(
      make_order(broker_exec::domain::OrderState::Unknown, broker_exec::domain::OrderType::Limit,
                 "boot-1a2b3c4d-00001111222233334444555566667777"));
  REQUIRE(inserted.has_value());

  const BootOutcome outcome = boot::boot(world.deps());

  CHECK_FALSE(outcome.ok);
  CHECK(outcome.step == BootStep::SafeStart);
  CHECK(outcome.exit_class == ExitClass::FailClosedNeedsHuman);
  CHECK(contains(outcome.error.message, "safe-start: reconciliation"));
  // Every check up to and including the last one delegated to a real component.
  CHECK(outcome.audit.all_invoked());
  CHECK_FALSE(outcome.broker.has_value());
  CHECK_FALSE(outcome.engine.has_value());
}

TEST_CASE("a pre-IMP-11 trigger-less stop blocks the start at the legacy-stop check",
          "[boot][safe-start][legacy-stops]") {
  World world;
  const auto inserted = world.store->insert_order(make_order(
      broker_exec::domain::OrderState::Acknowledged, broker_exec::domain::OrderType::StopLoss,
      "boot-2b3c4d5e-00001111222233334444555566668888"));
  REQUIRE(inserted.has_value());

  const BootOutcome outcome = boot::boot(world.deps());

  CHECK_FALSE(outcome.ok);
  CHECK(outcome.step == BootStep::SafeStart);
  CHECK(outcome.exit_class == ExitClass::FailClosedNeedsHuman);
  CHECK(contains(outcome.error.message, "safe-start: legacy-stops"));
  // Legacy-stops is check 9, so reconciliation (10) must NOT have run.
  CHECK(outcome.audit.count(SafeCheckId::LegacyStops) == 1);
  CHECK(outcome.audit.count(SafeCheckId::Reconciliation) == 0);
}

TEST_CASE("a stale instrument master that cannot refresh blocks the start",
          "[boot][safe-start][refdata]") {
  World world;
  // Replace the master with one whose download fails: require_fresh() is false and
  // the refresh cannot fix it, so the world is unverifiable.
  world.instruments = std::make_unique<broker_exec::refdata::InstrumentMaster>(
      []() -> Result<std::string> {
        return broker_exec::fail(broker_exec::errors::make_error(ErrorCategory::DataStale,
                                                                 "instruments: no dump today"));
      },
      world.test_clock, world.refdata_cache.path, "kite", "nfo");

  const BootOutcome outcome = boot::boot(world.deps());

  CHECK_FALSE(outcome.ok);
  CHECK(outcome.step == BootStep::SafeStart);
  CHECK(outcome.exit_class == ExitClass::FailClosedNeedsHuman);
  CHECK(contains(outcome.error.message, "safe-start: instrument-master"));
  CHECK(outcome.audit.count(SafeCheckId::InstrumentMaster) == 1);
  CHECK(outcome.audit.count(SafeCheckId::Calendar) == 0);
}

TEST_CASE("a clock stall detected DURING boot blocks the start", "[boot][safe-start][clock]") {
  World world;
  BootDeps deps = world.deps();
  // The gate's sample is the SECOND observation; make the interval between the
  // baseline and it exceed the stall threshold by advancing inside the probe.
  deps.session_probe = [&world]() -> Result<broker_exec::session::SessionState> {
    world.test_clock.advance_both(std::chrono::seconds(30));
    return broker_exec::session::SessionState::Healthy;
  };

  const BootOutcome outcome = boot::boot(deps);

  CHECK_FALSE(outcome.ok);
  CHECK(outcome.step == BootStep::SafeStart);
  CHECK(outcome.exit_class == ExitClass::FailClosedNeedsHuman);
  CHECK(contains(outcome.error.message, "safe-start: clock"));
  CHECK(outcome.audit.count(SafeCheckId::Clock) == 1);
  CHECK(outcome.audit.count(SafeCheckId::Session) == 0);
}

TEST_CASE("a wrong-sized token-store key blocks the start at the crypto-keys check",
          "[boot][safe-start][crypto]") {
  World world;
  world.secrets.values[boot::token_key_secret_name(World::kAccount)] = std::string(16, 'k');

  const BootOutcome outcome = boot::boot(world.deps());

  CHECK_FALSE(outcome.ok);
  CHECK(outcome.step == BootStep::SafeStart);
  CHECK(contains(outcome.error.message, "safe-start: crypto-keys"));
  CHECK(outcome.audit.count(SafeCheckId::CryptoKeys) == 1);
  CHECK(outcome.audit.count(SafeCheckId::Clock) == 0);
  // The key bytes must never appear in an operator-visible message.
  CHECK_FALSE(contains(outcome.error.message, "kkkk"));
}

// ── Composition and the later steps ─────────────────────────────────────────

TEST_CASE("an undeclared capability list refuses the broker composition at 70",
          "[boot][composition]") {
  World world;
  BootDeps deps = world.deps();
  deps.broker_options.required_capabilities.clear();  // silence is not a declaration

  const BootOutcome outcome = boot::boot(deps);

  CHECK_FALSE(outcome.ok);
  CHECK(outcome.step == BootStep::ComposeBroker);
  CHECK(outcome.exit_class == ExitClass::FailClosedNeedsHuman);
  // Safe-start ran to completion first — the refusal is the composition root's.
  CHECK(outcome.audit.all_invoked());
  CHECK_FALSE(outcome.broker.has_value());
  CHECK_FALSE(outcome.engine.has_value());
  CHECK(world.run_calls == 0);
}

TEST_CASE("a FixtureCertification refuses to start a production process", "[boot][composition]") {
  World world;
  BootDeps deps = world.deps();
  deps.broker_options.capability_override =
      comp::FixtureCertification(caps::CapabilitySet::builder()
                                     .set(caps::Capability::PlaceOrder, caps::Support::Supported)
                                     .set(caps::Capability::ModifyOrder, caps::Support::Supported)
                                     .set(caps::Capability::CancelOrder, caps::Support::Supported)
                                     .set(caps::Capability::SquareOff, caps::Support::Supported)
                                     .build());

  const BootOutcome outcome = boot::boot(deps);

  CHECK_FALSE(outcome.ok);
  CHECK(outcome.step == BootStep::ComposeBroker);
  CHECK(outcome.exit_class == ExitClass::FailClosedNeedsHuman);
  CHECK_FALSE(outcome.engine.has_value());
}

TEST_CASE("an unwired kill switch refuses the engine composition at 70", "[boot][composition]") {
  World world;
  BootDeps deps = world.deps();
  deps.engine_deps.kill_state = nullptr;

  const BootOutcome outcome = boot::boot(deps);

  CHECK_FALSE(outcome.ok);
  CHECK(outcome.step == BootStep::ComposeEngine);
  CHECK(outcome.exit_class == ExitClass::FailClosedNeedsHuman);
  // The broker WAS composed (it is the earlier step) but the engine was not, and
  // the run phase never started.
  CHECK(outcome.broker.has_value());
  CHECK_FALSE(outcome.engine.has_value());
  CHECK_FALSE(outcome.health_published);
  CHECK(world.run_calls == 0);
}

TEST_CASE("a health-endpoint bind failure is classified by the seam's own error",
          "[boot][health]") {
  World world;
  world.endpoint_answer = broker_exec::fail(
      broker_exec::errors::make_error(ErrorCategory::Network, "endpoint: address in use"));

  const BootOutcome outcome = boot::boot(world.deps());

  CHECK_FALSE(outcome.ok);
  CHECK(outcome.step == BootStep::StartHealthEndpoint);
  CHECK(outcome.exit_class == ExitClass::Crash);
  // The snapshot was published before the endpoint was asked to serve it.
  CHECK(outcome.health_published);
  CHECK_FALSE(outcome.health_endpoint_started);
  CHECK(world.run_calls == 0);
  CHECK_FALSE(outcome.absence_alarm_fired);
}

// ── The wiring itself ───────────────────────────────────────────────────────

TEST_CASE("a check with no component behind it fails closed AND is not counted as invoked",
          "[boot][wiring]") {
  // This is what makes SafeStartAudit meaningful: a missing component must not be
  // able to masquerade as a check that ran.
  broker_exec::boot::SafeStartAudit audit;
  broker_exec::boot::SafeStartWiring wiring;  // everything null / empty
  const broker_exec::session::SafeStartContext ctx =
      broker_exec::boot::make_safe_start_context(wiring, audit);

  const broker_exec::session::SafeStartGate gate{};
  const auto verdict = gate.verify(ctx);

  REQUIRE_FALSE(verdict.has_value());
  CHECK(verdict.error().action == SuggestedAction::BlockStrategy);
  CHECK(audit.total() == 0);
  CHECK_FALSE(audit.all_invoked());
  REQUIRE(audit.first_missing().has_value());
  CHECK(*audit.first_missing() == SafeCheckId::Config);

  // Every one of the ten is non-empty (so the gate never sees a bare "not
  // configured") yet every one refuses.
  CHECK(static_cast<bool>(ctx.config_check));
  CHECK(static_cast<bool>(ctx.strategy_name_check));
  CHECK(static_cast<bool>(ctx.crypto_keys_check));
  CHECK(static_cast<bool>(ctx.clock_check));
  CHECK(static_cast<bool>(ctx.session_check));
  CHECK(static_cast<bool>(ctx.egress_ip_check));
  CHECK(static_cast<bool>(ctx.instrument_master_check));
  CHECK(static_cast<bool>(ctx.calendar_check));
  CHECK(static_cast<bool>(ctx.legacy_stop_check));
  CHECK(static_cast<bool>(ctx.reconciliation_check));
}

TEST_CASE("the safe-check names match the gate's own check names", "[boot][wiring]") {
  // Renaming one would desynchronize an audit line from the
  // "safe-start: <name> check failed" message an operator greps for.
  CHECK(boot::to_string(SafeCheckId::Config) == "config");
  CHECK(boot::to_string(SafeCheckId::StrategyNames) == "strategy-names");
  CHECK(boot::to_string(SafeCheckId::CryptoKeys) == "crypto-keys");
  CHECK(boot::to_string(SafeCheckId::Clock) == "clock");
  CHECK(boot::to_string(SafeCheckId::Session) == "session");
  CHECK(boot::to_string(SafeCheckId::EgressIp) == "egress-IP");
  CHECK(boot::to_string(SafeCheckId::InstrumentMaster) == "instrument-master");
  CHECK(boot::to_string(SafeCheckId::Calendar) == "calendar");
  CHECK(boot::to_string(SafeCheckId::LegacyStops) == "legacy-stops");
  CHECK(boot::to_string(SafeCheckId::Reconciliation) == "reconciliation");
}

// ── The checks this module owns, tested directly ────────────────────────────

TEST_CASE("require_config_consistent refuses an account-identity mismatch", "[boot][checks]") {
  broker_exec::config::Config config;
  config.engine.account_id = "acct-1";
  config.broker.name = "kite";
  config.paths.data_dir = "/var/lib/broker-exec/accounts";

  broker_exec::boot::BootArgs args;
  args.account_id = "acct-1";
  CHECK(broker_exec::boot::require_config_consistent(config, args).has_value());

  // The footgun this exists for: systemd's %i naming one account while the
  // configuration describes another.
  args.account_id = "acct-2";
  const auto mismatch = broker_exec::boot::require_config_consistent(config, args);
  REQUIRE_FALSE(mismatch.has_value());
  CHECK(mismatch.error().action == SuggestedAction::BlockStrategy);

  // The broker-roster check config itself cannot make.
  args.account_id = "acct-1";
  config.broker.name = "zerodha";
  CHECK_FALSE(broker_exec::boot::require_config_consistent(config, args).has_value());

  config.broker.name = "kite";
  config.engine.account_id.clear();
  CHECK_FALSE(broker_exec::boot::require_config_consistent(config, args).has_value());
}

TEST_CASE("resolve_data_root prefers the argument and refuses a disagreement", "[boot][checks]") {
  broker_exec::config::Config config;
  broker_exec::boot::BootArgs args;

  CHECK_FALSE(broker_exec::boot::resolve_data_root(config, args).has_value());

  config.paths.data_dir = "/var/lib/broker-exec/accounts";
  auto from_config = broker_exec::boot::resolve_data_root(config, args);
  REQUIRE(from_config.has_value());
  CHECK(from_config.value() == fs::path("/var/lib/broker-exec/accounts"));

  // A trailing separator names the same directory and must not be a mismatch.
  args.data_root = "/var/lib/broker-exec/accounts/";
  CHECK(broker_exec::boot::resolve_data_root(config, args).has_value());

  args.data_root = "/srv/other";
  CHECK_FALSE(broker_exec::boot::resolve_data_root(config, args).has_value());
}

TEST_CASE("the egress-IP allow-list fails closed in every ambiguous case", "[boot][checks]") {
  std::map<std::string, std::string> vars;
  const broker_exec::config::EnvLookup env =
      [&vars](std::string_view key) -> std::optional<std::string> {
    const auto it = vars.find(std::string(key));
    if (it == vars.end()) {
      return std::nullopt;
    }
    return it->second;
  };

  // Nothing set at all.
  CHECK_FALSE(broker_exec::boot::require_egress_ip_allowed(env).has_value());

  // An observation with no allow-list is NOT "allow everything".
  vars[std::string(boot::kEgressIpEnv)] = "203.0.113.7";
  CHECK_FALSE(broker_exec::boot::require_egress_ip_allowed(env).has_value());

  // An EMPTY allow-list is an unconfigured check, not an empty rule.
  vars[std::string(boot::kEgressAllowlistEnv)] = "   ";
  CHECK_FALSE(broker_exec::boot::require_egress_ip_allowed(env).has_value());

  // Not in the list.
  vars[std::string(boot::kEgressAllowlistEnv)] = "198.51.100.4,198.51.100.5";
  CHECK_FALSE(broker_exec::boot::require_egress_ip_allowed(env).has_value());

  // In the list, with the surrounding spaces operators actually write.
  vars[std::string(boot::kEgressAllowlistEnv)] = " 198.51.100.4 , 203.0.113.7 ";
  CHECK(broker_exec::boot::require_egress_ip_allowed(env).has_value());

  // Substrings do NOT match: the comparison is whole-entry.
  vars[std::string(boot::kEgressIpEnv)] = "203.0.113.70";
  CHECK_FALSE(broker_exec::boot::require_egress_ip_allowed(env).has_value());

  // An absent seam is a wiring fault, and it also fails closed.
  const broker_exec::config::EnvLookup unwired{};
  CHECK_FALSE(broker_exec::boot::require_egress_ip_allowed(unwired).has_value());
}

TEST_CASE("is_unreconciled names exactly the states whose fate is unknown", "[boot][checks]") {
  using broker_exec::domain::OrderState;
  using broker_exec::domain::OrderType;
  const auto unresolved = [](OrderState state) {
    return broker_exec::boot::is_unreconciled(make_order(state, OrderType::Limit, "ref"));
  };

  CHECK(unresolved(OrderState::PendingSend));
  CHECK(unresolved(OrderState::Sent));
  CHECK(unresolved(OrderState::Unknown));
  CHECK(unresolved(OrderState::PartiallyPlaced));
  CHECK(unresolved(OrderState::ManualInterventionRequired));

  // A WORKING order whose state the broker has confirmed is ordinary to restart
  // into — blocking on one would make every open position a boot failure.
  CHECK_FALSE(unresolved(OrderState::Acknowledged));
  CHECK_FALSE(unresolved(OrderState::PartiallyFilled));
  CHECK_FALSE(unresolved(OrderState::Filled));
  CHECK_FALSE(unresolved(OrderState::Rejected));
  CHECK_FALSE(unresolved(OrderState::Cancelled));
  CHECK_FALSE(unresolved(OrderState::Reconciled));
}

TEST_CASE("require_no_unreconciled_orders names the count and the first client_ref",
          "[boot][checks]") {
  using broker_exec::domain::OrderState;
  using broker_exec::domain::OrderType;

  CHECK(broker_exec::boot::require_no_unreconciled_orders({}).has_value());

  const std::vector<broker_exec::domain::Order> clean{
      make_order(OrderState::Filled, OrderType::Limit, "a-ref"),
      make_order(OrderState::Acknowledged, OrderType::Limit, "b-ref")};
  CHECK(broker_exec::boot::require_no_unreconciled_orders(clean).has_value());

  const std::vector<broker_exec::domain::Order> dirty{
      make_order(OrderState::Filled, OrderType::Limit, "a-ref"),
      make_order(OrderState::Unknown, OrderType::Limit, "the-unknown-one"),
      make_order(OrderState::Sent, OrderType::Limit, "c-ref")};
  const auto refused = broker_exec::boot::require_no_unreconciled_orders(dirty);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().action == SuggestedAction::BlockStrategy);
  CHECK(contains(refused.error().message, "2 order(s)"));
  CHECK(contains(refused.error().message, "the-unknown-one"));
}

TEST_CASE("read_pinned_public_key accepts only a well-formed 32-byte hex key", "[boot][checks]") {
  TempDir dir;

  CHECK_FALSE(broker_exec::boot::read_pinned_public_key(dir.path / "absent.hex").has_value());

  const fs::path key_path = dir.path / "ledger_public_key.hex";
  write_text(key_path, pinned_key_hex() + "\n");
  const auto good = broker_exec::boot::read_pinned_public_key(key_path);
  REQUIRE(good.has_value());
  CHECK(good.value().size() == broker_exec::boot::kEd25519PublicKeyBytes);
  CHECK(good.value().front() == 0x01);
  CHECK(good.value().back() == 0x20);

  write_text(key_path, "0102");
  CHECK_FALSE(broker_exec::boot::read_pinned_public_key(key_path).has_value());

  std::string not_hex = pinned_key_hex();
  not_hex[0] = 'z';
  write_text(key_path, not_hex);
  CHECK_FALSE(broker_exec::boot::read_pinned_public_key(key_path).has_value());

  CHECK_FALSE(broker_exec::boot::read_pinned_public_key({}).has_value());
}

TEST_CASE("require_crypto_keys demands BOTH the token key and the pinned ledger key",
          "[boot][checks]") {
  TempDir dir;
  MapSecrets secrets;
  const std::string name = boot::token_key_secret_name("acct-1");
  CHECK(name == "acct-1.token_key");

  const fs::path key_path = dir.path / "ledger_public_key.hex";

  // No token key at all.
  CHECK_FALSE(broker_exec::boot::require_crypto_keys(secrets, name, key_path).has_value());

  secrets.values[name] = std::string(broker_exec::boot::kTokenKeyBytes, 'k');
  // Token key present, pinned ledger key absent -> still refused.
  CHECK_FALSE(broker_exec::boot::require_crypto_keys(secrets, name, key_path).has_value());

  write_text(key_path, pinned_key_hex());
  CHECK(broker_exec::boot::require_crypto_keys(secrets, name, key_path).has_value());

  // Wrong-sized token key -> refused, and the bytes never reach the message.
  secrets.values[name] = std::string(31, 'k');
  const auto short_key = broker_exec::boot::require_crypto_keys(secrets, name, key_path);
  REQUIRE_FALSE(short_key.has_value());
  CHECK_FALSE(contains(short_key.error().message, "kkkk"));
}

TEST_CASE("ledger_chain_present separates 'there is no chain' from 'I could not tell'",
          "[boot][checks][ledger]") {
  TempDir dir;
  const fs::path chain = dir.path / "ledger.jsonl";

  // Absent, cleanly. THIS is the only answer that may be read as a first boot.
  const auto absent = boot::ledger_chain_present(chain);
  REQUIRE(absent.has_value());
  CHECK_FALSE(absent.value());

  write_text(chain, "{}\n");
  const auto present = boot::ledger_chain_present(chain);
  REQUIRE(present.has_value());
  CHECK(present.value());

  // An unresolved path is a composition fault, never "no chain yet".
  CHECK_FALSE(boot::ledger_chain_present(fs::path{}).has_value());

  // A path the platform genuinely cannot stat must be an Error, not a `false`.
  // Only some platforms produce one here (libstdc++ reports ENAMETOOLONG for an
  // over-long component; the MSVC STL folds several such codes back into "not
  // found"), so the claim is made exactly where it is observable: whenever the
  // standard library itself sets an error_code, this function must REFUSE. That
  // conditional is also the evidence for why ChainPresenceFn exists at all.
  const fs::path unstatable = dir.path / std::string(600, 'a');
  std::error_code ec;
  static_cast<void>(fs::exists(unstatable, ec));
  if (ec) {
    const auto unanswerable = boot::ledger_chain_present(unstatable);
    REQUIRE_FALSE(unanswerable.has_value());
    CHECK(contains(unanswerable.error().message, "could not determine"));
  }
}
