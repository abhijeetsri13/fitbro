# Test suite. Each module registers its sources here as it lands; the orchestrator
# owns this list so parallel module work does not collide.
#
# ORCHESTRATOR-OWNED INTEGRATION POINT: append per-module test sources below.

add_executable(broker_exec_tests
  placeholder_test.cpp
  # --- waves append module test sources here ---
)

target_link_libraries(broker_exec_tests
  PRIVATE
    broker_exec_domain
    broker_exec_ports
    Catch2::Catch2WithMain
    broker_exec_warnings
    broker_exec_sanitizers)

# Run the whole Catch2 binary as one ctest entry. (Avoids a hard dependency on
# Catch2's discovery module; the binary itself still reports per-assertion.)
add_test(NAME broker_exec_tests COMMAND broker_exec_tests)

# ─────────────────────────────────────────────────────────────────────────────
# Story 1.12 — Conformance kit + SIGKILL durability harness (SM-1), the capstone
# of Epic 1. These are the cross-module / integration suites the conventions
# reserve the top-level tests/ directory for. All three are gated by
# BROKER_EXEC_BUILD_TESTS (enable_testing() ran in the top-level dir).
#
# The conformance kit (tests/conformance/conformance_kit.hpp) is a REUSABLE,
# broker-agnostic header: it drives ANY ports::BrokerPort factory through the full
# fault matrix and returns a structured zero-duplicate report. Epic 2 (Kite) and
# Epic 6 (Kotak) certify their adapters by including this same header and passing a
# different BrokerFactory — so this target's link set is the template every adapter
# conformance target reuses.
# ─────────────────────────────────────────────────────────────────────────────

# ── Conformance tests (Catch2) ──────────────────────────────────────────────
# Runs broker_exec::conformance::run_conformance() against the adversarial
# FakeBroker and asserts zero duplicate orders across the whole matrix.
add_executable(broker_exec_conformance_tests
  conformance/conformance_test.cpp)
# The kit header lives in tests/conformance and is #included by relative name.
target_include_directories(broker_exec_conformance_tests
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/conformance)
target_link_libraries(broker_exec_conformance_tests
  PRIVATE
    broker_exec_runtime
    broker_exec_fake_broker
    broker_exec_intentlog
    broker_exec_store
    broker_exec_idempotency
    broker_exec_lifecycle
    broker_exec_clock
    broker_exec_ports
    broker_exec_domain
    broker_exec_errors
    Catch2::Catch2WithMain
    broker_exec_warnings
    broker_exec_sanitizers)
add_test(NAME broker_exec_conformance_tests COMMAND broker_exec_conformance_tests)

# ── Kite adapter conformance (Story 2.14) ───────────────────────────────────
# The SAME broker-agnostic kit, now gating the real KiteBrokerAdapter via a
# stateful recorded Kite-HTTP server (no live creds). Proves zero duplicate
# orders across the full fault matrix for Kite — tier-1 certification.
add_executable(broker_exec_kite_conformance_tests
  conformance/kite_conformance_test.cpp)
target_include_directories(broker_exec_kite_conformance_tests
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/conformance)
target_link_libraries(broker_exec_kite_conformance_tests
  PRIVATE
    broker_exec_kite          # KiteBrokerAdapter + REST client + HttpClient seam
    broker_exec_runtime
    broker_exec_fake_broker   # FaultConfig (the per-scenario fault selector)
    broker_exec_intentlog
    broker_exec_store
    broker_exec_idempotency
    broker_exec_lifecycle
    broker_exec_clock
    broker_exec_ports
    broker_exec_domain
    broker_exec_errors
    Catch2::Catch2WithMain
    broker_exec_warnings
    broker_exec_sanitizers)
add_test(NAME broker_exec_kite_conformance_tests COMMAND broker_exec_kite_conformance_tests)

# ── Kotak Neo adapter conformance (Story 6.2) ───────────────────────────────
# The SAME broker-agnostic kit again, now gating the real KotakBrokerAdapter via a
# stateful recorded Kotak-HTTP server (synthetic session bundle, no live creds).
# Deliberately mirrors the Kite target above line-for-line — same kit, same bar,
# a different BrokerFactory — which is the whole portability claim (FR-37, CAP-13).
#
# TIER-1 ONLY: green here does NOT promote any entry in kotak_capabilities(); they
# stay Unknown until the operator-run live smoke in docs/kotak-min-qty-smoke.md
# passes (architecture TO-6).
add_executable(broker_exec_kotak_conformance_tests
  conformance/kotak_conformance_test.cpp)
target_include_directories(broker_exec_kotak_conformance_tests
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/conformance)
target_link_libraries(broker_exec_kotak_conformance_tests
  PRIVATE
    broker_exec_adapters_kotak  # KotakBrokerAdapter + REST client + HttpClient seam
    broker_exec_capabilities    # the AC-2 "tier-1 flips nothing" capability guard
    broker_exec_runtime
    broker_exec_fake_broker     # FaultConfig (the per-scenario fault selector)
    broker_exec_intentlog
    broker_exec_store
    broker_exec_idempotency
    broker_exec_lifecycle
    broker_exec_clock
    broker_exec_ports
    broker_exec_domain
    broker_exec_errors
    nlohmann_json::nlohmann_json  # the recorded server builds Kotak JSON directly
    Catch2::Catch2WithMain
    broker_exec_warnings
    broker_exec_sanitizers)
add_test(NAME broker_exec_kotak_conformance_tests COMMAND broker_exec_kotak_conformance_tests)

# ── Broker-portability proof (Story 6.3, SM-3) ──────────────────────────────
# The capstone of the portability claim: ONE strategy translation unit, written
# against ports::BrokerPort alone, composed twice through broker_exec_composition
# — once with broker.name = "kite" over the recorded Kite endpoint, once with
# broker.name = "kotak" over the recorded Kotak endpoint — asserting the two
# runs produce an EQUAL decision-path outcome (AC-1); that a required-but-
# uncertified capability is refused at composition time rather than mid-trade
# (AC-2); and that the strategy source names no broker (AC-3).
#
# It reuses the SAME recorded servers the two conformance suites drive
# (conformance/recorded_kite_server.hpp + recorded_kotak_server.hpp, extracted
# from those TUs in this story). A portability proof run against a private copy
# of a fixture would only prove the copy still works, so the include path below
# deliberately reaches into conformance/.
#
# The strategy's own source paths are injected as compile definitions so the
# AC-3 scan reads the exact committed files that were compiled into this binary
# — the same technique the SIGKILL test uses for its worker exe. CMake path
# variables always use forward slashes, so no separator is ever built by hand.

# ── The strategy TU, in its OWN target with a starved link set ──────────────
# AC-3's source scan proves the strategy does not NAME a broker. This proves it
# cannot USE one: an OBJECT library that links ports/domain/errors/capabilities
# and nothing else. There is no adapter symbol, no transport, no composition root
# on its link line, so the day someone reaches for one the BUILD fails — a
# mechanical guarantee that does not depend on anyone re-running a grep.
#
# It is also the honest statement of the portability claim in dependency form:
# this is the entire surface a portable strategy is allowed to have.
add_library(broker_exec_portable_strategy OBJECT
  portability/portable_strategy.cpp)
target_include_directories(broker_exec_portable_strategy
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/portability)
target_compile_features(broker_exec_portable_strategy PUBLIC cxx_std_20)
target_link_libraries(broker_exec_portable_strategy
  PUBLIC
    broker_exec_ports         # ports::BrokerPort — the entire broker surface
    broker_exec_domain        # OrderIntent/Order/Position/Money — value types
    broker_exec_errors        # Result<T>/Error — the normalized failure vocabulary
    broker_exec_capabilities  # the capabilities it declares as data
  PRIVATE
    broker_exec_warnings
    broker_exec_sanitizers)
# Belt and braces: fail CONFIGURE, not just link, if an adapter or the
# composition root ever appears on that list.
enforce_no_dependency(TARGET broker_exec_portable_strategy
  FORBIDS broker_exec_adapters broker_exec_kite broker_exec_adapters_kotak
          broker_exec_composition)

add_executable(broker_exec_portability_tests
  portability/portability_proof_test.cpp
  portability/strategy_scan_test.cpp)
target_include_directories(broker_exec_portability_tests
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/conformance
    ${CMAKE_CURRENT_SOURCE_DIR}/portability)
target_compile_definitions(broker_exec_portability_tests
  PRIVATE
    PORTABLE_STRATEGY_SRC="${CMAKE_CURRENT_SOURCE_DIR}/portability/portable_strategy.cpp"
    PORTABLE_STRATEGY_HDR="${CMAKE_CURRENT_SOURCE_DIR}/portability/portable_strategy.hpp")
target_link_libraries(broker_exec_portability_tests
  PRIVATE
    broker_exec_portable_strategy  # the starved-link-set strategy TU (see above)
    broker_exec_composition     # the composition root under test (links both adapters)
    broker_exec_kite            # RecordedKiteServer's HttpClient seam + the adapter
    broker_exec_adapters_kotak  # RecordedKotakServer's seam + the adapter
    broker_exec_capabilities    # the strategy's declared capabilities (as data)
    broker_exec_config          # config::Config — the value that gets flipped
    broker_exec_fake_broker     # FaultConfig (the recorded servers' fault selector)
    broker_exec_clock           # TestClock: deterministic, no real time
    broker_exec_ports
    broker_exec_domain
    broker_exec_errors
    nlohmann_json::nlohmann_json  # the recorded servers build broker JSON directly
    Catch2::Catch2WithMain
    broker_exec_warnings
    broker_exec_sanitizers)
add_test(NAME broker_exec_portability_tests COMMAND broker_exec_portability_tests)

# ── SIGKILL worker (plain exe — NO Catch2) ──────────────────────────────────
# A standalone `int main(argc,argv)` spawned by the SIGKILL test. It installs the
# Dispatcher pre-send barrier (std::_Exit, the portable uncatchable-kill analog)
# and dies between fsync and send; its `recover` subcommand proves zero duplicates.
add_executable(broker_exec_sigkill_worker
  sigkill/sigkill_worker.cpp)
target_link_libraries(broker_exec_sigkill_worker
  PRIVATE
    broker_exec_runtime
    broker_exec_fake_broker
    broker_exec_intentlog
    broker_exec_store
    broker_exec_idempotency
    broker_exec_lifecycle
    broker_exec_clock
    broker_exec_ports
    broker_exec_domain
    broker_exec_errors
    broker_exec_warnings
    broker_exec_sanitizers)

# ── SIGKILL test (Catch2) ───────────────────────────────────────────────────
# Spawns the worker, kills it in the barrier, runs recovery, asserts zero
# duplicates. The worker's exe path is injected as a compile definition so the
# test spawns the exact build artifact (cross-platform via std::system).
add_executable(broker_exec_sigkill_tests
  sigkill/sigkill_test.cpp)
target_compile_definitions(broker_exec_sigkill_tests
  PRIVATE SIGKILL_WORKER_EXE="$<TARGET_FILE:broker_exec_sigkill_worker>")
# Ensure the worker exe is built before the test that spawns it.
add_dependencies(broker_exec_sigkill_tests broker_exec_sigkill_worker)
target_link_libraries(broker_exec_sigkill_tests
  PRIVATE
    broker_exec_platform
    broker_exec_runtime
    broker_exec_fake_broker
    broker_exec_intentlog
    broker_exec_store
    broker_exec_idempotency
    broker_exec_lifecycle
    broker_exec_clock
    broker_exec_ports
    broker_exec_domain
    broker_exec_errors
    Catch2::Catch2WithMain
    broker_exec_warnings
    broker_exec_sanitizers)
add_test(NAME broker_exec_sigkill_tests COMMAND broker_exec_sigkill_tests)
