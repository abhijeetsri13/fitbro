// The cold-boot sequence and the exit-code contract (IMP-20).
//
// `boot()` is the whole sequence as ONE function so it is testable without a
// process: `src/main/main.cpp` is a thin shell that constructs the components,
// calls this, and maps the returned class to an exit code.
//
// FAIL-CLOSED AT EVERY STEP, AND NOTHING AFTER A FAILING STEP RUNS. The evidence
// flags on `BootOutcome` (`health_published`, `run_phase_entered`, the two
// assembly optionals) exist so a test can assert that structurally rather than by
// reading the code.
//
// No-throw, no float, no OS API, no `#ifdef`. C++20 standard library only.

#include "broker_exec/boot/boot.hpp"

#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

#include "broker_exec/cli/health_snapshot.hpp"
#include "broker_exec/domain/enums.hpp"

namespace broker_exec::boot {

namespace fs = std::filesystem;

namespace {

using errors::Error;
using errors::ErrorCategory;
using errors::make_error;
using errors::SuggestedAction;

// The filename `Ledger::write_public_key` produces. The pinned key is read from
// exactly that name under the account tree, so provisioning and verification
// cannot drift apart.
constexpr const char* kLedgerPublicKeyFile = "ledger_public_key.hex";

// No tick has been observed at boot — the market-data ingest belongs to the
// trading loop, which is out of scope. `is_ready`/`is_live` do not consult the
// tick age, so publishing the honest "infinitely old" value costs nothing and
// beats publishing a fabricated zero.
constexpr std::int64_t kNoTickYet = std::numeric_limits<std::int64_t>::max();

// A wiring fault: a component `BootDeps` promised was not supplied. Internal
// (it is a composition bug, not a data condition) but BlockStrategy, because the
// process must HALT — Internal's default action is RaiseAlert.
[[nodiscard]] Error not_wired(std::string_view what) {
  Error err = make_error(ErrorCategory::Internal,
                         std::string("boot: ") + std::string(what) + " was not wired");
  err.action = SuggestedAction::BlockStrategy;
  return err;
}

[[nodiscard]] Error blocking(ErrorCategory category, std::string message) {
  Error err = make_error(category, std::move(message));
  err.action = SuggestedAction::BlockStrategy;
  return err;
}

// Fire the dead-man's-switch absence alarm (architecture TO-5).
//
// A FAILING DELIVERY DOES NOT CHANGE THE EXIT CODE. That is the whole posture of
// the dead-man's switch: the alerter cannot be trusted to be up, so the ABSENCE
// of the process is itself the alarm, and the systemd unit's
// `OnFailure=broker-exec-alarm@%i.service` is the OS half that fires regardless.
void fire_absence_alarm(BootOutcome& outcome, const BootDeps& deps) {
  outcome.absence_alarm_fired = true;
  if (deps.alerts == nullptr) {
    return;
  }
  // Redaction-safe by construction: a step name, an account id (a `[a-z0-9_-]`
  // NAME, never a credential), an integer exit code, and an Error message that is
  // redaction-safe by the Error contract. The sink scrubs the body again on the
  // way out, which is where outbound redaction belongs.
  const std::string message =
      std::string("broker-exec FAILED CLOSED during boot at step '") +
      std::string(to_string(outcome.step)) + "' for account '" + deps.args.account_id + "'. Exit " +
      std::to_string(exit_code_for(outcome.exit_class)) +
      ": there will be NO automatic restart and this account is now ABSENT until a "
      "human intervenes. Reason: " +
      outcome.error.message;
  const Result<ports::Ok> sent = deps.alerts->send(ports::AlertLevel::Critical, message);
  outcome.absence_alarm_delivered = sent.has_value();
}

// Record a failed step on the outcome and, for the fail-closed class, raise the
// absence alarm. Every failure path in `boot()` goes through here, so no path can
// forget the alarm.
void fail_step(BootOutcome& outcome, BootStep step, ExitClass exit_class, Error error,
               const BootDeps& deps) {
  outcome.ok = false;
  outcome.step = step;
  outcome.exit_class = exit_class;
  outcome.error = std::move(error);
  if (exit_class == ExitClass::FailClosedNeedsHuman) {
    fire_absence_alarm(outcome, deps);
  }
}

// An order the broker has not finished with. Used only for the health snapshot's
// in-flight count (an observability figure), NOT as a gate.
[[nodiscard]] bool is_in_flight(const domain::Order& order) noexcept {
  switch (order.state) {
    case domain::OrderState::Filled:
    case domain::OrderState::Rejected:
    case domain::OrderState::Cancelled:
      return false;
    case domain::OrderState::Created:
    case domain::OrderState::Validated:
    case domain::OrderState::PendingSend:
    case domain::OrderState::Sent:
    case domain::OrderState::Acknowledged:
    case domain::OrderState::PartiallyFilled:
    case domain::OrderState::Unknown:
    case domain::OrderState::Reconciled:
    case domain::OrderState::PartiallyPlaced:
    case domain::OrderState::ManualInterventionRequired:
      return true;
  }
  return true;
}

}  // namespace

// ── Names and codes ─────────────────────────────────────────────────────────

std::string_view to_string(ExitClass exit_class) noexcept {
  switch (exit_class) {
    case ExitClass::Clean:
      return "clean";
    case ExitClass::FailClosedNeedsHuman:
      return "fail-closed-needs-human";
    case ExitClass::Crash:
      return "crash";
  }
  return "unknown";
}

int exit_code_for(ExitClass exit_class) noexcept {
  switch (exit_class) {
    case ExitClass::Clean:
      return supervisor::kExitClean;
    case ExitClass::FailClosedNeedsHuman:
      return supervisor::kExitFailClosedNeedsHuman;
    case ExitClass::Crash:
      return kExitCrash;
  }
  // Fail-safe, mirroring supervisor::exit_reason_from_code: an unrecognized class
  // is a crash (restart-with-backoff, bounded by the breaker), never a clean exit.
  return kExitCrash;
}

std::string_view to_string(BootStep step) noexcept {
  switch (step) {
    case BootStep::LoadConfig:
      return "load-config";
    case BootStep::ResolveDataDir:
      return "resolve-data-dir";
    case BootStep::OpenLedger:
      return "open-ledger";
    case BootStep::EstablishSession:
      return "establish-session";
    case BootStep::SafeStart:
      return "safe-start";
    case BootStep::ComposeBroker:
      return "compose-broker";
    case BootStep::ComposeEngine:
      return "compose-engine";
    case BootStep::PublishHealth:
      return "publish-health";
    case BootStep::StartHealthEndpoint:
      return "start-health-endpoint";
    case BootStep::Run:
      return "run";
  }
  return "unknown";
}

ExitClass exit_class_for(const errors::Error& error) noexcept {
  // TRANSPORT CONDITIONS ARE A CRASH whatever their action says: a capped
  // exponential backoff is exactly the right response to a broker that is
  // momentarily unreachable, and burning the account's restart budget is better
  // than paging a human for a 30-second outage.
  switch (error.category) {
    case ErrorCategory::Transient:
    case ErrorCategory::RateLimited:
    case ErrorCategory::Network:
    case ErrorCategory::Timeout:
      return ExitClass::Crash;
    case ErrorCategory::Auth:
    case ErrorCategory::SessionExpired:
    case ErrorCategory::Validation:
    case ErrorCategory::InsufficientFunds:
    case ErrorCategory::RiskRejected:
    case ErrorCategory::NotSupported:
    case ErrorCategory::DuplicateOrder:
    case ErrorCategory::OrderNotFound:
    case ErrorCategory::BrokerRejected:
    case ErrorCategory::MarketClosed:
    case ErrorCategory::DataStale:
    case ErrorCategory::Internal:
    case ErrorCategory::Unknown:
      break;
  }

  // Everything else is decided by the NORMALIZED action — the vocabulary the
  // runtime already switches on, so the exit contract cannot disagree with the
  // in-process decision the same Error would have produced.
  switch (error.action) {
    case SuggestedAction::RetrySafe:
    case SuggestedAction::ReconcileFirst:
      return ExitClass::Crash;
    case SuggestedAction::DoNotRetry:
    case SuggestedAction::BlockStrategy:
    case SuggestedAction::ReEstablishSession:
    case SuggestedAction::Cancel:
    case SuggestedAction::SquareOff:
    case SuggestedAction::RaiseAlert:
      return ExitClass::FailClosedNeedsHuman;
  }
  // Fail closed on an action nobody classified: a stop we cannot explain must not
  // be auto-restarted into.
  return ExitClass::FailClosedNeedsHuman;
}

RunPhaseFn unimplemented_run_phase() {
  return []() -> Result<ports::Ok> {
    // See boot.hpp: this REFUSES rather than pretending to trade. A loop that
    // spun doing nothing would report a healthy trading engine that places no
    // orders — a far worse lie to hand an operator than an honest 70.
    return fail(
        blocking(ErrorCategory::Internal,
                 "boot: the trading run phase is not implemented. IMP-20 builds the process "
                 "entrypoint and the cold-boot sequence ONLY; the synchronous main loop "
                 "(dispatcher pumping, reconcile scheduling, market-data ingest) is a separate "
                 "story and is deliberately NOT stubbed into a fake loop. The world was verified "
                 "safe and the health surface is up; there is nothing yet to run"));
  };
}

// ── The sequence ────────────────────────────────────────────────────────────

BootOutcome boot(const BootDeps& deps) {
  BootOutcome outcome;

  // The clock detector's BASELINE sample, taken before any work. The detector's
  // first sample is always Healthy by construction (it only establishes a
  // baseline), so taking it here is what makes the gate's later sample a genuine
  // second observation spanning the whole boot — able to see an NTP step or a
  // stall that happened while we were loading config and talking to the broker.
  if (deps.clock != nullptr && deps.clock_detector != nullptr) {
    static_cast<void>(deps.clock_detector->sample(*deps.clock));
  }

  // ── 1. Load + validate configuration ─────────────────────────────────────
  if (!deps.env) {
    fail_step(outcome, BootStep::LoadConfig, ExitClass::FailClosedNeedsHuman,
              not_wired("the environment seam"), deps);
    return outcome;
  }
  {
    auto loaded = config::load(deps.args.config_path, deps.env);
    if (!loaded) {
      // A bad/missing configuration is a human fix. Restarting re-reads the same
      // file and re-hits the same wall.
      fail_step(outcome, BootStep::LoadConfig, ExitClass::FailClosedNeedsHuman,
                std::move(loaded).error(), deps);
      return outcome;
    }
    outcome.config = std::move(loaded).value();
  }

  // ── 2. Resolve the per-account data directory (0700, idempotent) ──────────
  {
    // BEFORE ANY DIRECTORY IS CREATED. The safe-start config check runs this same
    // function again at step 5 (that is the gate's job and its counter proves it
    // ran), but waiting for the gate would mean creating — and tightening — a tree
    // for an account this process was never meant to open. An identity mismatch
    // must be refused before it touches the filesystem, not after.
    if (auto consistent = require_config_consistent(outcome.config, deps.args); !consistent) {
      fail_step(outcome, BootStep::ResolveDataDir, ExitClass::FailClosedNeedsHuman,
                std::move(consistent).error(), deps);
      return outcome;
    }

    auto root = resolve_data_root(outcome.config, deps.args);
    if (!root) {
      fail_step(outcome, BootStep::ResolveDataDir, ExitClass::FailClosedNeedsHuman,
                std::move(root).error(), deps);
      return outcome;
    }
    // The id this PROCESS was started for wins (systemd's `%i`); the config's own
    // id is cross-checked by the safe-start config check, which refuses a
    // mismatch rather than silently opening another account's tree.
    const std::string account_id =
        deps.args.account_id.empty() ? outcome.config.engine.account_id : deps.args.account_id;

    auto dir =
        accounts::AccountDataDir::create(std::move(root).value(), account_id, deps.dir_permissions);
    if (!dir) {
      // An invalid account id is never sanitized into something "close enough".
      fail_step(outcome, BootStep::ResolveDataDir, ExitClass::FailClosedNeedsHuman,
                std::move(dir).error(), deps);
      return outcome;
    }
    outcome.data_dir = std::move(dir).value();

    if (auto ensured = outcome.data_dir->ensure(); !ensured) {
      // CRASH, NOT FAIL-CLOSED, and deliberately so: creating a directory and
      // tightening it to 0700 fails for genuinely transient reasons (a
      // StateDirectory not yet mounted, a full disk, a racing sibling), and a
      // capped backoff is the right response. The crash-loop breaker — systemd's
      // StartLimitBurst and SupervisorPolicy's max_consecutive_restarts — bounds
      // it, so a permanent permissions fault still ends up escalated rather than
      // flapping forever.
      fail_step(outcome, BootStep::ResolveDataDir, ExitClass::Crash, std::move(ensured).error(),
                deps);
      return outcome;
    }
  }

  // ── 3. Open + verify the ledger chain ────────────────────────────────────
  if (deps.ledger == nullptr) {
    fail_step(outcome, BootStep::OpenLedger, ExitClass::FailClosedNeedsHuman,
              not_wired("the ledger"), deps);
    return outcome;
  }
  {
    // The pinned key is read from the account tree, where it must be provisioned
    // out-of-band (see ledger.hpp's trust model: the checkpoint's own embedded key
    // is attacker-writable and cannot anchor trust).
    auto pinned = read_pinned_public_key(outcome.data_dir->root() / kLedgerPublicKeyFile);
    if (!pinned) {
      fail_step(outcome, BootStep::OpenLedger, ExitClass::FailClosedNeedsHuman,
                std::move(pinned).error(), deps);
      return outcome;
    }

    // A FIRST boot has no chain file yet, and that is not corruption: there is
    // simply nothing to load or to verify the internal consistency of. The
    // checkpoint guard below still runs — a retained checkpoint with NO ledger
    // beneath it is a truncation to zero, which is exactly what it detects.
    std::error_code ec;
    const bool chain_exists = fs::exists(outcome.data_dir->ledger(), ec);
    if (chain_exists && !ec) {
      if (auto loaded = deps.ledger->load(); !loaded) {
        fail_step(outcome, BootStep::OpenLedger, ExitClass::FailClosedNeedsHuman,
                  std::move(loaded).error(), deps);
        return outcome;
      }
      if (auto verified = deps.ledger->verify_chain(); !verified) {
        // A broken chain is tamper or corruption. NEVER auto-restart into it.
        fail_step(outcome, BootStep::OpenLedger, ExitClass::FailClosedNeedsHuman,
                  std::move(verified).error(), deps);
        return outcome;
      }
    }
    if (auto anchored = deps.ledger->verify_against_checkpoint(pinned.value()); !anchored) {
      fail_step(outcome, BootStep::OpenLedger, ExitClass::FailClosedNeedsHuman,
                std::move(anchored).error(), deps);
      return outcome;
    }
  }

  // ── 4. Establish / validate the broker session ───────────────────────────
  if (!deps.session_probe) {
    fail_step(outcome, BootStep::EstablishSession, ExitClass::FailClosedNeedsHuman,
              not_wired("the broker session probe"), deps);
    return outcome;
  }
  {
    auto state = deps.session_probe();
    if (!state) {
      // PRESERVE the transport verdict: a 5xx on the probe is a Crash (retry with
      // backoff), while an Auth/SessionExpired failure is a human re-login. This
      // is the one place where collapsing the two would have been very expensive.
      const ExitClass exit_class = exit_class_for(state.error());
      fail_step(outcome, BootStep::EstablishSession, exit_class, std::move(state).error(), deps);
      return outcome;
    }
    outcome.session_state = state.value();
    if (auto healthy = session::session_state_to_result(state.value()); !healthy) {
      // Kite has no headless refresh: a dead daily token needs an operator.
      fail_step(outcome, BootStep::EstablishSession, ExitClass::FailClosedNeedsHuman,
                std::move(healthy).error(), deps);
      return outcome;
    }
  }

  // ── 5. The safe-start gate — ALL TEN checks, every one wired to a real
  //        component (see make_safe_start_context) ───────────────────────────
  {
    SafeStartWiring wiring;
    wiring.config = &outcome.config;
    wiring.args = &deps.args;
    wiring.env = deps.env;
    wiring.secrets = deps.secrets;
    wiring.token_key_secret = token_key_secret_name(outcome.data_dir->account_id());
    wiring.pinned_ledger_public_key = outcome.data_dir->root() / kLedgerPublicKeyFile;
    wiring.clock = deps.clock;
    wiring.clock_detector = deps.clock_detector;
    // The probe runs a SECOND time inside the gate, on purpose: the gate verifies
    // the world, it does not trust a value someone else read a moment ago. The
    // probe is a lightweight authenticated read (see KiteSessionEstablisher).
    wiring.session_probe = deps.session_probe;
    wiring.instruments = deps.instruments;
    wiring.calendar = deps.calendar;
    wiring.store = deps.store;

    const session::SafeStartContext ctx = make_safe_start_context(wiring, outcome.audit);
    // Braces, not bare declaration: SafeStartGate has no user-provided default
    // constructor, so a const object of it must be VALUE-initialized.
    const session::SafeStartGate gate{};
    if (auto verdict = gate.verify(ctx); !verdict) {
      // ALWAYS FAIL-CLOSED, whatever the inner category. A safe-start refusal is
      // the gate saying the world is not verifiable; auto-restarting into the same
      // refusal is a flap loop against the broker, and 70 is precisely the code
      // the committed systemd unit refuses to restart on.
      fail_step(outcome, BootStep::SafeStart, ExitClass::FailClosedNeedsHuman,
                std::move(verdict).error(), deps);
      return outcome;
    }
  }

  // ── 6. Compose the broker (IMP-12 / Story 6.3 load-time capability gate) ──
  {
    auto assembly = composition::make_broker(outcome.config, deps.broker_deps, deps.broker_options);
    if (!assembly) {
      fail_step(outcome, BootStep::ComposeBroker, ExitClass::FailClosedNeedsHuman,
                std::move(assembly).error(), deps);
      return outcome;
    }
    // The mechanical backstop behind the FixtureCertification naming convention:
    // a test posture that survived review still refuses to start the process.
    if (auto posture = composition::assert_production_posture(assembly.value()); !posture) {
      fail_step(outcome, BootStep::ComposeBroker, ExitClass::FailClosedNeedsHuman,
                std::move(posture).error(), deps);
      return outcome;
    }
    outcome.broker = std::move(assembly).value();
  }

  // ── 7. Compose the engine (IMP-12 refusal matrix) ────────────────────────
  {
    auto assembly = composition::make_engine(deps.engine_deps, deps.engine_options);
    if (!assembly) {
      fail_step(outcome, BootStep::ComposeEngine, ExitClass::FailClosedNeedsHuman,
                std::move(assembly).error(), deps);
      return outcome;
    }
    outcome.engine = std::move(assembly).value();
  }

  // ── 8. Publish the initial health snapshot ───────────────────────────────
  if (deps.health == nullptr) {
    fail_step(outcome, BootStep::PublishHealth, ExitClass::FailClosedNeedsHuman,
              not_wired("the health state"), deps);
    return outcome;
  }
  {
    int in_flight = 0;
    if (deps.store != nullptr) {
      auto rows = deps.store->all_orders();
      if (!rows) {
        const ExitClass exit_class = exit_class_for(rows.error());
        fail_step(outcome, BootStep::PublishHealth, exit_class, std::move(rows).error(), deps);
        return outcome;
      }
      for (const domain::Order& order : rows.value()) {
        if (is_in_flight(order)) {
          ++in_flight;
        }
      }
    }
    const bool clock_sane = deps.clock_detector != nullptr &&
                            deps.clock_detector->status() == clock::ClockStatus::Healthy;
    const cli::HealthSnapshot snapshot(
        outcome.session_state.value_or(session::SessionState::Failed),
        /*heartbeat_age_ms=*/0, kNoTickYet, in_flight, clock_sane, deps.replay_clean);
    deps.health->publish(snapshot);
    outcome.health_published = deps.health->has_published();
    if (!outcome.health_published) {
      fail_step(outcome, BootStep::PublishHealth, ExitClass::Crash,
                blocking(ErrorCategory::Internal,
                         "boot: the health state did not record the initial snapshot"),
                deps);
      return outcome;
    }
  }

  // ── 9. Start the localhost health endpoint ───────────────────────────────
  if (!deps.start_health_endpoint) {
    fail_step(outcome, BootStep::StartHealthEndpoint, ExitClass::FailClosedNeedsHuman,
              not_wired("the health endpoint"), deps);
    return outcome;
  }
  {
    auto started = deps.start_health_endpoint();
    if (!started) {
      // Classified by the seam's own Error: a bind refused because a sibling has
      // not let go of the port yet is a Crash (backoff), a misconfigured host is
      // a human fix.
      const ExitClass exit_class = exit_class_for(started.error());
      fail_step(outcome, BootStep::StartHealthEndpoint, exit_class, std::move(started).error(),
                deps);
      return outcome;
    }
    outcome.health_endpoint_started = true;
  }

  // ── 10. The run phase — STUBBED. See RunPhaseFn / unimplemented_run_phase.
  if (!deps.run_phase) {
    fail_step(outcome, BootStep::Run, ExitClass::FailClosedNeedsHuman, not_wired("the run phase"),
              deps);
    return outcome;
  }
  {
    // Recorded BEFORE the call so a failing run phase still shows that boot
    // reached the hand-off — the flag answers "did boot complete?", not "did the
    // run phase succeed?".
    outcome.run_phase_entered = true;
    auto ran = deps.run_phase();
    if (!ran) {
      const ExitClass exit_class = exit_class_for(ran.error());
      fail_step(outcome, BootStep::Run, exit_class, std::move(ran).error(), deps);
      return outcome;
    }
  }

  outcome.ok = true;
  outcome.step = BootStep::Run;
  outcome.exit_class = ExitClass::Clean;
  return outcome;
}

}  // namespace broker_exec::boot
