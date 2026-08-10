#pragma once

// broker_exec::boot — THE PROCESS BOOT SEQUENCE AND THE EXIT-CODE CONTRACT
// (IMP-20, FR-33, architecture TO-5 / ID-1).
//
// ── THE HOLE THIS CLOSES ────────────────────────────────────────────────────
// Until this module there was NO `int main` anywhere in src/. Every safety
// component was built and tested as an island with no production caller: no
// `SafeStartContext` was constructed outside a test, the ledger had no
// production caller, and the IMP-12 composition root was never invoked. Worse,
// deploy/systemd/broker-exec@.service (Story 6.5b) points `ExecStart` at a
// `broker-exec` binary this repo did not build, and asserts
// `RestartPreventExitStatus=70` against an exit-code contract nothing honoured.
//
// This module is the boot half of that program. `boot()` is the whole cold-boot
// sequence as ONE testable function; `src/main/main.cpp` is a thin shell over it
// that owns only argv parsing, component construction and the top-level catch.
//
// ── EXPLICITLY OUT OF SCOPE: THE TRADING MAIN LOOP ──────────────────────────
// The synchronous trading loop (dispatcher pumping, reconcile scheduling,
// market-data ingest, detector fan-in) is NOT here and is NOT faked. It is a
// large separate story, and `composition/engine_assembly.hpp` already documents
// that boundary ("THEY WIRE AT THE DISPATCHER/RECONCILE LAYER, which is the only
// unwired layer remaining"). The final boot step hands off to the `RunPhaseFn`
// seam; the shipped default is `unimplemented_run_phase()`, which REFUSES with a
// typed error rather than pretending to trade. See that function.
//
// ── THE SEQUENCE (fixed order; the FIRST failure short-circuits) ────────────
//   1. LoadConfig          config::load()               -> fail-closed
//   2. ResolveDataDir      accounts::AccountDataDir     -> create + ensure (0700)
//   3. OpenLedger          ledger::Ledger               -> load + verify_chain +
//                                                          verify_against_checkpoint
//   4. EstablishSession    the injected session probe   -> session::SessionState
//   5. SafeStart           session::SafeStartGate       -> ALL TEN checks
//   6. ComposeBroker       composition::make_broker     + assert_production_posture
//   7. ComposeEngine       composition::make_engine
//   8. PublishHealth       cli::HealthState::publish
//   9. StartHealthEndpoint the injected localhost-endpoint seam
//  10. Run                 the injected RunPhaseFn seam (STUBBED — see above)
//
// NOTHING AFTER A FAILING STEP RUNS. That is the property the tests assert with
// spies: when safe-start refuses, the engine is never built.
//
// ── THE EXIT-CODE CONTRACT (supervisor/supervisor_policy.hpp) ───────────────
//   0   ExitClass::Clean                -> intended stop; systemd does not restart
//   70  ExitClass::FailClosedNeedsHuman -> NEVER auto-restart (the committed
//                                          unit's RestartPreventExitStatus=70)
//   1   ExitClass::Crash                -> restart with capped backoff
// `exit_code_for()` produces exactly the codes `supervisor::exit_reason_from_code`
// maps back, and a test round-trips every class through it. A SAFE-START REFUSAL
// IS ALWAYS 70, whatever the inner error category: auto-restarting into the same
// refusal is a flap loop against the broker, and the gate exists to be believed.
//
// ── THE ABSENCE ALARM (TO-5) ───────────────────────────────────────────────
// Before returning on the FAIL-CLOSED class — for ANY step, not just safe-start —
// boot fires one Critical alert through the injected `ports::AlertSink`. A
// failing delivery does NOT change the exit code: the ABSENCE of the process is
// itself the alarm (that is the dead-man's-switch posture, and the systemd unit's
// `OnFailure=broker-exec-alarm@%i.service` is the OS half of it).
//
// ── NO-THROW ────────────────────────────────────────────────────────────────
// Nothing in this module throws. `boot()` returns a `BootOutcome` value carrying
// the verdict; there is no exception path of its own. An INJECTED seam that
// throws (a `std::function` is caller-supplied code) propagates through, and
// main() contains it at the very top — main IS the boundary, and it maps a
// escaped exception to the Crash class.
//
// Cross-platform: C++20 standard library + std::filesystem. No OS API, no
// `#ifdef`, no float.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "broker_exec/accounts/account_data_dir.hpp"
#include "broker_exec/cli/health_state.hpp"
#include "broker_exec/clock/skew_stall_detector.hpp"
#include "broker_exec/composition/broker_factory.hpp"
#include "broker_exec/composition/engine_assembly.hpp"
#include "broker_exec/config/config.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ledger/ledger.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/ports/clock_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/ports/secret_provider.hpp"
#include "broker_exec/refdata/instrument_master.hpp"
#include "broker_exec/refdata/trading_calendar.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/session/safe_start.hpp"
#include "broker_exec/session/session_state.hpp"
#include "broker_exec/store/store.hpp"
#include "broker_exec/supervisor/supervisor_policy.hpp"

namespace broker_exec::boot {

// ── The exit classes ────────────────────────────────────────────────────────
//
// A CLOSED enum whose members map 1:1 onto `supervisor::ExitReason`. The default
// member of every fail path is `Crash` only where a retry can genuinely help;
// everything a human must fix is `FailClosedNeedsHuman`.
enum class ExitClass {
  Clean,                 // -> supervisor::kExitClean (0)
  FailClosedNeedsHuman,  // -> supervisor::kExitFailClosedNeedsHuman (70)
  Crash                  // -> kExitCrash (1); ANY unlisted code is a crash too
};

// The canonical crash code this binary uses. `supervisor::exit_reason_from_code`
// maps ANY code that is neither 0 nor 70 to Crash, so the specific value is a
// convention rather than a contract — 1 is the conventional generic failure.
inline constexpr int kExitCrash = 1;

// Stable, log/serialization-friendly names (NFR-8 observability contract).
[[nodiscard]] std::string_view to_string(ExitClass exit_class) noexcept;

// The exit code a class produces. Exactly the values in supervisor_policy.hpp,
// so the committed systemd unit's RestartPreventExitStatus=70 is correct.
[[nodiscard]] int exit_code_for(ExitClass exit_class) noexcept;

// Classify a typed Error into an exit class.
//
// TRANSPORT CONDITIONS ARE A CRASH, EVERYTHING ELSE NEEDS A HUMAN. Network /
// Timeout / Transient / RateLimited are exactly the conditions a capped backoff
// fixes, so they take the restart path whatever their action says. Every other
// category is decided by the normalized `SuggestedAction`: RetrySafe and
// ReconcileFirst are restartable, and DoNotRetry / BlockStrategy /
// ReEstablishSession / Cancel / SquareOff / RaiseAlert all mean a human.
[[nodiscard]] ExitClass exit_class_for(const errors::Error& error) noexcept;

// ── The steps ───────────────────────────────────────────────────────────────
enum class BootStep {
  LoadConfig,
  ResolveDataDir,
  OpenLedger,
  EstablishSession,
  SafeStart,
  ComposeBroker,
  ComposeEngine,
  PublishHealth,
  StartHealthEndpoint,
  Run
};

// Stable, log/serialization-friendly step names.
[[nodiscard]] std::string_view to_string(BootStep step) noexcept;

// ── The ten safe-start checks ───────────────────────────────────────────────
//
// The identity of each of `session::SafeStartContext`'s ten members, in the
// gate's FIXED order. These exist so the audit below can say WHICH checks
// actually ran — a claim that would otherwise be unverifiable from outside
// `SafeStartGate::verify`.
enum class SafeCheckId {
  Config,
  StrategyNames,
  CryptoKeys,
  Clock,
  Session,
  EgressIp,
  InstrumentMaster,
  Calendar,
  LegacyStops,
  Reconciliation
};

inline constexpr std::size_t kSafeCheckCount = 10;

// Stable names, matching the gate's own check names in safe_start.cpp so an
// audit line and an Error message agree.
[[nodiscard]] std::string_view to_string(SafeCheckId id) noexcept;

// Invocation counters for the ten checks.
//
// WHY THIS IS NOT TEST SCAFFOLDING. `SafeStartGate` runs the ten checks
// internally, so "is every check wired to a real implementation?" is invisible to
// a caller: a context whose `calendar_check` was never wired fails closed with a
// "not configured" error, but a context whose calendar check was wired to a
// lambda that returns ok() looks IDENTICAL to one wired to
// `TradingCalendar::require_fresh()`. The counters make the difference
// observable: `make_safe_start_context` increments a check's counter ONLY when it
// is about to delegate to the real component behind it, so an unwired check
// leaves its counter at zero and `all_invoked()` false.
class SafeStartAudit {
 public:
  // Record that the check `id` delegated to its real implementation.
  void record(SafeCheckId id) noexcept;

  [[nodiscard]] int count(SafeCheckId id) const noexcept;

  // Total delegations across all ten checks.
  [[nodiscard]] int total() const noexcept;

  // True iff every one of the ten checks delegated at least once — i.e. the gate
  // ran to completion over ten real implementations.
  [[nodiscard]] bool all_invoked() const noexcept;

  // The id of the first check that never delegated, or nullopt when all did.
  // Diagnostic: names the missing wire rather than only reporting a count.
  [[nodiscard]] std::optional<SafeCheckId> first_missing() const noexcept;

 private:
  std::array<int, kSafeCheckCount> invocations_{};
};

// ── Arguments (what argv / the systemd unit supplies) ───────────────────────
//
// The shipped unit passes `run --account %i --data-root ... --shared-refdata ...`
// and NO `--config`; `config_path` is therefore allowed to be empty, which
// `config::load` reads as "defaults + environment only".
struct BootArgs {
  // The account id — systemd's `%i`. Validated by
  // `accounts::validate_account_id` at the ResolveDataDir step, and cross-checked
  // against `config.engine.account_id` by the config safe-start check.
  std::string account_id;
  // The TOML path. Empty => skip the file layer (defaults + env).
  std::filesystem::path config_path;
  // The parent every account directory is a sibling in. Empty => take
  // `config.paths.data_dir`; when both are set they must AGREE (see
  // `resolve_data_root`).
  std::filesystem::path data_root;
  // The shared (broker, segment, date) reference-data cache root. Owned by
  // main(), which wires it into the refdata fetch seams; boot only records it.
  std::filesystem::path shared_refdata_root;

  // The out-of-band operator surface. LOOPBACK ONLY — the endpoint is never
  // exposed off-box (cli/health_endpoint.hpp AC-3).
  std::string health_host = "127.0.0.1";
  int health_port = 9110;
  // The liveness SLO the endpoint judges the heartbeat against (ms, no float).
  std::int64_t health_live_budget_ms = 15000;
};

// ── Seams ───────────────────────────────────────────────────────────────────

// The broker-neutral daily-session probe. Kite wires
// `KiteSessionEstablisher::validate()`; Kotak wires its session validator. It
// returns a `Result<SessionState>` and NOT a bare state so a transport failure
// stays a transport failure — collapsing a 5xx into `Failed` would mislabel a
// retryable outage as a dead session (the wiring note on
// `session::session_state_to_result`).
using SessionProbeFn = std::function<Result<session::SessionState>()>;

// Starts serving the localhost health surface. Production wires
// `cli::HealthHttpServer::listen()`; a test wires a spy, so the suite never binds
// a socket (the same posture cli's own tests take).
using StartHealthEndpointFn = std::function<Result<ports::Ok>()>;

// ── THE RUN PHASE IS A STUB. THIS SEAM IS THE HAND-OFF POINT. ───────────────
//
// The synchronous trading main loop is OUT OF SCOPE for IMP-20 (see the file
// banner). This seam is where it will attach; nothing in this repository
// implements it yet.
using RunPhaseFn = std::function<Result<ports::Ok>()>;

// The shipped default run phase: it REFUSES, with a typed
// `Internal`/`BlockStrategy` error naming the missing story.
//
// READ WHAT THAT MEANS OPERATIONALLY: a production `broker-exec run` performs the
// entire cold-boot sequence, proves the world is safe, publishes health — and
// then exits 70, "a human is required". That is deliberate and it is the honest
// answer: this binary cannot trade yet, and the alternative (a loop that spins
// doing nothing) would report a healthy trading engine that places no orders,
// which is a far worse lie to hand an operator. Replace this seam, do not soften
// it.
[[nodiscard]] RunPhaseFn unimplemented_run_phase();

// ── Injected dependencies ───────────────────────────────────────────────────
//
// LIFETIME: every pointer is BORROWED and MUST outlive the `boot()` call. They
// are raw pointers, not shared ownership, because main() owns every one of them
// on its stack for the whole process lifetime.
//
// EVERY REQUIRED POINTER IS VALIDATED before the sequence starts: a null one is a
// typed `Internal`/`BlockStrategy` error naming the field, at the step that would
// have used it. There is no defaulted stand-in for any of them.
struct BootDeps {
  BootArgs args;

  // The environment seam. Used by BOTH the config loader and the egress-IP
  // allow-list check, so a test injects one deterministic map for both and
  // production reads the real environment through `config::default_env_lookup()`.
  config::EnvLookup env;

  // ── Borrowed components ───────────────────────────────────────────────────
  const ports::ClockPort* clock = nullptr;
  // The token-store key source (SE-5). Consulted by the crypto-keys check, which
  // never logs, echoes or retains the value.
  const ports::SecretProvider* secrets = nullptr;
  // The absence-alarm sink (TO-5). Non-const: send() mutates the sink's
  // heartbeat.
  ports::AlertSink* alerts = nullptr;
  // The clock health watch. boot takes its BASELINE sample at the top of the
  // sequence so the gate's sample is a genuine second observation.
  clock::SkewStallDetector* clock_detector = nullptr;
  // The SQLite projection, already opened by main(). Read by the legacy-stop and
  // reconciliation checks and by the in-flight count on the health snapshot.
  const store::Store* store = nullptr;
  // The tamper-evident ledger, constructed (not yet loaded) by main().
  ledger::Ledger* ledger = nullptr;
  // Reference data, constructed by main() over the shared-cache fetch seams.
  // NON-const: the freshness checks refresh a stale master/calendar in place.
  refdata::InstrumentMaster* instruments = nullptr;
  refdata::TradingCalendar* calendar = nullptr;
  // The latest-snapshot holder the health endpoint reads.
  cli::HealthState* health = nullptr;

  // ── Seams ─────────────────────────────────────────────────────────────────
  SessionProbeFn session_probe;
  StartHealthEndpointFn start_health_endpoint;
  RunPhaseFn run_phase;

  // The account-directory permission seam. Empty => the production seam
  // (`platform::restrict_to_owner_dir`); tests inject a spy.
  accounts::DirPermissionFn dir_permissions;

  // ── Composition-root inputs ───────────────────────────────────────────────
  // Handed straight to `composition::make_broker` / `make_engine`. boot does not
  // synthesize any of these: the refusal matrices in IMP-12 are the point, and a
  // default-constructed `BrokerOptions{}`/`EngineOptions{}` invented here would
  // be exactly the ungated composition those matrices exist to prevent.
  composition::BrokerDeps broker_deps;
  composition::BrokerOptions broker_options;
  composition::EngineDeps engine_deps;
  composition::EngineOptions engine_options;

  // ── Facts main() already knows ────────────────────────────────────────────
  // False when the projection had to be dropped and rebuilt (the `needs_rebuild`
  // of `store::Store::open_or_rebuild`). Published as
  // `HealthSnapshot::replay_clean`, which `is_ready` requires.
  bool replay_clean = true;
};

// ── The outcome ─────────────────────────────────────────────────────────────
//
// Move-only (it carries the two assemblies). Every fail path returns a fully
// populated value — there is no "and also check this out-param".
struct BootOutcome {
  // True only when every step, including the run phase, completed.
  bool ok = false;
  // The step that FAILED, or `Run` on the happy path.
  BootStep step = BootStep::LoadConfig;
  // FAIL-CLOSED DEFAULT: an outcome nobody filled in reads as a crash, never as
  // a clean exit — the same posture `supervisor::exit_reason_from_code` takes on
  // an unrecognized code.
  ExitClass exit_class = ExitClass::Crash;
  // Meaningful only when `!ok`. Redaction-safe by the Error contract.
  errors::Error error;

  // Which of the ten safe-start checks delegated to their real implementation.
  SafeStartAudit audit;

  // The validated configuration (empty-ish when LoadConfig itself failed).
  config::Config config;
  // The resolved per-account layout, present from ResolveDataDir onward.
  std::optional<accounts::AccountDataDir> data_dir;
  // The session state the probe reported, present from EstablishSession onward.
  std::optional<session::SessionState> session_state;

  // The composed assemblies. Present ONLY when their step ran and succeeded —
  // which is exactly how a test proves "the engine is never built when
  // safe-start refuses".
  std::optional<composition::BrokerAssembly> broker;
  std::optional<composition::EngineAssembly> engine;

  // ── Evidence trail ────────────────────────────────────────────────────────
  bool health_published = false;
  bool health_endpoint_started = false;
  bool run_phase_entered = false;
  // The absence alarm was ATTEMPTED (the fail-closed class was reached).
  bool absence_alarm_fired = false;
  // ...and the sink accepted it. A false here does NOT change the exit code:
  // absence is the alarm.
  bool absence_alarm_delivered = false;
};

// Run the cold-boot sequence. Never throws of its own accord (an injected seam
// that throws propagates; main is the boundary). See the file banner for the
// step order and the exit-code mapping.
[[nodiscard]] BootOutcome boot(const BootDeps& deps);

// ── The safe-start wiring (exposed so it is testable on its own) ────────────
//
// Everything the ten checks read. EVERY member is a real component or the seam a
// real component is reached through — there is no "returns ok()" placeholder
// anywhere in this struct or in the context it builds.
struct SafeStartWiring {
  // config check
  const config::Config* config = nullptr;
  const BootArgs* args = nullptr;
  config::EnvLookup env;

  // crypto-keys check
  const ports::SecretProvider* secrets = nullptr;
  // The logical name TokenStore itself derives: `<account_id>.token_key`. Built
  // by `token_key_secret_name()` so the boot check and the store can never
  // disagree about which secret is required.
  std::string token_key_secret;
  // The PINNED Ed25519 public key, provisioned out-of-band under the account
  // tree as `ledger_public_key.hex` (see ledger.hpp's trust model). Its ABSENCE
  // fails the check closed — the ledger's truncation/rollback detection has no
  // anchor without it.
  std::filesystem::path pinned_ledger_public_key;

  // clock check
  const ports::ClockPort* clock = nullptr;
  clock::SkewStallDetector* clock_detector = nullptr;

  // session check
  SessionProbeFn session_probe;

  // refdata checks (NON-const: a stale master/calendar is refreshed in place)
  refdata::InstrumentMaster* instruments = nullptr;
  refdata::TradingCalendar* calendar = nullptr;

  // legacy-stop + reconciliation checks
  const store::Store* store = nullptr;
};

// Build the ten checks over `wiring`, each instrumented to `audit`.
//
// `audit` MUST outlive the returned context (the checks capture it by reference).
// A wiring member that is null yields a check that fails closed naming the
// missing component — it does NOT yield an empty `std::function`, because an
// empty one would report "not configured" without saying which component was
// absent, and because it must never be callable into a `bad_function_call`.
[[nodiscard]] session::SafeStartContext make_safe_start_context(const SafeStartWiring& wiring,
                                                                SafeStartAudit& audit);

// The logical secret name `secrets::TokenStore` derives for an account.
[[nodiscard]] std::string token_key_secret_name(std::string_view account_id);

// ── The real check implementations this module owns ─────────────────────────
//
// These are the checks that had no home in an existing module. Each is a plain,
// separately testable function; none of them is a lambda that returns ok().

// The account-identity and broker-roster integrity of a loaded configuration.
// Fails closed when:
//   * `engine.account_id` is empty, or disagrees with the account id this
//     process was STARTED for (a systemd `%i` / config mismatch would otherwise
//     open a DIFFERENT account's tree — the exact cross-account contamination
//     `AccountDataDir` exists to prevent);
//   * `broker.name` is not a broker this build can compose
//     (`composition::parse_broker_choice`) — the roster check `config` cannot
//     make, because config must not know the adapter roster;
//   * neither `paths.data_dir` nor `--data-root` names a data root.
[[nodiscard]] Result<ports::Ok> require_config_consistent(const config::Config& config,
                                                          const BootArgs& args);

// The data root to use: `--data-root` when set, else `config.paths.data_dir`.
// When BOTH are set they must name the same directory (compared lexically
// normalized, trailing separator ignored) — a disagreement is a deployment fault
// and is refused rather than silently resolved in one side's favour.
[[nodiscard]] Result<std::filesystem::path> resolve_data_root(const config::Config& config,
                                                              const BootArgs& args);

// The number of bytes a token-store key must have (AES-256). Mirrors the
// constant `secrets::TokenStore` enforces; stated here so the boot check refuses
// a wrong-sized key BEFORE a session write discovers it.
inline constexpr std::size_t kTokenKeyBytes = 32;

// Ed25519 raw public key length.
inline constexpr std::size_t kEd25519PublicKeyBytes = 32;

// Crypto-key PRESENCE (SE-5). Both keys this process depends on must exist and
// be well-formed BEFORE trading:
//   * the per-account AES-256 token-store key, resolved through the
//     `SecretProvider` — required, and required to be exactly 32 bytes;
//   * the pinned Ed25519 ledger public key on disk — required, because
//     `Ledger::verify_against_checkpoint` anchors truncation/rollback detection
//     to a key supplied OUT-OF-BAND, and the checkpoint file's own embedded key
//     is attacker-writable.
// NEVER echoes, logs or returns key material; the resolved token key is
// overwritten before it leaves scope.
[[nodiscard]] Result<ports::Ok> require_crypto_keys(
    const ports::SecretProvider& secrets, const std::string& token_key_secret,
    const std::filesystem::path& pinned_ledger_public_key);

// Read a 32-byte Ed25519 public key from a lowercase-hex file (the format
// `Ledger::write_public_key` produces). Surrounding whitespace is ignored;
// anything else — a missing file, an odd length, a non-hex byte, a wrong size —
// is a typed Error. Fail closed: no partial key is ever returned.
[[nodiscard]] Result<std::vector<unsigned char>> read_pinned_public_key(
    const std::filesystem::path& path);

// The environment keys the egress-IP allow-list check reads.
//
//   BROKER_EXEC_EGRESS_IP            — the egress address this deployment is
//                                      CURRENTLY presenting, as observed
//                                      out-of-band (an ExecStartPre probe, the
//                                      NAT's fixed address, the operator).
//   BROKER_EXEC_EGRESS_IP_ALLOWLIST  — comma-separated addresses registered with
//                                      the broker.
inline constexpr std::string_view kEgressIpEnv = "BROKER_EXEC_EGRESS_IP";
inline constexpr std::string_view kEgressAllowlistEnv = "BROKER_EXEC_EGRESS_IP_ALLOWLIST";

// Egress-IP allow-list match, read through the SAME environment seam the config
// loader uses.
//
// BE HONEST ABOUT WHAT THIS IS. It is an EXACT STRING comparison of a declared
// observation against a declared allow-list — no CIDR, no DNS, and no probe of
// its own: this library has no "what is my egress address" transport seam, and
// inventing one would be new transport (out of scope) pointed at a third-party
// echo service. It fails CLOSED in every ambiguous case: an unset or empty
// observation, an unset or empty allow-list, or a value not in the list all
// block the start. It is the weakest of the ten checks and it is deliberately
// impossible to satisfy by accident.
[[nodiscard]] Result<ports::Ok> require_egress_ip_allowed(const config::EnvLookup& env);

// True for an order whose FATE IS NOT KNOWN: PendingSend, Sent, Unknown,
// PartiallyPlaced or ManualInterventionRequired. Acknowledged and PartiallyFilled
// are deliberately NOT here — those are WORKING orders whose state the broker has
// confirmed, and restarting into one is ordinary.
[[nodiscard]] bool is_unreconciled(const domain::Order& order) noexcept;

// The cold-boot reconciliation-completeness precondition.
//
// SCOPE, STATED PLAINLY: this is the LOCAL-PROJECTION half of reconciliation. It
// refuses to start when the projection still holds an order whose send result was
// never confirmed, because trading on top of one is precisely the duplicate-order
// hazard this library exists to prevent, and resolving it requires broker truth
// plus the UNKNOWN resolver — i.e. `reconcile::RecoveryCoordinator`, which runs on
// the main loop that IMP-20 does not build. It is a fail-closed precondition, not
// a substitute for that coordinator. Redaction-safe: a count and a client_ref
// only, never a price or a quantity.
[[nodiscard]] Result<ports::Ok> require_no_unreconciled_orders(
    const std::vector<domain::Order>& orders);

}  // namespace broker_exec::boot
