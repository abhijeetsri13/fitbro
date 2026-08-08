# broker_exec::adapters_kotak — the native Kotak Neo transport: multi-step auth,
# REST protocol, error mapping and the pure websocket protocol layer (Story 6.1,
# FR-1/FR-25, IBR-6).
#
# An ADAPTER (outer layer): the only place a transport/SDK may live (hexagonal
# boundary enforced). It reuses the broker-agnostic HttpClient seam that shipped
# under adapters/kite (header-only; see kotak_transport.hpp for the tier-2 hoist
# note), so there is NO new Conan dependency and NO websocket library here — the
# WS layer is pure frame building/classification.

add_library(broker_exec_adapters_kotak STATIC
  kotak_errors.cpp
  kotak_session.cpp
  kotak_rest_client.cpp
  kotak_ws_protocol.cpp
  kotak_capabilities.cpp
)
add_library(broker_exec::adapters_kotak ALIAS broker_exec_adapters_kotak)

target_include_directories(broker_exec_adapters_kotak
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_adapters_kotak PUBLIC cxx_std_20)

# PUBLIC: the Result<T>/Error vocabulary, the SecretProvider port, the TokenStore
# (KotakSessionEstablisher's ctor), the CapabilitySet returned by
# kotak_capabilities(), and nlohmann_json (the REST methods return
# Result<nlohmann::json>) all appear in the public headers.
# PRIVATE: domain (scrub) and brokerreason (the canonical reject classifier) are
# implementation details of the .cpp files, as are warnings/sanitizers.
# NOTE: broker_exec_kite is deliberately NOT linked — only its header-only
# HttpClient seam is included, and the include path is already PUBLIC above.
target_link_libraries(broker_exec_adapters_kotak
  PUBLIC
    broker_exec_ports
    broker_exec_errors
    broker_exec_secrets
    broker_exec_capabilities
    nlohmann_json::nlohmann_json
  PRIVATE
    broker_exec_domain
    broker_exec_brokerreason
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_adapters_kotak_tests
    kotak_errors_test.cpp
    kotak_session_test.cpp
    kotak_rest_client_test.cpp
    kotak_ws_protocol_test.cpp
  )
  target_link_libraries(broker_exec_adapters_kotak_tests
    PRIVATE
      broker_exec_adapters_kotak
      broker_exec_domain            # assert scrub() hid the credentials in error text
      broker_exec_secrets           # real TokenStore for the encrypted-at-rest assertions
      nlohmann_json::nlohmann_json  # json appears directly in the test bodies
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_adapters_kotak_tests COMMAND broker_exec_adapters_kotak_tests)
endif()
