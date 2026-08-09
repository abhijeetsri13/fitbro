#include "broker_exec/config/config.hpp"

#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

#include "broker_exec/domain/redaction.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::config {

using ::broker_exec::errors::Error;
using ::broker_exec::errors::ErrorCategory;
using ::broker_exec::errors::make_error;

namespace {

namespace fs = std::filesystem;

// ── small text helpers ───────────────────────────────────────────────────────

[[nodiscard]] char to_lower_ascii(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Case-insensitive ASCII substring test (denylist needles are lowercase, so we
// fold only the haystack — same idiom as errors::contains_ci).
[[nodiscard]] bool contains_ci(std::string_view haystack, std::string_view needle) noexcept {
  if (needle.empty()) {
    return true;
  }
  if (needle.size() > haystack.size()) {
    return false;
  }
  const std::size_t last = haystack.size() - needle.size();
  for (std::size_t i = 0; i <= last; ++i) {
    bool match = true;
    for (std::size_t j = 0; j < needle.size(); ++j) {
      if (to_lower_ascii(haystack[i + j]) != needle[j]) {
        match = false;
        break;
      }
    }
    if (match) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool equals_ci(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (to_lower_ascii(a[i]) != to_lower_ascii(b[i])) {
      return false;
    }
  }
  return true;
}

// ── typed errors that name the offending field ───────────────────────────────
//
// All config failures are Validation/DoNotRetry: a fail-closed config is a bad
// input the operator must fix, not something to retry blindly.

[[nodiscard]] Error field_error(std::string_view section, std::string_view field,
                                std::string_view reason) {
  std::string msg = "config: field '";
  msg += section;
  msg += '.';
  msg += field;
  msg += "' ";
  msg += reason;
  return make_error(ErrorCategory::Validation, std::move(msg));
}

[[nodiscard]] Error secret_error(std::string_view key_path) {
  std::string msg = "config: secret-shaped key '";
  msg += key_path;
  msg +=
      "' is forbidden in the TOML; provide secrets via the env seam / SecretProvider, never the "
      "config file";
  return make_error(ErrorCategory::Validation, std::move(msg));
}

// ── env key convention: BROKER_EXEC_<SECTION>_<FIELD> (uppercase) ─────────────

[[nodiscard]] std::string env_key(std::string_view section, std::string_view field) {
  std::string key = "BROKER_EXEC_";
  for (char c : section) {
    key += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  key += '_';
  for (char c : field) {
    key += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return key;
}

// ── secret denylist scan ─────────────────────────────────────────────────────

// Substring needles (lowercase). Deliberately omits ambiguous fragments like a
// bare "pin" that would also match innocuous keys ("mapping", "endpoint").
constexpr std::array<std::string_view, 12> kSecretNeedles = {
    "token",   "secret",  "password", "passwd",     "pwd",     "api_key",
    "api-key", "apikey",  "mpin",     "totp",       "bearer",  "credential"};

[[nodiscard]] std::optional<Error> matches_secret(std::string_view key, std::string_view path) {
  for (std::string_view needle : kSecretNeedles) {
    if (contains_ci(key, needle)) {
      return secret_error(path);
    }
  }
  return std::nullopt;
}

// Recursively reject any key (at any depth, including inside arrays-of-tables)
// whose name looks like a secret. Runs BEFORE any value is read so a secret in
// the file never reaches a loadable Config.
[[nodiscard]] std::optional<Error> scan_secrets(const toml::table& table, const std::string& prefix);

[[nodiscard]] std::optional<Error> scan_node(std::string_view path, const toml::node& node) {
  if (const toml::table* sub = node.as_table()) {
    return scan_secrets(*sub, std::string(path));
  }
  if (const toml::array* arr = node.as_array()) {
    for (const toml::node& element : *arr) {
      if (auto e = scan_node(path, element)) {
        return e;
      }
    }
  }
  return std::nullopt;
}

std::optional<Error> scan_secrets(const toml::table& table, const std::string& prefix) {
  for (const auto& [key, node] : table) {
    const std::string_view key_str = key.str();
    std::string path = prefix.empty() ? std::string(key_str) : prefix + "." + std::string(key_str);
    if (auto e = matches_secret(key_str, path)) {
      return e;
    }
    if (auto e = scan_node(path, node)) {
      return e;
    }
  }
  return std::nullopt;
}

// ── layered field application (file then env; env wins) ──────────────────────

[[nodiscard]] std::optional<Error> apply_string(const toml::table& table, const EnvLookup& env,
                                                 std::string_view section, std::string_view field,
                                                 std::string& out) {
  if (auto node = table[section][field]) {
    auto value = node.value<std::string>();
    if (!value) {
      return field_error(section, field, "must be a string");
    }
    out = std::move(*value);
  }
  if (auto value = env(env_key(section, field))) {
    out = std::move(*value);
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<Error> apply_int64(const toml::table& table, const EnvLookup& env,
                                               std::string_view section, std::string_view field,
                                               std::int64_t& out) {
  if (auto node = table[section][field]) {
    auto value = node.value<std::int64_t>();
    if (!value) {
      return field_error(section, field, "must be an integer");
    }
    out = *value;
  }
  if (auto value = env(env_key(section, field))) {
    const std::string& text = *value;
    std::int64_t parsed = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(first, last, parsed);
    if (ec != std::errc{} || ptr != last) {
      return field_error(section, field, "must be an integer");
    }
    out = parsed;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<Error> apply_path(const toml::table& table, const EnvLookup& env,
                                              std::string_view section, std::string_view field,
                                              fs::path& out) {
  std::string raw;
  bool present = false;
  if (auto node = table[section][field]) {
    auto value = node.value<std::string>();
    if (!value) {
      return field_error(section, field, "must be a string path");
    }
    raw = std::move(*value);
    present = true;
  }
  if (auto value = env(env_key(section, field))) {
    raw = std::move(*value);
    present = true;
  }
  if (present) {
    // A null byte is the one thing std::filesystem::path cannot represent
    // portably; everything else is resolved by the platform's path grammar.
    if (raw.find('\0') != std::string::npos) {
      return field_error(section, field, "is not a well-formed path");
    }
    out = fs::path(raw);
  }
  return std::nullopt;
}

// A list-valued field: a TOML array of strings, overridable WHOLESALE by a
// comma-separated env variable (env wins, exactly as for a scalar — it REPLACES
// the list rather than appending, so an operator can always see the effective list
// in one place). Surrounding ASCII spaces are trimmed off each entry; an env value
// that is empty or all-whitespace means "the empty list", which is a legal
// declaration and not a parse error.
[[nodiscard]] std::optional<Error> apply_string_list(const toml::table& table, const EnvLookup& env,
                                                     std::string_view section,
                                                     std::string_view field,
                                                     std::vector<std::string>& out) {
  if (auto node = table[section][field]) {
    const toml::array* arr = node.as_array();
    if (arr == nullptr) {
      return field_error(section, field, "must be an array of strings");
    }
    std::vector<std::string> parsed;
    parsed.reserve(arr->size());
    for (const toml::node& element : *arr) {
      auto value = element.value<std::string>();
      if (!value) {
        return field_error(section, field, "must be an array of strings");
      }
      parsed.push_back(std::move(*value));
    }
    out = std::move(parsed);
  }
  if (auto value = env(env_key(section, field))) {
    std::vector<std::string> parsed;
    const std::string& text = *value;
    std::size_t begin = 0;
    while (begin <= text.size()) {
      const std::size_t comma = text.find(',', begin);
      const std::size_t end = comma == std::string::npos ? text.size() : comma;
      std::size_t lo = begin;
      std::size_t hi = end;
      while (lo < hi && (text[lo] == ' ' || text[lo] == '\t')) {
        ++lo;
      }
      while (hi > lo && (text[hi - 1] == ' ' || text[hi - 1] == '\t')) {
        --hi;
      }
      // A single empty/blank variable declares an EMPTY list; an empty entry
      // BETWEEN commas is a typo and is kept so validation names it.
      if (!(comma == std::string::npos && parsed.empty() && lo == hi)) {
        parsed.emplace_back(text, lo, hi - lo);
      }
      if (comma == std::string::npos) {
        break;
      }
      begin = comma + 1;
    }
    out = std::move(parsed);
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<Error> apply_run_profile(const toml::table& table, const EnvLookup& env,
                                                     std::string_view section,
                                                     std::string_view field, RunProfile& out) {
  std::string raw;
  bool present = false;
  if (auto node = table[section][field]) {
    auto value = node.value<std::string>();
    if (!value) {
      return field_error(section, field, "must be a string");
    }
    raw = std::move(*value);
    present = true;
  }
  if (auto value = env(env_key(section, field))) {
    raw = std::move(*value);
    present = true;
  }
  if (present) {
    auto parsed = parse_run_profile(raw);
    if (!parsed) {
      return field_error(section, field,
                         "is not a valid mode (expected dry-run|paper|live|replay)");
    }
    out = *parsed;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<Error> apply_log_level(const toml::table& table, const EnvLookup& env,
                                                   std::string_view section, std::string_view field,
                                                   LogLevel& out) {
  std::string raw;
  bool present = false;
  if (auto node = table[section][field]) {
    auto value = node.value<std::string>();
    if (!value) {
      return field_error(section, field, "must be a string");
    }
    raw = std::move(*value);
    present = true;
  }
  if (auto value = env(env_key(section, field))) {
    raw = std::move(*value);
    present = true;
  }
  if (present) {
    auto parsed = parse_log_level(raw);
    if (!parsed) {
      return field_error(section, field,
                         "is not a valid level (expected trace|debug|info|warn|error)");
    }
    out = *parsed;
  }
  return std::nullopt;
}

// ── final cross-field validation ─────────────────────────────────────────────

[[nodiscard]] std::optional<Error> validate(const Config& cfg) {
  if (cfg.engine.account_id.empty()) {
    return field_error("engine", "account_id", "is required");
  }
  if (cfg.broker.name.empty()) {
    return field_error("broker", "name", "is required");
  }
  if (cfg.broker.timeout_ms <= 0) {
    return field_error("broker", "timeout_ms", "must be > 0");
  }
  if (cfg.risk.max_order_value_paise < 0) {
    return field_error("risk", "max_order_value_paise", "must be >= 0");
  }
  if (cfg.risk.max_open_positions < 0) {
    return field_error("risk", "max_open_positions", "must be >= 0");
  }
  if (cfg.paths.data_dir.empty()) {
    return field_error("paths", "data_dir", "is required");
  }
  // IMP-19: a strategy name is the FIRST SEGMENT of every client_ref it mints, so
  // an invalid one makes every alert and ledger entry about that strategy's orders
  // read `client_ref=***REDACTED***`. Rejecting it here means the operator fixes a
  // name before the process ever holds a session — the rule and the diagnostic both
  // come from domain, so config cannot drift from the redaction contract.
  for (std::size_t i = 0; i < cfg.strategies.names.size(); ++i) {
    const std::string& name = cfg.strategies.names[i];
    const std::string reason = domain::explain_invalid_strategy_name(name);
    if (!reason.empty()) {
      return field_error("strategies", "names",
                         "entry #" + std::to_string(i + 1) + " is invalid: " + reason);
    }
  }
  return std::nullopt;
}

}  // namespace

std::string_view to_string(RunProfile profile) noexcept {
  switch (profile) {
    case RunProfile::DryRun:
      return "dry-run";
    case RunProfile::Paper:
      return "paper";
    case RunProfile::Live:
      return "live";
    case RunProfile::Replay:
      return "replay";
  }
  return "dry-run";
}

std::string_view to_string(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Trace:
      return "trace";
    case LogLevel::Debug:
      return "debug";
    case LogLevel::Info:
      return "info";
    case LogLevel::Warn:
      return "warn";
    case LogLevel::Error:
      return "error";
  }
  return "info";
}

std::optional<RunProfile> parse_run_profile(std::string_view text) noexcept {
  // Accept both spellings of the hyphenated mode so file/env are forgiving.
  if (equals_ci(text, "dry-run") || equals_ci(text, "dryrun") || equals_ci(text, "dry_run")) {
    return RunProfile::DryRun;
  }
  if (equals_ci(text, "paper")) {
    return RunProfile::Paper;
  }
  if (equals_ci(text, "live")) {
    return RunProfile::Live;
  }
  if (equals_ci(text, "replay")) {
    return RunProfile::Replay;
  }
  return std::nullopt;
}

std::optional<LogLevel> parse_log_level(std::string_view text) noexcept {
  if (equals_ci(text, "trace")) {
    return LogLevel::Trace;
  }
  if (equals_ci(text, "debug")) {
    return LogLevel::Debug;
  }
  if (equals_ci(text, "info")) {
    return LogLevel::Info;
  }
  if (equals_ci(text, "warn") || equals_ci(text, "warning")) {
    return LogLevel::Warn;
  }
  if (equals_ci(text, "error")) {
    return LogLevel::Error;
  }
  return std::nullopt;
}

EnvLookup default_env_lookup() {
  // The single std::getenv call site in the module — the production seam. This
  // is portable C++ standard library (no OS API / no #ifdef); MSVC's *_s
  // deprecation is suppressed project-wide via _CRT_SECURE_NO_WARNINGS.
  return [](std::string_view key) -> std::optional<std::string> {
    const std::string key_str(key);
    const char* value = std::getenv(key_str.c_str());  // NOLINT(concurrency-mt-unsafe)
    if (value == nullptr) {
      return std::nullopt;
    }
    return std::string(value);
  };
}

Result<Config> load(const std::filesystem::path& toml_path, const EnvLookup& env) {
  Config cfg;  // layer 1: built-in defaults.

  toml::table table;  // stays empty when no file layer is used.
  if (!toml_path.empty()) {
    std::error_code ec;
    if (!fs::exists(toml_path, ec) || ec) {
      return fail(make_error(ErrorCategory::Validation,
                             "config: TOML file not found: " + toml_path.string()));
    }

    // layer 2: parse the TOML file. toml++ signals parse failure differently
    // depending on whether the build has C++ exceptions (TOML_EXCEPTIONS is a
    // library feature switch, not an OS macro). Handle both; never let a parse
    // failure escape as a throw across our boundary.
#if TOML_EXCEPTIONS
    try {
      table = toml::parse_file(toml_path.string());
    } catch (const toml::parse_error& e) {
      std::string msg = "config: TOML parse error: ";
      msg += e.description();
      return fail(make_error(ErrorCategory::Validation, std::move(msg)));
    }
#else
    toml::parse_result result = toml::parse_file(toml_path.string());
    if (!result) {
      std::string msg = "config: TOML parse error: ";
      msg += result.error().description();
      return fail(make_error(ErrorCategory::Validation, std::move(msg)));
    }
    table = std::move(result).table();
#endif

    // Reject secret-shaped keys before reading any value.
    if (auto e = scan_secrets(table, std::string{})) {
      return fail(std::move(*e));
    }
  }

  // layer 2 then layer 3 per field: file overrides defaults, env overrides file.
  if (auto e = apply_run_profile(table, env, "engine", "mode", cfg.engine.mode)) {
    return fail(std::move(*e));
  }
  if (auto e = apply_string(table, env, "engine", "account_id", cfg.engine.account_id)) {
    return fail(std::move(*e));
  }
  if (auto e = apply_string(table, env, "broker", "name", cfg.broker.name)) {
    return fail(std::move(*e));
  }
  if (auto e = apply_string(table, env, "broker", "base_url", cfg.broker.base_url)) {
    return fail(std::move(*e));
  }
  if (auto e = apply_int64(table, env, "broker", "timeout_ms", cfg.broker.timeout_ms)) {
    return fail(std::move(*e));
  }
  if (auto e = apply_int64(table, env, "risk", "max_order_value_paise",
                           cfg.risk.max_order_value_paise)) {
    return fail(std::move(*e));
  }
  if (auto e = apply_int64(table, env, "risk", "max_open_positions", cfg.risk.max_open_positions)) {
    return fail(std::move(*e));
  }
  if (auto e = apply_path(table, env, "paths", "data_dir", cfg.paths.data_dir)) {
    return fail(std::move(*e));
  }
  if (auto e = apply_path(table, env, "paths", "log_dir", cfg.paths.log_dir)) {
    return fail(std::move(*e));
  }
  if (auto e = apply_log_level(table, env, "logging", "level", cfg.logging.level)) {
    return fail(std::move(*e));
  }
  if (auto e = apply_string_list(table, env, "strategies", "names", cfg.strategies.names)) {
    return fail(std::move(*e));
  }

  if (auto e = validate(cfg)) {
    return fail(std::move(*e));
  }
  return cfg;
}

Result<Config> load(const std::filesystem::path& toml_path) {
  return load(toml_path, default_env_lookup());
}

}  // namespace broker_exec::config
