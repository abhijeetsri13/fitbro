#include "broker_exec/config/config.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "broker_exec/errors/error.hpp"

namespace fs = std::filesystem;

using broker_exec::config::Config;
using broker_exec::config::EnvLookup;
using broker_exec::config::load;
using broker_exec::config::LogLevel;
using broker_exec::config::RunProfile;
using broker_exec::errors::ErrorCategory;

namespace {

// A unique temp TOML per test, removed on scope exit so runs never collide and
// no artifacts are left behind (mirrors the intentlog TempLog fixture).
struct TempToml {
  fs::path path;

  TempToml(const std::string& tag, std::string_view contents)
      : path(fs::temp_directory_path() /
             ("broker_exec_config_" + tag + "_" +
              std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".toml")) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
  }
  ~TempToml() {
    std::error_code ec;
    fs::remove(path, ec);
  }
  TempToml(const TempToml&) = delete;
  TempToml& operator=(const TempToml&) = delete;
};

// A deterministic, in-test environment seam — the ONLY source of "env" in these
// tests. The real process environment is never read.
EnvLookup env_from(std::map<std::string, std::string> vars) {
  return [vars = std::move(vars)](std::string_view key) -> std::optional<std::string> {
    auto it = vars.find(std::string(key));
    if (it == vars.end()) {
      return std::nullopt;
    }
    return it->second;
  };
}

// A complete, valid file surface used as a baseline by several tests.
constexpr std::string_view kValidToml = R"toml(
[engine]
mode = "paper"
account_id = "acct-file"

[broker]
name = "kite"
base_url = "https://api.kite.example"
timeout_ms = 4000

[risk]
max_order_value_paise = 5000000
max_open_positions = 10

[paths]
data_dir = "/var/lib/broker_exec"

[logging]
level = "info"
)toml";

}  // namespace

TEST_CASE("valid TOML loads into a typed Config and env overrides win over the file", "[config]") {
  const TempToml file("valid_env_wins", kValidToml);

  const EnvLookup env = env_from({
      {"BROKER_EXEC_ENGINE_MODE", "live"},
      {"BROKER_EXEC_BROKER_TIMEOUT_MS", "1500"},
      {"BROKER_EXEC_RISK_MAX_OPEN_POSITIONS", "3"},
  });

  const auto result = load(file.path, env);
  REQUIRE(result.has_value());
  const Config& cfg = result.value();

  // File-only fields resolve from the TOML.
  CHECK(cfg.engine.account_id == "acct-file");
  CHECK(cfg.broker.name == "kite");
  CHECK(cfg.broker.base_url == "https://api.kite.example");
  CHECK(cfg.risk.max_order_value_paise == 5000000);
  CHECK(cfg.paths.data_dir == fs::path("/var/lib/broker_exec"));
  CHECK(cfg.logging.level == LogLevel::Info);

  // Overridden fields take the env value, not the file value (env WINS).
  CHECK(cfg.engine.mode == RunProfile::Live);  // file said "paper"
  CHECK(cfg.broker.timeout_ms == 1500);        // file said 4000
  CHECK(cfg.risk.max_open_positions == 3);     // file said 10
}

TEST_CASE("an invalid enum value fails fast with an Error naming the field", "[config]") {
  const TempToml file("bad_enum", R"toml(
[engine]
mode = "uber-live"
account_id = "acct"
[broker]
name = "kite"
[paths]
data_dir = "/data"
)toml");

  const auto result = load(file.path, env_from({}));
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::Validation);
  CHECK(result.error().message.find("engine.mode") != std::string::npos);
}

TEST_CASE("an out-of-range numeric value fails fast naming the field", "[config]") {
  const TempToml file("bad_range", R"toml(
[engine]
account_id = "acct"
[broker]
name = "kite"
timeout_ms = 0
[paths]
data_dir = "/data"
)toml");

  const auto result = load(file.path, env_from({}));
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::Validation);
  CHECK(result.error().message.find("broker.timeout_ms") != std::string::npos);
}

TEST_CASE("a missing required field fails fast naming the field", "[config]") {
  // broker.name is absent -> required-field failure.
  const TempToml file("missing_required", R"toml(
[engine]
account_id = "acct"
[paths]
data_dir = "/data"
)toml");

  const auto result = load(file.path, env_from({}));
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::Validation);
  CHECK(result.error().message.find("broker.name") != std::string::npos);
}

TEST_CASE("a non-integer env override fails fast naming the field", "[config]") {
  const TempToml file("bad_env_int", kValidToml);
  const auto result =
      load(file.path, env_from({{"BROKER_EXEC_BROKER_TIMEOUT_MS", "not-a-number"}}));
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::Validation);
  CHECK(result.error().message.find("broker.timeout_ms") != std::string::npos);
}

TEST_CASE("a secret-shaped key in the TOML is rejected at load", "[config]") {
  SECTION("api_key under a section") {
    const TempToml file("secret_api_key", R"toml(
[engine]
account_id = "acct"
[broker]
name = "kite"
api_key = "should-not-be-here"
[paths]
data_dir = "/data"
)toml");
    const auto result = load(file.path, env_from({}));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().category == ErrorCategory::Validation);
    CHECK(result.error().message.find("api_key") != std::string::npos);
  }

  SECTION("a TOTP key, case-insensitively, anywhere in the tree") {
    const TempToml file("secret_totp", R"toml(
[engine]
account_id = "acct"
[broker]
name = "kite"
[auth]
TOTP_secret = "ABCDEF"
[paths]
data_dir = "/data"
)toml");
    const auto result = load(file.path, env_from({}));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().category == ErrorCategory::Validation);
    // The reported message names the offending key (it is itself non-secret).
    CHECK(result.error().message.find("TOTP_secret") != std::string::npos);
  }

  // The recursion must catch a secret regardless of how TOML nests it. toml++
  // normalizes inline tables and dotted keys into sub-tables, so these exercise
  // scan_node's table/array descent on every shape a real config could use.
  SECTION("inside an inline table") {
    const TempToml file("secret_inline", R"toml(
[engine]
account_id = "acct"
[broker]
name = "kite"
creds = { access_token = "x" }
[paths]
data_dir = "/data"
)toml");
    const auto result = load(file.path, env_from({}));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("access_token") != std::string::npos);
  }

  SECTION("a dotted key") {
    const TempToml file("secret_dotted", R"toml(
[engine]
account_id = "acct"
[broker]
name = "kite"
auth.bearer_token = "x"
[paths]
data_dir = "/data"
)toml");
    const auto result = load(file.path, env_from({}));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("bearer_token") != std::string::npos);
  }

  SECTION("inside an array of tables") {
    const TempToml file("secret_aot", R"toml(
[engine]
account_id = "acct"
[broker]
name = "kite"
[[broker.sessions]]
label = "morning"
[[broker.sessions]]
api_secret = "x"
[paths]
data_dir = "/data"
)toml");
    const auto result = load(file.path, env_from({}));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("api_secret") != std::string::npos);
  }
}

TEST_CASE("env overrides win over the file for every field (AC-3, all fields)", "[config]") {
  const TempToml file("env_wins_all", kValidToml);
  const auto result = load(file.path, env_from({
                                          {"BROKER_EXEC_ENGINE_MODE", "live"},
                                          {"BROKER_EXEC_ENGINE_ACCOUNT_ID", "acct-env"},
                                          {"BROKER_EXEC_BROKER_NAME", "kotak_neo"},
                                          {"BROKER_EXEC_BROKER_BASE_URL", "https://env.example"},
                                          {"BROKER_EXEC_BROKER_TIMEOUT_MS", "999"},
                                          {"BROKER_EXEC_RISK_MAX_ORDER_VALUE_PAISE", "7"},
                                          {"BROKER_EXEC_RISK_MAX_OPEN_POSITIONS", "42"},
                                          {"BROKER_EXEC_PATHS_DATA_DIR", "/env/data"},
                                          {"BROKER_EXEC_PATHS_LOG_DIR", "/env/log"},
                                          {"BROKER_EXEC_LOGGING_LEVEL", "debug"},
                                      }));
  REQUIRE(result.has_value());
  const Config& cfg = result.value();
  CHECK(cfg.engine.mode == RunProfile::Live);
  CHECK(cfg.engine.account_id == "acct-env");
  CHECK(cfg.broker.name == "kotak_neo");
  CHECK(cfg.broker.base_url == "https://env.example");
  CHECK(cfg.broker.timeout_ms == 999);
  CHECK(cfg.risk.max_order_value_paise == 7);
  CHECK(cfg.risk.max_open_positions == 42);
  CHECK(cfg.paths.data_dir == fs::path("/env/data"));
  CHECK(cfg.paths.log_dir == fs::path("/env/log"));
  CHECK(cfg.logging.level == LogLevel::Debug);
}

TEST_CASE(
    "one binary, config-only: the same TOML + two env profile sets yield two distinct Configs",
    "[config]") {
  const TempToml file("one_binary", kValidToml);

  // Paper profile: small book, conservative.
  const auto paper = load(file.path, env_from({
                                         {"BROKER_EXEC_ENGINE_MODE", "paper"},
                                         {"BROKER_EXEC_ENGINE_ACCOUNT_ID", "acct-paper"},
                                         {"BROKER_EXEC_RISK_MAX_OPEN_POSITIONS", "2"},
                                     }));
  // Live profile: same source/file, different env only.
  const auto live = load(file.path, env_from({
                                        {"BROKER_EXEC_ENGINE_MODE", "live"},
                                        {"BROKER_EXEC_ENGINE_ACCOUNT_ID", "acct-live"},
                                        {"BROKER_EXEC_RISK_MAX_OPEN_POSITIONS", "25"},
                                    }));

  REQUIRE(paper.has_value());
  REQUIRE(live.has_value());

  CHECK(paper.value().engine.mode == RunProfile::Paper);
  CHECK(live.value().engine.mode == RunProfile::Live);
  CHECK(paper.value().engine.account_id == "acct-paper");
  CHECK(live.value().engine.account_id == "acct-live");
  CHECK(paper.value().risk.max_open_positions == 2);
  CHECK(live.value().risk.max_open_positions == 25);

  // Two distinct, independently valid configurations from one source.
  CHECK(paper.value().engine.mode != live.value().engine.mode);
}

TEST_CASE("defaults + env only (no file) load when required fields come from env", "[config]") {
  const auto result = load(fs::path{}, env_from({
                                           {"BROKER_EXEC_ENGINE_ACCOUNT_ID", "acct-env"},
                                           {"BROKER_EXEC_BROKER_NAME", "kotak_neo"},
                                           {"BROKER_EXEC_PATHS_DATA_DIR", "/srv/data"},
                                       }));
  REQUIRE(result.has_value());
  const Config& cfg = result.value();
  CHECK(cfg.engine.mode == RunProfile::DryRun);  // safe built-in default
  CHECK(cfg.broker.name == "kotak_neo");
  CHECK(cfg.broker.timeout_ms == 5000);  // built-in default
  CHECK(cfg.paths.data_dir == fs::path("/srv/data"));
}

TEST_CASE("a missing TOML file is reported as an error naming the path", "[config]") {
  const fs::path missing = fs::temp_directory_path() / "broker_exec_config_does_not_exist_zzz.toml";
  std::error_code ec;
  fs::remove(missing, ec);

  const auto result = load(missing, env_from({}));
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::Validation);
  CHECK(result.error().message.find("not found") != std::string::npos);
}

// ── IMP-19: strategy names are configuration, and they are VALIDATED ──────────
//
// A strategy name is the FIRST SEGMENT of every client_ref it mints, so an
// ill-shaped one makes every alert and every ledger entry about that strategy's
// orders read `client_ref=***REDACTED***`. The rule and the diagnostic both come
// from domain::is_valid_strategy_name / explain_invalid_strategy_name, so config
// cannot drift from the redaction contract it is enforcing.

TEST_CASE("strategy names load from the TOML and from a comma-separated env override",
          "[config][IMP-19]") {
  const TempToml file("strategies", R"toml(
[engine]
account_id = "acct"
[broker]
name = "kite"
[paths]
data_dir = "/data"
[strategies]
names = ["alpha", "S-1", "atm-straddle-9-20"]
)toml");

  // The one env key under test, spelled once.
  const auto names_env = [](std::string value) {
    return env_from({{"BROKER_EXEC_STRATEGIES_NAMES", std::move(value)}});
  };

  const auto from_file = load(file.path, env_from({}));
  REQUIRE(from_file.has_value());
  REQUIRE(from_file.value().strategies.names.size() == 3);
  CHECK(from_file.value().strategies.names[0] == "alpha");
  CHECK(from_file.value().strategies.names[2] == "atm-straddle-9-20");

  // env WINS and REPLACES the whole list; surrounding spaces are trimmed.
  const auto from_env = load(file.path, names_env("beta, momentum-v-2"));
  REQUIRE(from_env.has_value());
  REQUIRE(from_env.value().strategies.names.size() == 2);
  CHECK(from_env.value().strategies.names[0] == "beta");
  CHECK(from_env.value().strategies.names[1] == "momentum-v-2");

  // An empty variable declares the EMPTY list, not a one-element list of "".
  const auto cleared = load(file.path, names_env(""));
  REQUIRE(cleared.has_value());
  CHECK(cleared.value().strategies.names.empty());

  // Absent everywhere -> empty list, and the config still loads.
  const TempToml bare("no_strategies", R"toml(
[engine]
account_id = "acct"
[broker]
name = "kite"
[paths]
data_dir = "/data"
)toml");
  const auto none = load(bare.path, env_from({}));
  REQUIRE(none.has_value());
  CHECK(none.value().strategies.names.empty());
}

TEST_CASE("an ill-shaped strategy name fails the load, naming the entry and the fix",
          "[config][IMP-19]") {
  const TempToml file("bad_strategy", R"toml(
[engine]
account_id = "acct"
[broker]
name = "kite"
[paths]
data_dir = "/data"
[strategies]
names = ["alpha", "S1"]
)toml");

  const auto names_env = [](std::string value) {
    return env_from({{"BROKER_EXEC_STRATEGIES_NAMES", std::move(value)}});
  };

  const auto result = load(file.path, env_from({}));
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::Validation);
  CHECK(result.error().message.find("strategies.names") != std::string::npos);
  CHECK(result.error().message.find("entry #2") != std::string::npos);
  CHECK(result.error().message.find("segment 'S1'") != std::string::npos);
  CHECK(result.error().message.find("use 'S-1'") != std::string::npos);

  // The same name spelled the way the message suggests loads cleanly.
  REQUIRE(load(file.path, names_env("alpha,S-1")).has_value());

  // A name with a space ("iron condor v2") is refused too — and the offending byte
  // is never echoed raw into the Error message.
  const auto spaced = load(file.path, names_env("iron condor v2"));
  REQUIRE_FALSE(spaced.has_value());
  CHECK(spaced.error().message.find("iron?condor?v2") != std::string::npos);
  CHECK(spaced.error().message.find("iron condor v2") == std::string::npos);

  // A name of nothing but separators is refused for the same reason "" is: it
  // renders a `strategy=` column that attributes an order to nothing at all. It is
  // otherwise perfectly legal — it mints an id-shaped ref and survives its own
  // column — so the rule, not the shape, is what stops it.
  const auto separators = load(file.path, names_env("alpha,---"));
  REQUIRE_FALSE(separators.has_value());
  CHECK(separators.error().message.find("entry #2") != std::string::npos);
  CHECK(separators.error().message.find("no letter or digit") != std::string::npos);

  // A TRAILING COMMA in the env variable is NOT ignored: it yields a second, EMPTY
  // entry, and an empty name fails the load. Documented in config/example.toml
  // (the env-override semantics) and pinned here so the two cannot drift.
  const auto trailing = load(file.path, names_env("alpha,"));
  REQUIRE_FALSE(trailing.has_value());
  CHECK(trailing.error().message.find("entry #2") != std::string::npos);
  CHECK(trailing.error().message.find("EMPTY") != std::string::npos);
}

TEST_CASE("a non-array / non-string strategies.names is a typed error naming the field",
          "[config][IMP-19]") {
  const TempToml scalar("strategies_scalar", R"toml(
[engine]
account_id = "acct"
[broker]
name = "kite"
[paths]
data_dir = "/data"
[strategies]
names = "alpha"
)toml");
  const auto r1 = load(scalar.path, env_from({}));
  REQUIRE_FALSE(r1.has_value());
  CHECK(r1.error().message.find("strategies.names") != std::string::npos);
  CHECK(r1.error().message.find("array of strings") != std::string::npos);

  const TempToml mixed("strategies_mixed", R"toml(
[engine]
account_id = "acct"
[broker]
name = "kite"
[paths]
data_dir = "/data"
[strategies]
names = ["alpha", 7]
)toml");
  const auto r2 = load(mixed.path, env_from({}));
  REQUIRE_FALSE(r2.has_value());
  CHECK(r2.error().message.find("array of strings") != std::string::npos);
}

// ── IMP-34: an integer field accepts ONLY a TOML integer ─────────────────────
//
// toml++'s node::value<T>() is documented PERMISSIVE: a boolean node hands back
// 0/1 and a whole-valued float hands back its truncation, with no diagnostic. The
// loader used it while promising "must be an integer", so EVERY case below LOADED
// CLEANLY before the fix — each converted value landed inside validate()'s legal
// range, which is precisely why nothing downstream could notice. value_exact<T>()
// makes the file layer refuse exactly what the env layer's std::from_chars always
// refused.

TEST_CASE("a boolean in an integer field is refused, not read as 0/1", "[config][IMP-34]") {
  // The 1 ms timeout is the whole point: `true` -> 1 passes `timeout_ms <= 0`, and
  // a 1 ms broker timeout expires before any response arrives, so every order
  // placement resolves to UNKNOWN — the ambiguity this library exists to prevent —
  // out of a file the loader reported as valid.
  const TempToml file("int_bool_timeout", R"toml(
[engine]
account_id = "acct"
[broker]
name = "kite"
timeout_ms = true
[paths]
data_dir = "/data"
)toml");

  const auto result = load(file.path, env_from({}));
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::Validation);
  CHECK(result.error().message.find("broker.timeout_ms") != std::string::npos);
  CHECK(result.error().message.find("must be an integer") != std::string::npos);
  // The message names the type actually written, so the operator is told WHAT they
  // wrote rather than only what was expected.
  CHECK(result.error().message.find("boolean") != std::string::npos);

  // The sibling integer field is exactly as strict: `false` -> 0 also cleared
  // validate()'s `< 0` test before the fix, so it loaded a silently invented limit.
  const TempToml positions("int_bool_positions", R"toml(
[engine]
account_id = "acct"
[broker]
name = "kite"
[risk]
max_open_positions = false
[paths]
data_dir = "/data"
)toml");

  const auto r2 = load(positions.path, env_from({}));
  REQUIRE_FALSE(r2.has_value());
  CHECK(r2.error().message.find("risk.max_open_positions") != std::string::npos);
  CHECK(r2.error().message.find("boolean") != std::string::npos);
}

TEST_CASE("a float in the paise field is refused — no double reaches the money path",
          "[config][IMP-34]") {
  // 2.5e5 was accepted as 250000: a floating-point literal converted into an
  // int64 paise field, against the binding "no double/float in a money or price
  // path" rule, with nothing in the loaded Config recording that it happened.
  const TempToml file("int_float_paise", R"toml(
[engine]
account_id = "acct"
[broker]
name = "kite"
[risk]
max_order_value_paise = 2.5e5
[paths]
data_dir = "/data"
)toml");

  const auto result = load(file.path, env_from({}));
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::Validation);
  CHECK(result.error().message.find("risk.max_order_value_paise") != std::string::npos);
  CHECK(result.error().message.find("must be an integer") != std::string::npos);
  // "must be an integer" alone is a riddle for a value that looks numeric; the
  // message has to say which character made it a float.
  CHECK(result.error().message.find("floating-point") != std::string::npos);
}

TEST_CASE("the file and env layers agree on what an integer is", "[config][IMP-34]") {
  // The two layers disagreed: std::from_chars leaves ".0" unconsumed so the env
  // branch always rejected "4000.0", while the file branch read the same text as
  // 4000. A field's type must not depend on which layer supplied it.
  const TempToml from_file_toml("int_float_timeout", R"toml(
[engine]
account_id = "acct"
[broker]
name = "kite"
timeout_ms = 4000.0
[paths]
data_dir = "/data"
)toml");

  const auto from_file = load(from_file_toml.path, env_from({}));
  REQUIRE_FALSE(from_file.has_value());
  CHECK(from_file.error().message.find("broker.timeout_ms") != std::string::npos);
  CHECK(from_file.error().message.find("must be an integer") != std::string::npos);

  const TempToml baseline("int_float_env_twin", kValidToml);
  const auto from_env =
      load(baseline.path, env_from({{"BROKER_EXEC_BROKER_TIMEOUT_MS", "4000.0"}}));
  REQUIRE_FALSE(from_env.has_value());
  CHECK(from_env.error().message.find("broker.timeout_ms") != std::string::npos);
  CHECK(from_env.error().message.find("must be an integer") != std::string::npos);
}
