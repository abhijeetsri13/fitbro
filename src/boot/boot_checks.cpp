// The ten safe-start checks, wired to REAL components (IMP-20).
//
// This translation unit owns two things:
//   * the check implementations that had no home in an existing module
//     (config consistency, crypto-key presence, the egress-IP allow-list, the
//     cold-boot reconciliation precondition), each a plain testable function;
//   * `make_safe_start_context`, which binds all TEN members of
//     `session::SafeStartContext` to a real component call and instruments each
//     one so the audit can prove it delegated.
//
// THERE IS NO `[] { return ports::ok(); }` ANYWHERE IN THIS FILE, by design. A
// check that cannot reach its component fails closed NAMING the component; it is
// never softened into a pass.
//
// No-throw, no float, no OS API, no `#ifdef`. C++20 standard library only.

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "broker_exec/boot/boot.hpp"
#include "broker_exec/domain/enums.hpp"

namespace broker_exec::boot {

namespace fs = std::filesystem;

namespace {

using errors::Error;
using errors::ErrorCategory;
using errors::make_error;
using errors::SuggestedAction;

// A boot-time refusal that must HALT the process rather than be retried. Every
// error this file raises is one of these unless it is propagating an inner error
// from a real component (in which case the inner category/action are preserved,
// exactly as `SafeStartGate` does when it wraps).
[[nodiscard]] Error blocking(ErrorCategory category, std::string message) {
  Error err = make_error(category, std::move(message));
  err.action = SuggestedAction::BlockStrategy;
  return err;
}

// A required component was not supplied to the wiring. Internal, because it is a
// composition fault rather than a data fault, but BlockStrategy because the
// process must stop (Internal's default is RaiseAlert).
[[nodiscard]] Error missing_component(SafeCheckId id, std::string_view component) {
  return blocking(ErrorCategory::Internal,
                  std::string("boot: the ") + std::string(to_string(id)) +
                      " check has no " + std::string(component) +
                      " to consult (composition fault: the component was not wired)");
}

// Trim ASCII spaces/tabs from both ends. Used on env-supplied list entries; it is
// deliberately ASCII-only (a Unicode space is NOT silently accepted — that is the
// class of hole the alerting allowlist finding covered).
[[nodiscard]] std::string_view trim_ascii(std::string_view text) noexcept {
  const auto is_space = [](char c) noexcept { return c == ' ' || c == '\t'; };
  while (!text.empty() && is_space(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && is_space(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

// A path in comparable form: lexically normalized, forward slashes, no trailing
// separator. `"/a/b/"` and `"/a/b"` name the same directory, and treating them as
// different would let a trailing slash defeat the data-root agreement check.
[[nodiscard]] std::string comparable_path(const fs::path& path) {
  std::string text = path.lexically_normal().generic_string();
  while (text.size() > 1 && text.back() == '/') {
    text.pop_back();
  }
  return text;
}

// Decode one lowercase/uppercase hex digit, or -1.
[[nodiscard]] int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// Build ONE instrumented safe-start check.
//
// `inner` is EMPTY exactly when a component the check needs is absent. In that
// case the returned check fails closed with `missing_detail` naming the component
// and — this is the load-bearing part — does NOT record an invocation, so
// `SafeStartAudit::all_invoked()` stays false. A check that merely FAILS still
// counts as invoked: it reached its real implementation and that implementation
// said no.
[[nodiscard]] session::SafeCheck instrumented(SafeCheckId id, SafeStartAudit& audit,
                                              session::SafeCheck inner, Error missing_detail) {
  return [id, &audit, check = std::move(inner),
          absent = std::move(missing_detail)]() -> Result<ports::Ok> {
    if (!check) {
      return fail(absent);
    }
    audit.record(id);
    return check();
  };
}

}  // namespace

// ── Names ───────────────────────────────────────────────────────────────────

std::string_view to_string(SafeCheckId id) noexcept {
  // These MATCH the gate's own check names in safe_start.cpp, so an audit line
  // and a "safe-start: <name> check failed" message name the same thing.
  switch (id) {
    case SafeCheckId::Config:
      return "config";
    case SafeCheckId::StrategyNames:
      return "strategy-names";
    case SafeCheckId::CryptoKeys:
      return "crypto-keys";
    case SafeCheckId::Clock:
      return "clock";
    case SafeCheckId::Session:
      return "session";
    case SafeCheckId::EgressIp:
      return "egress-IP";
    case SafeCheckId::InstrumentMaster:
      return "instrument-master";
    case SafeCheckId::Calendar:
      return "calendar";
    case SafeCheckId::LegacyStops:
      return "legacy-stops";
    case SafeCheckId::Reconciliation:
      return "reconciliation";
  }
  return "unknown";
}

// ── SafeStartAudit ──────────────────────────────────────────────────────────

void SafeStartAudit::record(SafeCheckId id) noexcept {
  const auto index = static_cast<std::size_t>(id);
  if (index < kSafeCheckCount) {
    ++invocations_[index];
  }
}

int SafeStartAudit::count(SafeCheckId id) const noexcept {
  const auto index = static_cast<std::size_t>(id);
  return index < kSafeCheckCount ? invocations_[index] : 0;
}

int SafeStartAudit::total() const noexcept {
  int sum = 0;
  for (const int value : invocations_) {
    sum += value;
  }
  return sum;
}

bool SafeStartAudit::all_invoked() const noexcept { return !first_missing().has_value(); }

std::optional<SafeCheckId> SafeStartAudit::first_missing() const noexcept {
  for (std::size_t i = 0; i < kSafeCheckCount; ++i) {
    if (invocations_[i] == 0) {
      return static_cast<SafeCheckId>(i);
    }
  }
  return std::nullopt;
}

// ── The checks this module owns ─────────────────────────────────────────────

std::string token_key_secret_name(std::string_view account_id) {
  // EXACTLY what secrets::TokenStore::resolve_key derives. Keeping the two in one
  // spelling is the point: a boot check that asked for a different name would
  // pass while the store still could not open its blob.
  return std::string(account_id) + ".token_key";
}

Result<ports::Ok> require_config_consistent(const config::Config& cfg, const BootArgs& args) {
  if (cfg.engine.account_id.empty()) {
    return fail(blocking(ErrorCategory::Validation,
                         "boot: config engine.account_id is empty — the per-account data tree, "
                         "the token-store key name and the ledger all key off it"));
  }
  if (!args.account_id.empty() && cfg.engine.account_id != args.account_id) {
    // Both ids are validated `[a-z0-9_-]` NAMES by the time they matter, but the
    // config one has not been through validate_account_id yet, so neither is
    // echoed here: a mismatch is reported without quoting either value.
    return fail(blocking(
        ErrorCategory::Validation,
        "boot: the account this process was started for does not match "
        "config engine.account_id — this binary would open a DIFFERENT account's "
        "data tree than its configuration describes. Check the systemd instance "
        "name (%i) against the configuration file"));
  }
  // The broker-roster check `config` deliberately cannot make (it must not know
  // which adapters exist). A typo here would otherwise survive until make_broker.
  if (auto choice = composition::parse_broker_choice(cfg.broker.name); !choice) {
    return fail(std::move(choice).error());
  }
  if (cfg.paths.data_dir.empty() && args.data_root.empty()) {
    return fail(blocking(ErrorCategory::Validation,
                         "boot: no data root — set config paths.data_dir or pass --data-root"));
  }
  return ports::ok();
}

Result<fs::path> resolve_data_root(const config::Config& cfg, const BootArgs& args) {
  const bool have_arg = !args.data_root.empty();
  const bool have_config = !cfg.paths.data_dir.empty();

  if (!have_arg && !have_config) {
    return fail(blocking(ErrorCategory::Validation,
                         "boot: no data root — set config paths.data_dir or pass --data-root"));
  }
  if (have_arg && have_config &&
      comparable_path(args.data_root) != comparable_path(cfg.paths.data_dir)) {
    // Refused rather than resolved in either side's favour: whichever we picked,
    // the other one names a tree that some part of the deployment still believes
    // in, and two beliefs about where the intent log lives is how two processes
    // end up sharing one.
    return fail(blocking(ErrorCategory::Validation,
                         "boot: --data-root and config paths.data_dir name different "
                         "directories; they must agree (they select the tree holding the "
                         "intent log, the projection, the token store and the ledger)"));
  }
  return have_arg ? args.data_root : cfg.paths.data_dir;
}

Result<std::vector<unsigned char>> read_pinned_public_key(const fs::path& path) {
  if (path.empty()) {
    return fail(blocking(ErrorCategory::Validation,
                         "boot: no pinned ledger public key path was configured"));
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return fail(blocking(
        ErrorCategory::Validation,
        "boot: the pinned ledger public key is missing. It anchors the ledger's "
        "truncation/rollback detection and must be provisioned out-of-band as "
        "ledger_public_key.hex under the account directory (see ledger.hpp's trust model)"));
  }

  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) {
    return fail(blocking(ErrorCategory::Internal,
                         "boot: failed to read the pinned ledger public key file"));
  }
  // Strip ALL ASCII whitespace (a hex file written by an editor carries a
  // trailing newline; some carry CRLF).
  text.erase(std::remove_if(text.begin(), text.end(),
                            [](char c) noexcept {
                              return c == '\n' || c == '\r' || c == ' ' || c == '\t';
                            }),
             text.end());

  if (text.size() != kEd25519PublicKeyBytes * 2) {
    return fail(blocking(ErrorCategory::Validation,
                         "boot: the pinned ledger public key must be exactly " +
                             std::to_string(kEd25519PublicKeyBytes) +
                             " bytes of hex (Ed25519 raw public key)"));
  }

  std::vector<unsigned char> key;
  key.reserve(kEd25519PublicKeyBytes);
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const int hi = hex_value(text[i]);
    const int lo = hex_value(text[i + 1]);
    if (hi < 0 || lo < 0) {
      // The offending byte is NOT echoed: this file is operator-supplied and a
      // malformed one is exactly where a paste accident lands.
      return fail(blocking(ErrorCategory::Validation,
                           "boot: the pinned ledger public key is not valid hex"));
    }
    key.push_back(static_cast<unsigned char>((hi << 4) | lo));
  }
  return key;
}

Result<ports::Ok> require_crypto_keys(const ports::SecretProvider& secret_source,
                                      const std::string& token_key_secret,
                                      const fs::path& pinned_ledger_public_key) {
  if (token_key_secret.empty()) {
    return fail(blocking(ErrorCategory::Internal,
                         "boot: no token-store key name was derived for this account"));
  }

  auto resolved = secret_source.get(token_key_secret);
  if (!resolved) {
    // The provider's Error is redaction-safe by contract and names the KEY, never
    // a value; preserve its category/action so an Auth failure stays an Auth
    // failure.
    return fail(std::move(resolved).error());
  }
  const std::size_t key_length = resolved.value().size();
  // Overwrite the key material before it leaves scope. BEST-EFFORT and stated as
  // such: this module does not link OpenSSL, so there is no OPENSSL_cleanse here
  // and a compiler may elide the store. `secrets::TokenStore`, which is the code
  // that actually USES the key, does the guaranteed cleanse.
  std::fill(resolved.value().begin(), resolved.value().end(), '\0');
  if (key_length != kTokenKeyBytes) {
    // The LENGTH is not secret; the bytes are never touched by this message.
    return fail(blocking(ErrorCategory::Validation,
                         "boot: the per-account token-store key must be " +
                             std::to_string(kTokenKeyBytes) +
                             " bytes (AES-256); the configured secret is a different size"));
  }

  if (auto pinned = read_pinned_public_key(pinned_ledger_public_key); !pinned) {
    return fail(std::move(pinned).error());
  }
  return ports::ok();
}

Result<ports::Ok> require_egress_ip_allowed(const config::EnvLookup& env_seam) {
  if (!env_seam) {
    return fail(blocking(ErrorCategory::Internal,
                         "boot: the egress-IP check has no environment seam to read"));
  }

  const std::optional<std::string> observed = env_seam(kEgressIpEnv);
  if (!observed.has_value() || trim_ascii(*observed).empty()) {
    return fail(blocking(ErrorCategory::Validation,
                         std::string("boot: ") + std::string(kEgressIpEnv) +
                             " is not set, so the egress address this deployment presents to "
                             "the broker is unknown. A world we cannot verify is not a world "
                             "we trade in — set it from the out-of-band probe"));
  }
  const std::optional<std::string> allowlist = env_seam(kEgressAllowlistEnv);
  if (!allowlist.has_value() || trim_ascii(*allowlist).empty()) {
    return fail(blocking(ErrorCategory::Validation,
                         std::string("boot: ") + std::string(kEgressAllowlistEnv) +
                             " is not set. An empty allow-list is not 'allow everything' — "
                             "it is an unconfigured check, and it blocks the start"));
  }

  const std::string_view current = trim_ascii(*observed);
  std::string_view rest = *allowlist;
  while (!rest.empty()) {
    const std::size_t comma = rest.find(',');
    const std::string_view entry =
        trim_ascii(comma == std::string_view::npos ? rest : rest.substr(0, comma));
    if (!entry.empty() && entry == current) {
      return ports::ok();
    }
    if (comma == std::string_view::npos) {
      break;
    }
    rest.remove_prefix(comma + 1);
  }

  // NEITHER value is echoed. An egress address is not a secret, but this message
  // travels into an operator alert, and the allow-list is deployment topology.
  return fail(blocking(ErrorCategory::Validation,
                       std::string("boot: the observed egress address (") +
                           std::string(kEgressIpEnv) + ") is not in " +
                           std::string(kEgressAllowlistEnv) +
                           ". Orders from an unregistered address are rejected by the broker "
                           "or, worse, accepted from the wrong host"));
}

bool is_unreconciled(const domain::Order& order) noexcept {
  switch (order.state) {
    case domain::OrderState::PendingSend:
    case domain::OrderState::Sent:
    case domain::OrderState::Unknown:
    case domain::OrderState::PartiallyPlaced:
    case domain::OrderState::ManualInterventionRequired:
      return true;
    case domain::OrderState::Created:
    case domain::OrderState::Validated:
    case domain::OrderState::Acknowledged:
    case domain::OrderState::PartiallyFilled:
    case domain::OrderState::Filled:
    case domain::OrderState::Rejected:
    case domain::OrderState::Cancelled:
    case domain::OrderState::Reconciled:
      return false;
  }
  // Unreachable for the fixed enum; a state nobody classified is, by definition,
  // one whose fate we do not know. Fail closed.
  return true;
}

Result<ports::Ok> require_no_unreconciled_orders(const std::vector<domain::Order>& orders) {
  std::size_t count = 0;
  std::string first_ref;
  std::string_view first_state;
  for (const domain::Order& order : orders) {
    if (!is_unreconciled(order)) {
      continue;
    }
    ++count;
    if (first_ref.empty()) {
      first_ref = order.intent.client_ref;
      first_state = domain::to_string(order.state);
    }
  }
  if (count == 0) {
    return ports::ok();
  }

  // Redaction-safe: a count, a state name and a client_ref (an id the operator
  // needs in order to act) — never a price or a quantity.
  return fail(blocking(
      ErrorCategory::Validation,
      "boot: the projection holds " + std::to_string(count) +
          " order(s) whose send result was never confirmed (first state " +
          std::string(first_state.empty() ? std::string_view("unknown") : first_state) +
          "). Trading on top of one is the duplicate-order hazard this library exists to "
          "prevent: it must be resolved against broker truth before a start. First: " +
          (first_ref.empty() ? std::string("<no client_ref>") : first_ref)));
}

// ── The ten-check wiring ────────────────────────────────────────────────────

session::SafeStartContext make_safe_start_context(const SafeStartWiring& wiring,
                                                  SafeStartAudit& audit) {
  session::SafeStartContext ctx;

  // Everything is captured BY VALUE (pointers, strings, paths, std::functions)
  // so the returned context does NOT depend on `wiring` outliving this call.
  // Only `audit` is captured by reference, and its lifetime requirement is stated
  // on the declaration.

  // 1. config — re-parse the file through the REAL loader, then apply the
  //    cross-field integrity `config::load` cannot: the account identity this
  //    process was started for, and the broker roster.
  {
    const config::Config* cfg = wiring.config;
    const BootArgs* args = wiring.args;
    config::EnvLookup env_seam = wiring.env;
    session::SafeCheck inner;
    if (cfg != nullptr && args != nullptr && env_seam) {
      inner = [cfg, args, env_seam]() -> Result<ports::Ok> {
        // Re-loading (rather than reusing the step-1 value) is deliberate: it
        // proves the configuration ON DISK still parses at gate time, so a file
        // edited or swapped between process start and the gate is caught.
        auto reloaded = config::load(args->config_path, env_seam);
        if (!reloaded) {
          return fail(std::move(reloaded).error());
        }
        if (auto consistent = require_config_consistent(reloaded.value(), *args); !consistent) {
          return consistent;
        }
        return require_config_consistent(*cfg, *args);
      };
    }
    ctx.config_check = instrumented(SafeCheckId::Config, audit, std::move(inner),
                                    missing_component(SafeCheckId::Config,
                                                      "configuration, arguments or environment "
                                                      "seam"));
  }

  // 2. strategy-names — session::require_valid_strategy_names over the ONE place
  //    a strategy name is declared in configuration (IMP-19).
  {
    const config::Config* cfg = wiring.config;
    session::SafeCheck inner;
    if (cfg != nullptr) {
      inner = [cfg]() -> Result<ports::Ok> {
        return session::require_valid_strategy_names(cfg->strategies.names);
      };
    }
    ctx.strategy_name_check =
        instrumented(SafeCheckId::StrategyNames, audit, std::move(inner),
                     missing_component(SafeCheckId::StrategyNames, "configuration"));
  }

  // 3. crypto-keys — the AES-256 token-store key via the real SecretProvider AND
  //    the pinned Ed25519 ledger public key on disk (SE-5).
  {
    const ports::SecretProvider* secret_source = wiring.secrets;
    std::string secret_name = wiring.token_key_secret;
    fs::path pinned = wiring.pinned_ledger_public_key;
    session::SafeCheck inner;
    if (secret_source != nullptr) {
      inner = [secret_source, secret_name = std::move(secret_name),
               pinned = std::move(pinned)]() -> Result<ports::Ok> {
        return require_crypto_keys(*secret_source, secret_name, pinned);
      };
    }
    ctx.crypto_keys_check = instrumented(SafeCheckId::CryptoKeys, audit, std::move(inner),
                                         missing_component(SafeCheckId::CryptoKeys,
                                                           "secret provider"));
  }

  // 4. clock — a REAL second sample through clock::SkewStallDetector. boot took
  //    the baseline at the top of the sequence, so this observation spans the
  //    config/data-dir/ledger/session work and can genuinely see a stall or an
  //    NTP step that happened during boot.
  {
    const ports::ClockPort* clock_port = wiring.clock;
    clock::SkewStallDetector* detector = wiring.clock_detector;
    session::SafeCheck inner;
    if (clock_port != nullptr && detector != nullptr) {
      inner = [clock_port, detector]() -> Result<ports::Ok> {
        const clock::ClockStatus status = detector->sample(*clock_port);
        if (status == clock::ClockStatus::Healthy) {
          return ports::ok();
        }
        // reason() is documented safe to log. DataStale, because audit stamps and
        // wall-clock deadlines are the untrustworthy thing here.
        return fail(blocking(ErrorCategory::DataStale,
                             std::string("boot: clock is ") +
                                 std::string(clock::to_string(status)) + " — " +
                                 detector->reason()));
      };
    }
    ctx.clock_check =
        instrumented(SafeCheckId::Clock, audit, std::move(inner),
                     missing_component(SafeCheckId::Clock, "clock or skew/stall detector"));
  }

  // 5. session — the composition-root wiring safe_start.hpp documents verbatim:
  //    PRESERVE a transport error, and map only the STATE. Collapsing a 5xx into
  //    SessionExpired would mislabel a retryable outage as a dead session (and,
  //    here, would turn a Crash-class exit into a fail-closed one).
  {
    SessionProbeFn session_probe = wiring.session_probe;
    session::SafeCheck inner;
    if (session_probe) {
      inner = [probe = std::move(session_probe)]() -> Result<ports::Ok> {
        auto state = probe();
        if (!state) {
          return fail(std::move(state).error());
        }
        return session::session_state_to_result(state.value());
      };
    }
    ctx.session_check = instrumented(SafeCheckId::Session, audit, std::move(inner),
                                     missing_component(SafeCheckId::Session, "session probe"));
  }

  // 6. egress-IP — see require_egress_ip_allowed for the honest scope note.
  {
    config::EnvLookup env_seam = wiring.env;
    session::SafeCheck inner;
    if (env_seam) {
      inner = [env_seam]() -> Result<ports::Ok> { return require_egress_ip_allowed(env_seam); };
    }
    ctx.egress_ip_check = instrumented(SafeCheckId::EgressIp, audit, std::move(inner),
                                       missing_component(SafeCheckId::EgressIp,
                                                         "environment seam"));
  }

  // 7. instrument-master — refdata::InstrumentMaster::require_fresh(), refreshing
  //    through its injected (shared-cache-backed) fetch seam when today's master
  //    is not loaded yet. require_fresh is the authority either way.
  {
    refdata::InstrumentMaster* master = wiring.instruments;
    session::SafeCheck inner;
    if (master != nullptr) {
      inner = [master]() -> Result<ports::Ok> {
        if (auto fresh = master->require_fresh(); fresh) {
          return fresh;
        }
        if (auto refreshed = master->refresh(); !refreshed) {
          return fail(std::move(refreshed).error());
        }
        return master->require_fresh();
      };
    }
    ctx.instrument_master_check =
        instrumented(SafeCheckId::InstrumentMaster, audit, std::move(inner),
                     missing_component(SafeCheckId::InstrumentMaster, "instrument master"));
  }

  // 8. calendar — the same shape over refdata::TradingCalendar.
  {
    refdata::TradingCalendar* trading_calendar = wiring.calendar;
    session::SafeCheck inner;
    if (trading_calendar != nullptr) {
      inner = [trading_calendar]() -> Result<ports::Ok> {
        if (auto fresh = trading_calendar->require_fresh(); fresh) {
          return fresh;
        }
        if (auto refreshed = trading_calendar->refresh(); !refreshed) {
          return fail(std::move(refreshed).error());
        }
        return trading_calendar->require_fresh();
      };
    }
    ctx.calendar_check = instrumented(SafeCheckId::Calendar, audit, std::move(inner),
                                      missing_component(SafeCheckId::Calendar, "trading calendar"));
  }

  // 9. legacy-stops — the wiring safe_start.hpp documents verbatim, over the real
  //    SQLite projection. A projection we cannot READ is a world we cannot verify,
  //    so the read error propagates rather than being treated as "no rows".
  {
    const store::Store* projection = wiring.store;
    session::SafeCheck inner;
    if (projection != nullptr) {
      inner = [projection]() -> Result<ports::Ok> {
        auto rows = projection->all_orders();
        if (!rows) {
          return fail(std::move(rows).error());
        }
        return session::require_no_legacy_stops(rows.value());
      };
    }
    ctx.legacy_stop_check = instrumented(SafeCheckId::LegacyStops, audit, std::move(inner),
                                         missing_component(SafeCheckId::LegacyStops,
                                                           "order projection"));
  }

  // 10. reconciliation — the cold-boot LOCAL-PROJECTION precondition. See
  //     require_no_unreconciled_orders for the scope note: the broker-truth fold
  //     is reconcile::RecoveryCoordinator's job on the main loop IMP-20 does not
  //     build, and this refuses to start on the state that would make it unsafe.
  {
    const store::Store* projection = wiring.store;
    session::SafeCheck inner;
    if (projection != nullptr) {
      inner = [projection]() -> Result<ports::Ok> {
        auto rows = projection->all_orders();
        if (!rows) {
          return fail(std::move(rows).error());
        }
        return require_no_unreconciled_orders(rows.value());
      };
    }
    ctx.reconciliation_check = instrumented(SafeCheckId::Reconciliation, audit, std::move(inner),
                                            missing_component(SafeCheckId::Reconciliation,
                                                              "order projection"));
  }

  return ctx;
}

}  // namespace broker_exec::boot
