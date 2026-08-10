# broker_exec::composition — THE COMPOSITION ROOT.
#
# TWO HALVES, ONE TARGET:
#   * broker_factory.cpp (Story 6.3, SM-3, FR-1/FR-2, CAP-2) — turns a config
#     string ("kite" | "kotak") into an assembled ports::BrokerPort + that
#     broker's CapabilitySet, rejecting a missing capability at LOAD time
#     (CapabilitySet::require_all) before anything that can reach a broker is
#     constructed.
#   * engine_assembly.cpp (IMP-12) — turns the injected safety modules into a
#     fully-wired, FAIL-CLOSED decision core: an entry-capable engine REFUSES to
#     build unless funds/calendar/risk/kill/posture (and the rest) are actually
#     present, closing the "an absent GateContext predicate PASSES" hole.
# They share a target because they are the same layer answering the same
# question from two sides ("may I reach this broker" / "may this order go").
#
# THIS TARGET LINKS ADAPTERS ON PURPOSE, AND IT IS THE ONLY LIBRARY TARGET THAT
# SHOULD. The boundary rule enforced in the top-level CMakeLists by
# enforce_no_dependency() is `domain`/`ports` must not link `adapters` — the
# INNER layers stay pure. A composition root is the outermost layer: it links
# every adapter so that nothing else has to, which is exactly what lets a
# strategy depend on ports::BrokerPort alone. The boundary checks are unchanged
# and still pass; nothing inner links this target.
#
# WHAT IT DELIBERATELY DOES *NOT* LINK: broker_exec_idempotency (and through it
# the SQLite projection). The duplicate check arrives as a std::function probe
# so the decision core never owns a database handle to answer a boolean — see
# EngineDeps::duplicate_probe.
#
# DEFERRED: SPLITTING engine_assembly.cpp INTO ITS OWN broker_exec_engine TARGET.
# It would link only the decision-core modules (no kite/kotak, no cpr), so a
# strategy-only consumer would stop paying for the broker transports at link
# time. It was weighed and deferred here rather than silently skipped: it costs a
# second library AND a second test target out of ONE module directory, plus a
# target name that no longer matches its broker_exec/<module> include path —
# a direct deviation from the binding "one CMake target per module" rule in
# docs/conventions.md, bought for a link footprint that only matters once a
# strategy-only binary actually exists. The dependency DIRECTION is already
# correct (engine_assembly.cpp includes no adapter header); this is packaging,
# not layering. Revisit when that binary lands. The caveat is also recorded in
# engine_assembly.hpp so a reader of the header learns it without opening CMake.
#
# NO new Conan dependency: cpr/nlohmann_json arrive transitively via the two
# adapter targets that already use them; every module added below is first-party.

add_library(broker_exec_composition STATIC
  broker_factory.cpp
  engine_assembly.cpp
)
add_library(broker_exec::composition ALIAS broker_exec_composition)

target_include_directories(broker_exec_composition
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_composition PUBLIC cxx_std_20)

# PUBLIC — every one of these appears in a composition PUBLIC header:
#   broker_factory.hpp
#     kite            -> adapters::kite::HttpClient in BrokerDeps
#     adapters_kotak  -> adapters::kotak::BundleProvider in BrokerDeps
#     capabilities    -> CapabilitySet / Capability in BrokerOptions + BrokerAssembly
#     config          -> config::Config in the config-driven make_broker() overload
#   engine_assembly.hpp
#     risk            -> GateContext/GateOutcome, FundsView, RiskEngine/Limits/State
#     modes           -> KillState, PostureCoordinator, Posture, DetectorSignal
#     refdata         -> TradingCalendar (the gate's entry time-window authority)
#     priceband       -> PriceBand in PreflightInputs, BandCheckResult in the outcome
#     marginsafety    -> MarginInputs/MarginSafetyConfig/MarginSafetyResult
#     modifyguard     -> OrderModifyState in ModifyContext, ModifyResult in the outcome
#     session         -> session::SessionState in SessionSnapshot
#   both
#     ports           -> ports::BrokerPort / ports::Ok
#     errors          -> the Result<T>/Error vocabulary
#     domain          -> OrderIntent/Instrument/Money/Price/Product in both headers
#
# PRIVATE — implementation details of the .cpp files:
#   sessionguard      -> assess_session / require_op_allowed, called from
#                        engine_assembly.cpp only (SessionSnapshot in the header
#                        carries a session::SessionState, not a SessionPosture)
target_link_libraries(broker_exec_composition
  PUBLIC
    broker_exec_kite
    broker_exec_adapters_kotak
    broker_exec_capabilities
    broker_exec_config
    broker_exec_risk
    broker_exec_modes
    broker_exec_refdata
    broker_exec_priceband
    broker_exec_marginsafety
    broker_exec_modifyguard
    broker_exec_session
    broker_exec_domain
    broker_exec_ports
    broker_exec_errors
  PRIVATE
    broker_exec_sessionguard
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_composition_tests
    broker_factory_test.cpp
    engine_assembly_test.cpp)
  target_link_libraries(broker_exec_composition_tests
    PRIVATE
      broker_exec_composition
      broker_exec_domain   # OrderIntent/Quantity/Price in the per-call gate tests
      broker_exec_clock    # TestClock — the deterministic ClockPort behind the
                           # real FundsView + TradingCalendar in the smoke test
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_composition_tests COMMAND broker_exec_composition_tests)
endif()
