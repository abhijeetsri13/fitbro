# broker_exec::kite — the native Kite Connect REST client (Story 2.3, FR-25).
#
# An ADAPTER (outer layer): the only place a transport/SDK may live (hexagonal
# boundary enforced). It speaks Kite Connect v3 over an HttpClient seam and maps
# raw HTTP/Kite errors to the typed taxonomy, scrubbed of secrets. cpr/libcurl is
# confined to cpr_http_client.cpp; no transport type leaks into a public header.

add_library(broker_exec_kite STATIC
  cpr_http_client.cpp
  kite_rest_client.cpp
  kite_broker_adapter.cpp
)
add_library(broker_exec::kite ALIAS broker_exec_kite)

target_include_directories(broker_exec_kite
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_kite PUBLIC cxx_std_20)

# SecretProvider + the Result<T>/Error vocabulary appear in the public headers
# (KiteRestClient's ctor/returns), so ports/errors are PUBLIC. nlohmann_json is
# also PUBLIC: KiteRestClient's methods return Result<nlohmann::json>, so any
# in-tree consumer needs the type on its include path. cpr (only
# cpr_http_client.cpp), domain (scrub), fillnorm (the canonical quantity-first fill
# normalizer that sizes the square-off exit — used only inside
# kite_broker_adapter.cpp) and warnings/sanitizers are implementation details ->
# PRIVATE.
target_link_libraries(broker_exec_kite
  PUBLIC
    broker_exec_ports
    broker_exec_errors
    nlohmann_json::nlohmann_json
  PRIVATE
    cpr::cpr
    broker_exec_domain
    broker_exec_fillnorm
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_kite_tests kite_rest_client_test.cpp)
  target_link_libraries(broker_exec_kite_tests
    PRIVATE
      broker_exec_kite
      broker_exec_domain          # assert scrub() hid the token in error text
      nlohmann_json::nlohmann_json # json appears directly in the test body
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_kite_tests COMMAND broker_exec_kite_tests)
endif()
