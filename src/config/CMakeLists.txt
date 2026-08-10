# broker_exec::config — typed, validated, layered configuration (Story 2.1,
# FR-34, DA-5). Built-in defaults -> TOML file (toml++) -> env overrides (env
# wins), parsed into an immutable typed Config and returned as Result<Config>.
# Secrets are rejected from the TOML and sourced only via the env seam /
# SecretProvider. Depends inward only (domain + errors); no adapters/ports.

add_library(broker_exec_config STATIC
  config.cpp
)
add_library(broker_exec::config ALIAS broker_exec_config)

target_include_directories(broker_exec_config
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_config PUBLIC cxx_std_20)

# Error/Result appear in the public API (load() returns Result<Config>), so
# broker_exec_errors is PUBLIC. toml++ is an implementation detail of the loader
# and the warnings/sanitizers policy stays PRIVATE per convention. domain is
# PRIVATE: the run-profile/log-level enums are local to this module, and the only
# domain symbol used is domain::explain_invalid_strategy_name (IMP-19), called
# inside validate() — no domain type appears in config's public header.
target_link_libraries(broker_exec_config
  PUBLIC
    broker_exec_errors
  PRIVATE
    broker_exec_domain
    tomlplusplus::tomlplusplus
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_config_tests config_test.cpp)
  target_link_libraries(broker_exec_config_tests
    PRIVATE
      broker_exec_config
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_config_tests COMMAND broker_exec_config_tests)
endif()
