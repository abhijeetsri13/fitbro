# broker_exec::composition — THE COMPOSITION ROOT (Story 6.3, SM-3, FR-1/FR-2,
# CAP-2). Turns a config string ("kite" | "kotak") into an assembled
# ports::BrokerPort + that broker's CapabilitySet, rejecting a missing capability
# at LOAD time (CapabilitySet::require_all) before anything that can reach a
# broker is constructed.
#
# THIS TARGET LINKS ADAPTERS ON PURPOSE, AND IT IS THE ONLY LIBRARY TARGET THAT
# SHOULD. The boundary rule enforced in the top-level CMakeLists by
# enforce_no_dependency() is `domain`/`ports` must not link `adapters` — the
# INNER layers stay pure. A composition root is the outermost layer: it links
# every adapter so that nothing else has to, which is exactly what lets a
# strategy depend on ports::BrokerPort alone. The boundary checks are unchanged
# and still pass; nothing inner links this target.
#
# NO new Conan dependency: cpr/nlohmann_json arrive transitively via the two
# adapter targets that already use them.

add_library(broker_exec_composition STATIC
  broker_factory.cpp
)
add_library(broker_exec::composition ALIAS broker_exec_composition)

target_include_directories(broker_exec_composition
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_composition PUBLIC cxx_std_20)

# PUBLIC — every one of these appears in broker_factory.hpp:
#   kite            -> adapters::kite::HttpClient in BrokerDeps
#   adapters_kotak  -> adapters::kotak::BundleProvider in BrokerDeps
#   capabilities    -> CapabilitySet / Capability in BrokerOptions + BrokerAssembly
#   config          -> config::Config in the config-driven make_broker() overload
#   ports           -> ports::BrokerPort / ports::Ok
#   errors          -> the Result<T>/Error vocabulary
# PRIVATE — implementation detail of broker_factory.cpp:
#   domain          -> the OrderIntent/Order/Trade/Position value types the owning
#                      BrokerPort wrappers forward (it also arrives transitively
#                      via adapters_kotak; named here because this TU uses it)
target_link_libraries(broker_exec_composition
  PUBLIC
    broker_exec_kite
    broker_exec_adapters_kotak
    broker_exec_capabilities
    broker_exec_config
    broker_exec_ports
    broker_exec_errors
  PRIVATE
    broker_exec_domain
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_composition_tests broker_factory_test.cpp)
  target_link_libraries(broker_exec_composition_tests
    PRIVATE
      broker_exec_composition
      broker_exec_domain   # OrderIntent/Quantity/Price in the per-call gate tests
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_composition_tests COMMAND broker_exec_composition_tests)
endif()
