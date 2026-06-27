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
  CHECK(cfg.engine.mode == RunProfile::Live);          // file said "paper"
  CHECK(cfg.broker.timeout_ms == 1500);                // file said 4000
  CHECK(cfg.risk.max_open_positions == 3);             // file said 10
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

TEST_CASE("one binary, config-only: the same TOML + two env profile sets yield two distinct Configs",
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
  const fs::path missing =
      fs::temp_directory_path() / "broker_exec_config_does_not_exist_zzz.toml";
  std::error_code ec;
  fs::remove(missing, ec);

  const auto result = load(missing, env_from({}));
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::Validation);
  CHECK(result.error().message.find("not found") != std::string::npos);
}
