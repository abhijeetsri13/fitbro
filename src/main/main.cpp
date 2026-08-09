// broker-exec — THE PROCESS ENTRYPOINT (IMP-20).
//
// This is the binary `deploy/systemd/broker-exec@.service` has always pointed
// `ExecStart` at and that this repository did not previously build. It is a THIN
// SHELL and deliberately holds no logic of its own:
//
//   argv  ->  construct the real components  ->  broker_exec::boot::boot(deps)
//         ->  map the returned ExitClass to a process exit code.
//
// Everything decidable lives in `broker_exec::boot`, which is unit-tested without
// a process. What lives HERE is only what a test cannot have: the real cpr/libcurl
// transport, the real environment, the real SQLite file, the real clock, the
// operator-visible stderr line, and the top-level catch.
//
// ── MAIN IS THE NO-THROW BOUNDARY ──────────────────────────────────────────
// The library never throws across its boundaries, but an injected seam is
// caller-supplied code and a third-party dependency (CLI11, cpr, SQLite's C++
// glue) can still raise. Letting that escape `main` is `std::terminate`, i.e. a
// signal-derived exit code the supervisor reads as a crash by accident rather
// than by decision. So main catches EVERYTHING at the very top and maps it to the
// crash class on purpose. That IS the boundary.
//
// ── THE VERBS ───────────────────────────────────────────────────────────────
//   broker-exec run        --account <id> [--config <toml>] [--data-root <dir>]
//                          [--shared-refdata <dir>] [--health-host H] [--health-port P]
//   broker-exec boot-check <the same options>
//
// `run` is what the systemd unit invokes (its ExecStart passes exactly
// `--account %i --data-root ... --shared-refdata ...`, and no `--config`, which is
// why `--config` is optional: an empty path makes `config::load` read defaults +
// environment only). `boot-check` runs the IDENTICAL cold-boot sequence and exits
// 0 when the world verified — a deployment preflight that never enters a run
// phase and never claims to trade.
//
// ── ARGUMENT HANDLING ───────────────────────────────────────────────────────
// CLI11, used the same way `src/cli/cli_app.cpp` uses it (one `CLI::App`,
// subcommands, CLI11 confined to this translation unit). There is no second
// argument parser in this repository.
//
// Cross-platform: C++20 standard library, std::filesystem, and the existing
// adapter/platform seams. No OS API and no `#ifdef` in this file.

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// CLI11 is CONFINED to this translation unit (the same posture cli_app.cpp takes).
#include <CLI/CLI.hpp>

#include "broker_exec/accounts/account_data_dir.hpp"
#include "broker_exec/accounts/shared_refdata_cache.hpp"
#include "broker_exec/adapters/kite/cpr_http_client.hpp"
#include "broker_exec/adapters/kite/kite_rest_client.hpp"
#include "broker_exec/boot/boot.hpp"
#include "broker_exec/capabilities/capabilities.hpp"
#include "broker_exec/cli/health_endpoint.hpp"
#include "broker_exec/cli/health_state.hpp"
#include "broker_exec/clock/skew_stall_detector.hpp"
#include "broker_exec/clock/system_clock.hpp"
#include "broker_exec/composition/broker_factory.hpp"
#include "broker_exec/composition/engine_assembly.hpp"
#include "broker_exec/config/config.hpp"
#include "broker_exec/domain/redaction.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ledger/ledger.hpp"
#include "broker_exec/modes/killswitch.hpp"
#include "broker_exec/modes/posture.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/refdata/instrument_master.hpp"
#include "broker_exec/refdata/trading_calendar.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/secrets/env_secret_provider.hpp"
#include "broker_exec/secrets/token_store.hpp"
#include "broker_exec/session/kite_session_establisher.hpp"
#include "broker_exec/session/session_state.hpp"
#include "broker_exec/store/store.hpp"

namespace {

namespace fs = std::filesystem;
namespace boot = broker_exec::boot;
namespace caps = broker_exec::capabilities;
namespace comp = broker_exec::composition;

using broker_exec::Result;
using broker_exec::errors::Error;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::make_error;
using broker_exec::errors::SuggestedAction;

// Kite's default REST base URL, used when `broker.base_url` is not configured.
constexpr const char* kKiteDefaultBaseUrl = "https://api.kite.trade";

// The operator-provisioned trading-calendar document.
//
// THERE IS NO CALENDAR ENDPOINT IN THIS LIBRARY. `refdata::TradingCalendar` takes
// an injected fetch seam and no broker adapter implements one, so production reads
// a JSON document the deployment provisions (config management, a cron download,
// an operator). Unset or unreadable is a typed DataStale error, which means the
// calendar safe-start check FAILS CLOSED — never a silently empty calendar, which
// would read as "every weekday is a trading day, all day".
constexpr const char* kCalendarFileEnv = "BROKER_EXEC_CALENDAR_FILE";

[[nodiscard]] Error blocking(ErrorCategory category, std::string message) {
  Error err = make_error(category, std::move(message));
  err.action = SuggestedAction::BlockStrategy;
  return err;
}

// The LOCAL leg of the operator alarm: one scrubbed line per alert on stderr,
// which under systemd is journald.
//
// WHY NOT `alerting::MultiChannelAlertSink`. That sink needs a channel list and a
// POST seam, and `config::Config` carries neither yet — wiring it with an empty
// channel list would produce a sink that fails every send, which is strictly worse
// than a line an operator can actually read. The OUT-OF-BAND leg of the
// dead-man's switch is already deployed: the committed unit's
// `OnFailure=broker-exec-alarm@%i.service` fires on exactly the two cases that
// matter (a fail-closed exit and a tripped crash-loop breaker), and it does not
// depend on this process being able to talk to anything.
class JournalAlertSink final : public broker_exec::ports::AlertSink {
 public:
  [[nodiscard]] Result<broker_exec::ports::Ok> send(broker_exec::ports::AlertLevel level,
                                                    const std::string& message) override {
    // Outbound payload: scrub before it leaves the process (SEC-3).
    std::fprintf(stderr, "[%s] %s\n", level_name(level),
                 broker_exec::domain::scrub(message).c_str());
    return broker_exec::ports::ok();
  }

  [[nodiscard]] Result<broker_exec::ports::Ok> send_test_alert() override {
    return send(broker_exec::ports::AlertLevel::Info, "broker-exec: test alert");
  }

 private:
  [[nodiscard]] static const char* level_name(broker_exec::ports::AlertLevel level) noexcept {
    switch (level) {
      case broker_exec::ports::AlertLevel::Info:
        return "INFO";
      case broker_exec::ports::AlertLevel::Warning:
        return "WARN";
      case broker_exec::ports::AlertLevel::Error:
        return "ERROR";
      case broker_exec::ports::AlertLevel::Critical:
        return "CRITICAL";
    }
    return "ALERT";
  }
};

// Today's UTC date as ISO YYYY-MM-DD, derived from the INJECTED clock via
// std::chrono's calendar (no localtime/strftime, no `#ifdef`). It keys the shared
// reference-data cache, which must agree with refdata's own per-day stamp.
[[nodiscard]] std::string iso_date_utc(const broker_exec::ports::ClockPort& clock_port) {
  const auto days = std::chrono::floor<std::chrono::days>(clock_port.now_wall());
  const std::chrono::year_month_day ymd{days};
  const int year = static_cast<int>(ymd.year());
  const unsigned month = static_cast<unsigned>(ymd.month());
  const unsigned day = static_cast<unsigned>(ymd.day());

  std::string text;
  text.reserve(10);
  text += std::to_string(year);
  text += '-';
  if (month < 10) {
    text += '0';
  }
  text += std::to_string(month);
  text += '-';
  if (day < 10) {
    text += '0';
  }
  text += std::to_string(day);
  return text;
}

// Read an operator-provisioned text document (the trading calendar). Never
// throws; a missing/unreadable file is a typed DataStale error so the calendar
// freshness gate blocks.
[[nodiscard]] Result<std::string> read_text_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return broker_exec::fail(blocking(
        ErrorCategory::DataStale,
        std::string("calendar: the document named by ") + kCalendarFileEnv + " could not be read"));
  }
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) {
    return broker_exec::fail(
        blocking(ErrorCategory::DataStale, "calendar: failed to read the calendar document"));
  }
  return text;
}

// ── Parsed command line ─────────────────────────────────────────────────────
struct Options {
  std::string account;
  std::string config_path;
  std::string data_root;
  std::string shared_refdata;
  std::string health_host = "127.0.0.1";
  int health_port = 9110;
  std::int64_t health_live_budget_ms = 15000;
  bool boot_check = false;
};

// Print one redaction-safe line and return the code. Every operator-visible line
// scrubs itself, exactly as `cli::run_cli` does.
[[nodiscard]] int report(const boot::BootOutcome& outcome, const Options& options) {
  const int code = boot::exit_code_for(outcome.exit_class);
  if (outcome.ok) {
    std::fprintf(stdout, "broker-exec: %s completed for account %s\n",
                 options.boot_check ? "boot-check" : "run",
                 broker_exec::domain::scrub(options.account).c_str());
    return code;
  }
  std::fprintf(stderr, "broker-exec: FAILED at step '%s' (%s, exit %d): %s\n",
               std::string(boot::to_string(outcome.step)).c_str(),
               std::string(boot::to_string(outcome.exit_class)).c_str(), code,
               broker_exec::domain::scrub(outcome.error.message).c_str());
  return code;
}

[[nodiscard]] int report_error(const Error& error, const char* where) {
  const boot::ExitClass exit_class = boot::exit_class_for(error);
  const int code = boot::exit_code_for(exit_class);
  std::fprintf(stderr, "broker-exec: FAILED during %s (%s, exit %d): %s\n", where,
               std::string(boot::to_string(exit_class)).c_str(), code,
               broker_exec::domain::scrub(error.message).c_str());
  return code;
}

// ── The wiring ──────────────────────────────────────────────────────────────
//
// Everything below builds the REAL components `boot()` borrows. It is deliberately
// dumb: no decision here is load-bearing, because every step it performs is
// repeated fail-closed inside `boot()`.
//
// THE CONFIGURATION IS LOADED TWICE, ON PURPOSE. main needs the resolved paths
// (data root, broker name, timeouts) to CONSTRUCT the store, the ledger and the
// reference data that boot borrows; boot then loads and validates it again as its
// authoritative step 1, and the safe-start config check loads it a third time to
// prove the file on disk still parses at gate time. `config::load` is pure and
// side-effect-free, so the only cost is a re-parse — and the alternative (main
// handing boot a config it already trusted) would make step 1 a formality.
[[nodiscard]] int run_boot(const Options& options) {
  broker_exec::clock::SystemClock system_clock;
  broker_exec::clock::SkewStallDetector clock_detector;
  broker_exec::secrets::EnvSecretProvider secret_provider{
      broker_exec::secrets::default_env_lookup()};
  JournalAlertSink alerts;
  broker_exec::cli::HealthState health;

  const broker_exec::config::EnvLookup env = broker_exec::config::default_env_lookup();

  // ── Preflight: resolve the layout so the components can be built ──────────
  boot::BootArgs args;
  args.account_id = options.account;
  args.config_path = options.config_path;
  args.data_root = options.data_root;
  args.shared_refdata_root = options.shared_refdata;
  args.health_host = options.health_host;
  args.health_port = options.health_port;
  args.health_live_budget_ms = options.health_live_budget_ms;

  auto loaded = broker_exec::config::load(args.config_path, env);
  if (!loaded) {
    return report_error(loaded.error(), "configuration load");
  }
  const broker_exec::config::Config config = std::move(loaded).value();

  // THE IDENTITY CHECK RUNS HERE TOO, and it must. boot() refuses a `%i` / config
  // account mismatch before IT creates anything, but this preflight creates the
  // account tree first (the store and the ledger cannot be opened in a directory
  // that does not exist), so without this a mismatched invocation would leave an
  // empty tree for an account it was never meant to touch before boot said no.
  // It is the same function the safe-start config check calls; there is one rule.
  if (auto consistent = boot::require_config_consistent(config, args); !consistent) {
    return report_error(consistent.error(), "configuration consistency");
  }

  auto data_root = boot::resolve_data_root(config, args);
  if (!data_root) {
    return report_error(data_root.error(), "data-root resolution");
  }

  const std::string account_id =
      args.account_id.empty() ? config.engine.account_id : args.account_id;
  auto account_dir = broker_exec::accounts::AccountDataDir::create(std::move(data_root).value(),
                                                                   account_id);
  if (!account_dir) {
    return report_error(account_dir.error(), "account layout");
  }
  // `broker_exec::expected` exposes value()/operator* but no operator->, so the
  // layout is bound once and used by value from here on.
  const broker_exec::accounts::AccountDataDir dir = std::move(account_dir).value();

  // Idempotent, and repeated authoritatively inside boot(). It runs here only
  // because the store/ledger files below cannot be opened in a directory that
  // does not exist yet.
  if (auto ensured = dir.ensure(); !ensured) {
    return report_error(ensured.error(), "account directory creation");
  }

  // ── The SQLite projection ────────────────────────────────────────────────
  auto opened = broker_exec::store::Store::open_or_rebuild(dir.store_db());
  if (!opened) {
    return report_error(opened.error(), "store open");
  }
  broker_exec::store::Store::OpenOutcome store_outcome = std::move(opened).value();

  // ── The tamper-evident ledger ────────────────────────────────────────────
  broker_exec::ledger::Ledger ledger(system_clock, dir.ledger());

  // ── Transport + the daily Kite session ───────────────────────────────────
  const std::string base_url =
      config.broker.base_url.empty() ? kKiteDefaultBaseUrl : config.broker.base_url;
  const broker_exec::adapters::kite::CprHttpClient kite_http(base_url, config.broker.timeout_ms);

  // TokenStore's layout is `<data_dir>/<account>/<name>.enc`, so handing it the
  // DATA ROOT and naming the blob "token_store" puts it at exactly
  // AccountDataDir::token_store(). The two layouts agree by construction rather
  // than by coincidence.
  broker_exec::secrets::TokenStore token_store(secret_provider, dir.data_root());
  broker_exec::session::KiteSessionEstablisher establisher(kite_http, secret_provider, token_store,
                                                           dir.account_id(),
                                                           "kite.api_key", "kite.api_secret",
                                                           "token_store");
  broker_exec::adapters::kite::KiteRestClient kite_rest(kite_http, secret_provider, "kite.api_key",
                                                        "kite.access_token");

  // The broker-neutral session probe. Kite's daily token has NO headless refresh,
  // so `validate()` is the honest question ("is the stored token still alive?")
  // and its transport errors are preserved rather than collapsed.
  //
  // KOTAK NEO IS NOT WIRED HERE. Its session establishment is a different flow
  // (`adapters::kotak::KotakSession`) and this story does not compose it; a kotak
  // deployment therefore fails CLOSED at the session step with the message below
  // rather than silently probing the wrong broker. (`make_broker` would refuse a
  // kotak assembly anyway — every Kotak capability is still `Unknown` pending the
  // operator-run live smoke.)
  const bool is_kite = config.broker.name == "kite";
  boot::SessionProbeFn session_probe;
  if (is_kite) {
    session_probe = [&establisher]() -> Result<broker_exec::session::SessionState> {
      return establisher.validate();
    };
  } else {
    session_probe = []() -> Result<broker_exec::session::SessionState> {
      return broker_exec::fail(blocking(
          ErrorCategory::NotSupported,
          "boot: no session establishment is wired for the configured broker; only 'kite' "
          "is composed by this entrypoint today"));
    };
  }

  // ── Reference data over the SHARED cross-process cache ───────────────────
  //
  // One sibling downloads; the rest read what it atomically published. The cache
  // root MUST live outside every per-account tree, and passing `account_data_root`
  // turns that from a comment into a checked invariant on every fetch.
  broker_exec::accounts::SharedRefdataCacheConfig cache_config;
  // The default mirrors the deployed layout in deploy/systemd/broker-exec@.service
  // (`/var/lib/broker-exec/accounts` alongside `/var/lib/broker-exec/shared/refdata`).
  // It MUST be a sibling of the account data root, not a child: the cache is shared
  // by every account and `require_outside_account_tree` refuses the nesting.
  cache_config.shared_root = options.shared_refdata.empty()
                                 ? dir.data_root().parent_path() / "shared" / "refdata"
                                 : fs::path(options.shared_refdata);
  cache_config.account_data_root = dir.data_root();
  const auto shared_cache =
      std::make_shared<const broker_exec::accounts::SharedRefdataCache>(cache_config);

  const auto trading_date = [&system_clock]() { return iso_date_utc(system_clock); };

  broker_exec::accounts::RefdataFetchFn instruments_upstream =
      [&kite_rest, is_kite]() -> Result<std::string> {
    if (!is_kite) {
      return broker_exec::fail(blocking(ErrorCategory::NotSupported,
                                        "instruments: no instrument-master download is wired for "
                                        "the configured broker"));
    }
    return kite_rest.instruments();
  };
  broker_exec::accounts::RefdataFetchFn calendar_upstream = [&env]() -> Result<std::string> {
    const std::optional<std::string> path = env(kCalendarFileEnv);
    if (!path.has_value() || path->empty()) {
      return broker_exec::fail(blocking(
          ErrorCategory::DataStale,
          std::string("calendar: ") + kCalendarFileEnv +
              " is not set. There is no broker calendar endpoint in this library, so the "
              "trading calendar must be provisioned as a JSON document; without it the "
              "calendar freshness gate blocks the start"));
    }
    return read_text_file(fs::path(*path));
  };

  // NOTE the two cache directories are DIFFERENT on purpose: refdata keeps its own
  // per-day copy inside the ACCOUNT tree, while the shared cache publishes the
  // cross-process artifact under the shared root. Pointing both at one directory
  // would have them write the same `<broker>_<segment>_<date>` filename — one
  // atomically, one not.
  broker_exec::refdata::InstrumentMaster instruments(
      broker_exec::accounts::shared_instrument_csv_fetcher(shared_cache, config.broker.name, "nfo",
                                                           trading_date,
                                                           std::move(instruments_upstream)),
      system_clock, dir.root(), config.broker.name, "nfo");
  broker_exec::refdata::TradingCalendar calendar(
      broker_exec::accounts::shared_calendar_json_fetcher(shared_cache, config.broker.name,
                                                          trading_date,
                                                          std::move(calendar_upstream)),
      system_clock, dir.root(), config.broker.name);

  // ── The health surface ───────────────────────────────────────────────────
  //
  // The server is CONSTRUCTED over the shared state here; SERVING is
  // `HealthHttpServer::listen()`, which BLOCKS and therefore belongs to whatever
  // owns the process's main thread — i.e. the trading run phase, which IMP-20 does
  // not build. This binary consequently spawns NO thread at all. Said plainly so
  // nobody reads "the endpoint started" as "the endpoint is answering": it is
  // wired to the real HealthState and ready to serve the instant a run phase calls
  // listen() on it.
  broker_exec::cli::HealthHttpServer health_server(health, options.health_live_budget_ms);
  boot::StartHealthEndpointFn start_health = [&health_server]() -> Result<broker_exec::ports::Ok> {
    static_cast<void>(health_server);
    return broker_exec::ports::ok();
  };

  // ── Kill switch + posture (required by the engine assembly in EVERY mode) ─
  //
  // KNOWN GAP, recorded rather than hidden: persisted kills are NOT replayed here.
  // `modes::KillController::replay()` needs a loader over the kill journal, and no
  // module in this repository writes that journal's format yet. Until it does, a
  // kill engaged before a restart is NOT restored — which is why the engine below
  // is composed ExitOnly and why the run phase refuses.
  broker_exec::modes::KillState kill_state;
  // Braces, not a bare declaration: PostureCoordinator has no user-provided
  // default constructor, so a const object of it must be VALUE-initialized.
  const broker_exec::modes::PostureCoordinator posture{};

  // ── Assemble the BootDeps ────────────────────────────────────────────────
  boot::BootDeps deps;
  deps.args = args;
  deps.env = env;
  deps.clock = &system_clock;
  deps.secrets = &secret_provider;
  deps.alerts = &alerts;
  deps.clock_detector = &clock_detector;
  deps.store = &store_outcome.store;
  deps.ledger = &ledger;
  deps.instruments = &instruments;
  deps.calendar = &calendar;
  deps.health = &health;
  deps.replay_clean = !store_outcome.needs_rebuild;
  deps.session_probe = std::move(session_probe);
  deps.start_health_endpoint = std::move(start_health);

  deps.broker_deps.kite_http = &kite_http;
  deps.broker_deps.kite_secrets = &secret_provider;
  // What this process needs to be able to do at all. Declared as DATA and checked
  // by make_broker BEFORE anything that can reach a broker exists (CAP-2).
  deps.broker_options.required_capabilities = {
      caps::Capability::PlaceOrder, caps::Capability::ModifyOrder, caps::Capability::CancelOrder,
      caps::Capability::SquareOff};

  deps.engine_deps.kill_state = &kill_state;
  deps.engine_deps.posture = &posture;
  // The detector fan-in (stale data, mute feed, mismatch, clock stall, ...) is
  // produced by the reconcile/market-data loop, which is out of scope. An empty
  // signal list is therefore the TRUTH for this process — no detector is running —
  // and it is safe only because the assembly below is ExitOnly and nothing calls
  // its pre-flight chains.
  deps.engine_deps.detector_signals = []() {
    return std::vector<broker_exec::modes::DetectorSignal>{};
  };
  deps.engine_deps.session = []() {
    comp::SessionSnapshot snapshot;
    // Safe-start proved the session Healthy immediately before this assembly was
    // built; the LIVE mid-session state is the run phase's to track.
    snapshot.state = broker_exec::session::SessionState::Healthy;
    return snapshot;
  };
  // EXIT-ONLY, DELIBERATELY (and it is EngineOptions' safe default). An
  // EntryCapable assembly requires a funds view, a risk engine, an UNKNOWN-pause
  // flag, an idempotency duplicate probe and a live margin quote — every one of
  // which is produced by the trading loop this story does not build. Composing
  // EntryCapable here would mean inventing those five sources, which is exactly
  // the "a wired guard is not an armed one" hole IMP-12 closed.
  deps.engine_options.mode = comp::EngineMode::ExitOnly;
  deps.engine_options.declares_no_hedge_check = true;

  // The run phase. `boot-check` is a preflight that never claims to trade, so its
  // hand-off is a documented no-op; `run` gets the refusing stub.
  if (options.boot_check) {
    deps.run_phase = []() -> Result<broker_exec::ports::Ok> {
      // boot-check verifies and exits. There is nothing to run, and saying so with
      // ok() is truthful for THIS verb only.
      return broker_exec::ports::ok();
    };
  } else {
    deps.run_phase = boot::unimplemented_run_phase();
  }

  const boot::BootOutcome outcome = boot::boot(deps);
  return report(outcome, options);
}

}  // namespace

int main(int argc, char** argv) {
  // THE NO-THROW BOUNDARY. See the file banner: an exception that escapes main is
  // std::terminate, and the supervisor would read the resulting signal exit as a
  // crash by accident. Map it to the crash class on purpose instead.
  try {
    CLI::App app{"broker-exec — broker-neutral execution engine"};
    app.require_subcommand(1);

    Options options;
    const auto add_common = [&options](CLI::App* sub) {
      sub->add_option("--account", options.account,
                      "Account id (the systemd instance name, %i)")
          ->required();
      sub->add_option("--config", options.config_path,
                      "TOML configuration path (optional; empty = defaults + environment)");
      sub->add_option("--data-root", options.data_root,
                      "Parent directory holding every per-account tree");
      sub->add_option("--shared-refdata", options.shared_refdata,
                      "Shared reference-data cache root (MUST be outside the data root)");
      sub->add_option("--health-host", options.health_host, "Health endpoint bind host (loopback)");
      sub->add_option("--health-port", options.health_port, "Health endpoint port");
    };

    CLI::App* run = app.add_subcommand("run", "Boot the engine and hand off to the run phase");
    add_common(run);
    run->callback([&options]() { options.boot_check = false; });

    CLI::App* check =
        app.add_subcommand("boot-check", "Run the cold-boot sequence and exit (deployment preflight)");
    add_common(check);
    check->callback([&options]() { options.boot_check = true; });

    try {
      app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
      // `app.exit` prints help/errors and yields CLI11's code; 0 means --help, which
      // is a clean stop. Anything else is a malformed invocation — a human fix, so
      // the fail-closed code, never a restart loop against a bad unit file.
      const int cli_code = app.exit(e);
      return cli_code == 0 ? boot::exit_code_for(boot::ExitClass::Clean)
                           : boot::exit_code_for(boot::ExitClass::FailClosedNeedsHuman);
    }

    return run_boot(options);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "broker-exec: unhandled exception: %s\n",
                 broker_exec::domain::scrub(std::string(e.what())).c_str());
    return boot::exit_code_for(boot::ExitClass::Crash);
  } catch (...) {
    std::fprintf(stderr, "%s\n", "broker-exec: unhandled unknown exception");
    return boot::exit_code_for(boot::ExitClass::Crash);
  }
}
