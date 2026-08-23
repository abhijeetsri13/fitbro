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

# This project uses no C++20 modules, and asking CMake to scan for them broke the
# one consumer of compile_commands.json we have. With cxx_std_20 under Ninja,
# CMake turns module scanning on by default and writes an `@<target>.modmap`
# response-file flag into every entry. Those files are produced by the BUILD, so
# clang-tidy — which the lint job runs after configure, without building — died on
# every translation unit with
#
#   error: no such file or directory: '@src/.../kite_broker_adapter.cpp.o.modmap'
#
# before parsing a line of code. The job still reported six-figure warning counts,
# which is what a failing driver dumping partially-parsed headers looks like, so
# it read as "analysis with findings" rather than "analysis never ran". Turning
# scanning off removes the flag, makes clang-tidy work, and drops a per-TU
# dependency-scan pass from every Ninja build. Revisit only if this project ever
# actually adopts modules. (#47)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

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
# `errors` owns Result<T>/Error and depends on nothing, so it is added FIRST:
# `platform` now returns a typed Result<FileLock> from its cross-process file
# lock (Story 6.5b) and therefore links it PUBLICly.
add_subdirectory(src/errors)
add_subdirectory(src/platform)   # cross-platform OS seam (fsync/_commit, perms, file lock, ...)
add_subdirectory(src/domain)
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
# --- Epic 5 ---
add_subdirectory(src/options)          # hedge-first option execution: never momentarily naked (5.1)
# --- Epic 6 ---
add_subdirectory(src/isolation)        # per-strategy virtual book + global limit + independent stop (6.4)
add_subdirectory(src/supervisor)       # supervisor decision core: exit-code contract + restart-backoff + absence alarm (6.5) + multi-account SupervisorPlan (6.5b)
add_subdirectory(src/accounts)         # multi-account process wiring (6.5b): per-account data-dir layout + shared refdata cache behind the cross-process file lock
add_subdirectory(src/brokerreason)     # versioned canonical rejection/status classifier: fail-closed reason + retry posture
add_subdirectory(src/fillnorm)         # canonical fill normalizer: drive state off filled qty not event type; a push is never authoritative (exit only off a reconciled fill)
# Kotak Neo adapter (6.1 transport + 6.2 BrokerPort impl).
# Placed after brokerreason/fillnorm/secrets/capabilities, the modules it links —
# 6.2's OrderState mapping drives off fillnorm's quantity-first normalizer.
add_subdirectory(src/adapters/kotak)
add_subdirectory(src/protection)       # library-owned protective-stop supervisor: never trust the GTT; re-arm on fired-but-unfilled
add_subdirectory(src/modifyguard)      # modify-order safety guard: never let a modify cancel a working/partially-filled remainder
add_subdirectory(src/priceband)        # pre-submission circuit/LPP price-band validator: block out-of-band entry, clamp out-of-band exit
add_subdirectory(src/sessionguard)     # mid-session re-auth guard: detect a token that died DURING the session -> freeze entries, keep exits/reconcile alive, alert
add_subdirectory(src/marginsafety)     # margin safety buffer: fail-closed buffer over the under-counting broker margin API + worst-case multi-leg
# NOTE: src/fillnorm is added ABOVE (before src/adapters/kotak), which links it.
add_subdirectory(src/feedsub)          # websocket resubscribe guard: on every (re)connect re-issue the FULL subscription set + verify a first tick per token by a deadline -> flag connected-but-mute tokens (not tradable)

# Composition root (6.3): config "kite"|"kotak" -> assembled ports::BrokerPort +
# CapabilitySet, with require_all() gating at LOAD time. Placed AFTER both adapter
# targets and after config/capabilities, all of which it links. It is the
# OUTERMOST layer and the only library target that links adapters on purpose —
# the boundary rule (enforced below) is that domain/ports must not, and nothing
# inner links this.
add_subdirectory(src/composition)
# IMP-20: the process boot sequence + the exit-code contract, and the executable
# the committed systemd unit's ExecStart points at. `boot` is the OUTERMOST
# library layer (it links the composition root, and through it every adapter);
# `src/main` is a thin shell over it. Both must come AFTER every module they wire.
add_subdirectory(src/boot)
add_subdirectory(src/main)
# --- waves append here ---
#   src/persistence  src/refdata  src/risk  src/session
#   src/modes  src/runtime  src/observability  src/cli

if(BROKER_EXEC_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# ── Architecture enforcement (the import-linter analog, build-time) ──
# Fails the configure step if the inner layers ever link an outer one.
#
# EVERY OUTER TARGET IS NAMED, not just the `broker_exec_adapters` INTERFACE
# umbrella. The concrete adapter targets (broker_exec_kite,
# broker_exec_adapters_kotak) do NOT link that umbrella — they were added as
# independent targets — so listing only `broker_exec_adapters` would have let
# `domain` link the real Kite adapter without tripping anything. The composition
# root is listed for the same reason: it links every adapter, so an inner layer
# reaching it would import all of them transitively.
# `broker_exec_boot` is listed for the SAME reason `broker_exec_composition` is:
# it links the composition root and therefore every adapter transitively, so an
# inner layer reaching it would import all of them without naming one.
set(BROKER_EXEC_OUTER_TARGETS
  broker_exec_adapters
  broker_exec_kite
  broker_exec_adapters_kotak
  broker_exec_composition
  broker_exec_boot)
enforce_no_dependency(TARGET broker_exec_domain FORBIDS ${BROKER_EXEC_OUTER_TARGETS})
enforce_no_dependency(TARGET broker_exec_ports  FORBIDS ${BROKER_EXEC_OUTER_TARGETS})
