#pragma once

// broker_exec::config — typed, validated, layered configuration (Story 2.1,
// FR-34, DA-5).
//
// One binary runs dev/paper/live by configuration ONLY. Behaviour is driven by
// a typed, immutable `Config` value assembled from three layers, lowest to
// highest precedence:
//
//     built-in defaults  ->  TOML file (toml++)  ->  environment overrides
//                                                    (env WINS)
//
// SECRETS NEVER LIVE IN THE TOML. The loader rejects any TOML key whose name
// matches the secret denylist (token/secret/password/api_key/mpin/totp,
// case-insensitive). Credentials are sourced exclusively through the injected
// env seam / a future `ports::SecretProvider`.
//
// ENV OVERRIDE CONVENTION: each field maps to `BROKER_EXEC_<SECTION>_<FIELD>`
// (uppercase), e.g. `engine.mode` -> `BROKER_EXEC_ENGINE_MODE`,
// `risk.max_open_positions` -> `BROKER_EXEC_RISK_MAX_OPEN_POSITIONS`.
//
// NO-THROW POLICY: `load()` returns `Result<Config>` (expected<Config, Error>);
// every failure — missing file, parse error, bad enum, out-of-range number,
// missing required field, secret-shaped key — is a typed `errors::Error` that
// NAMES the offending field. Nothing throws across this boundary.
//
// Cross-platform: C++20 standard library only. Paths are `std::filesystem`; the
// only environment access is through the injected seam. No OS APIs, no `#ifdef`.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "broker_exec/result.hpp"

namespace broker_exec::config {

// The trading run mode / profile selector. There is no pre-existing mode enum in
// `domain/enums.hpp` (which holds Side/OrderType/Product/OrderState only), so
// this local enum owns the live/paper/dry-run/replay vocabulary. The same binary
// switches profile by config alone (AC-3). DryRun is the safe default: it never
// touches a live broker.
enum class RunProfile { DryRun, Paper, Live, Replay };

// Log verbosity. Stored as a stable lowercase string in the TOML/env surface.
enum class LogLevel { Trace, Debug, Info, Warn, Error };

// Stable, log/serialization-friendly names (observability contract; renames are
// breaking). These are also the accepted TOML/env spellings (case-insensitive on
// parse — see parse_*).
[[nodiscard]] std::string_view to_string(RunProfile profile) noexcept;
[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;

// Parse an enum from its textual spelling (case-insensitive). nullopt on an
// unknown token — the loader turns that into a field-named Error.
[[nodiscard]] std::optional<RunProfile> parse_run_profile(std::string_view text) noexcept;
[[nodiscard]] std::optional<LogLevel> parse_log_level(std::string_view text) noexcept;

// ── Sections ───────────────────────────────────────────────────────────────
// Plain value structs (mirrors the `errors::Error` style). Treat every field as
// immutable after load: `Config` is produced once by `load()` and read via const
// access thereafter. Only the fields needed now are present; new fields are
// purely additive.

struct EngineConfig {
  RunProfile mode = RunProfile::DryRun;  // env: BROKER_EXEC_ENGINE_MODE
  std::string account_id;                // env: BROKER_EXEC_ENGINE_ACCOUNT_ID (required)
};

struct BrokerConfig {
  std::string name;                // env: BROKER_EXEC_BROKER_NAME (required, e.g. "kite")
  std::string base_url;            // env: BROKER_EXEC_BROKER_BASE_URL (optional)
  std::int64_t timeout_ms = 5000;  // env: BROKER_EXEC_BROKER_TIMEOUT_MS (> 0)
};

struct RiskConfig {
  // Money path: integer paise only, never double/float (binding convention).
  std::int64_t max_order_value_paise = 0;  // env: BROKER_EXEC_RISK_MAX_ORDER_VALUE_PAISE (>= 0)
  std::int64_t max_open_positions = 0;     // env: BROKER_EXEC_RISK_MAX_OPEN_POSITIONS (>= 0)
};

struct PathsConfig {
  std::filesystem::path data_dir;  // env: BROKER_EXEC_PATHS_DATA_DIR (required)
  std::filesystem::path log_dir;   // env: BROKER_EXEC_PATHS_LOG_DIR (optional)
};

struct LoggingConfig {
  LogLevel level = LogLevel::Info;  // env: BROKER_EXEC_LOGGING_LEVEL
};

// The immutable, validated top-level configuration. Composed of nested sections
// so the surface stays organized and extensible.
struct Config {
  EngineConfig engine;
  BrokerConfig broker;
  RiskConfig risk;
  PathsConfig paths;
  LoggingConfig logging;
};

// The environment seam: maps an env key to its value, or nullopt if unset. The
// loader takes this by value so tests inject a deterministic map and never read
// the real process environment. `default_env_lookup()` is the production seam.
using EnvLookup = std::function<std::optional<std::string>(std::string_view)>;

// The production environment seam (reads the real process environment through
// the C++ standard library behind this single function — the only `std::getenv`
// call site in the module).
[[nodiscard]] EnvLookup default_env_lookup();

// Load and validate configuration from `toml_path` layered with `env` overrides.
//
//   - `toml_path` empty  -> skip the file layer (defaults + env only).
//   - file missing       -> Error (the path is named).
//   - any secret-shaped TOML key -> Error (the key is named), before any value
//                                    is applied.
//   - bad enum / wrong type / out-of-range / missing required field -> Error
//     naming the field.
//
// Returns the fully-resolved `Config` on success. Never throws.
[[nodiscard]] Result<Config> load(const std::filesystem::path& toml_path, const EnvLookup& env);

// Convenience overload using the production environment seam.
[[nodiscard]] Result<Config> load(const std::filesystem::path& toml_path);

}  // namespace broker_exec::config
