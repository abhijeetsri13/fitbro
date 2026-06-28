cmake_minimum_required(VERSION 3.28)

project(broker_exec
  VERSION 0.1.0
  DESCRIPTION "Broker-neutral trading execution library"
  LANGUAGES CXX C)   # C for the vendored SQLite amalgamation

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for clang-tidy / editors.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Single-config generators: default to Release if unset.
get_property(_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _is_multi_config AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(CompilerWarnings)   # broker_exec_warnings  (INTERFACE)
include(Sanitizers)         # broker_exec_sanitizers (INTERFACE)
include(HexagonalBoundary)  # enforce_no_dependency()

option(BROKER_EXEC_BUILD_TESTS "Build the test suite" ON)

# enable_testing() must run in the top-level dir BEFORE any add_subdirectory()
# that registers module-local tests, or those add_test() calls are silently
# dropped.
if(BROKER_EXEC_BUILD_TESTS)
  enable_testing()
endif()

# ── Dependencies (resolved by Conan-generated find_package config files) ──
find_package(Catch2 3 REQUIRED)
find_package(nlohmann_json REQUIRED)  # intent-log JSON lines (Story 1.5)
find_package(tomlplusplus CONFIG REQUIRED)  # typed configuration (Story 2.1)
find_package(OpenSSL REQUIRED)  # AES-256-GCM token encryption at rest (Story 2.2)
find_package(cpr REQUIRED)  # Kite Connect REST transport over libcurl (Story 2.3)
find_package(spdlog REQUIRED)  # structured JSON logging + redaction (Story 4.1)
find_package(CLI11 REQUIRED)  # operator CLI subcommands (Story 4.6)
find_package(httplib REQUIRED)  # localhost health endpoint transport (Story 4.6)

# Vendored SQLite (static, hermetic) — provides target SQLite::SQLite3.
add_subdirectory(third_party/sqlite3)

# ── Layered targets (hexagonal architecture) ──
# Inner → outer. The `domain` core is pure; `ports` are pure abstractions;
# `adapters` are the only place broker SDKs / transports may be imported.
#
# ORCHESTRATOR-OWNED INTEGRATION POINT: later stories append their module
# subdirectories below (one add_subdirectory per module target). Module
# CMakeLists files are self-contained so parallel work does not collide here.
add_subdirectory(src/platform)   # cross-platform OS seam (fsync/_commit, perms, ...)
add_subdirectory(src/domain)
add_subdirectory(src/errors)
add_subdirectory(src/ports)
add_subdirectory(src/clock)      # SystemClock/TestClock + skew/stall detector (impl of ClockPort)
add_subdirectory(src/adapters)
# --- Wave 2 ---
add_subdirectory(src/intentlog)        # fsync'd hash-chained write-ahead intent log (1.5)
add_subdirectory(src/store)            # SQLite WAL projection + numbered migrations (1.6)
add_subdirectory(src/idempotency)      # client-ref gen + idempotency index (1.7)
add_subdirectory(src/lifecycle)        # order lifecycle FSM + parent/child fold (1.8)
add_subdirectory(src/adapters/fake)    # adversarial fake broker / fault injection (1.11)
add_subdirectory(src/runtime)          # dispatch() send chokepoint, no-blind-retry (1.9)
# --- Epic 2 ---
add_subdirectory(src/config)           # typed/validated layered config: defaults->TOML->env (2.1)
add_subdirectory(src/secrets)          # env SecretProvider + AES-256-GCM token store (2.2)
add_subdirectory(src/adapters/kite)    # Kite Connect REST client (transport) (2.3)
add_subdirectory(src/session)          # Kite daily session establishment + expiry detection (2.4)
add_subdirectory(src/capabilities)     # per-broker capability model + early rejection (2.5)
add_subdirectory(src/refdata)          # instrument-master lifecycle + staleness gate (2.6)
add_subdirectory(src/risk)             # pre-submission validation gate (2.8) + risk engine (2.10)
add_subdirectory(src/slicing)          # freeze-quantity slicer: deterministic <parent>#<k> children (2.9)
add_subdirectory(src/ratelimit)        # token bucket + reserved exit lane (2.12)
add_subdirectory(src/reconcile)        # continuous reconciliation: fetch-off-loop/apply-on-loop (3.1)
add_subdirectory(src/marketdata)       # tick stream tradability states (Live/Stale/...) (3.5)
add_subdirectory(src/health)           # liveness + self-health watchdogs (3.6)
add_subdirectory(src/modes)            # degradation-posture coordinator (3.7)
add_subdirectory(src/observability)    # structured logging + provenance + audit (4.1)
add_subdirectory(src/alerting)         # alerting + dead-man's-switch heartbeat (4.3)
add_subdirectory(src/ledger)           # tamper-evident hash-chained + Ed25519-signed ledger (4.4)
add_subdirectory(src/cli)              # thin operator CLI + localhost health endpoint (4.6)
# --- waves append here ---
#   src/persistence  src/refdata  src/risk  src/session
#   src/modes  src/runtime  src/observability  src/cli

if(BROKER_EXEC_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# ── Architecture enforcement (the import-linter analog, build-time) ──
# Fails the configure step if the inner layers ever link an outer one.
enforce_no_dependency(TARGET broker_exec_domain FORBIDS broker_exec_adapters)
enforce_no_dependency(TARGET broker_exec_ports  FORBIDS broker_exec_adapters)
