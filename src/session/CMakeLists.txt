# broker_exec::session — daily Kite login modeled as first-class establishment +
# expiry detection (Story 2.4, FR-21, IBR-1/IBR-6).
#
# An UPPER orchestration module (not domain/ports): it composes the Kite
# HttpClient seam + map_http_error (adapters/kite), the AES-256-GCM TokenStore
# (secrets), the SecretProvider port, and the typed error taxonomy. It exchanges
# an operator-supplied request_token for the daily access_token (checksum via
# OpenSSL SHA-256), persists it encrypted, and normalizes liveness into
# SessionState. No transport/SDK lives here — only the seam.

add_library(broker_exec_session STATIC
  kite_session_establisher.cpp
  safe_start.cpp
)
add_library(broker_exec::session ALIAS broker_exec_session)

target_include_directories(broker_exec_session
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_session PUBLIC cxx_std_20)

# The Result<T>/Error vocabulary and the SecretProvider port appear in the public
# header (KiteSessionEstablisher's ctor/returns), so ports/errors are PUBLIC. The
# Kite HttpClient seam + TokenStore appear in the public header only as references
# (the include path is already covered by PROJECT_SOURCE_DIR/include) and their
# symbols are an implementation detail of the .cpp, so kite/secrets are PRIVATE —
# CMake still propagates a static lib's PRIVATE deps to the final link. OpenSSL
# (SHA-256), nlohmann_json (envelope parse), and warnings/sanitizers are likewise
# implementation details -> PRIVATE.
# domain::Order appears in safe_start.hpp's public API (require_no_legacy_stops /
# is_legacy_trigger_less_stop — the IMP-11 cold-boot guard), so domain is PUBLIC.
target_link_libraries(broker_exec_session
  PUBLIC
    broker_exec_ports
    broker_exec_errors
    broker_exec_domain
  PRIVATE
    broker_exec_kite
    broker_exec_secrets
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_session_tests session_test.cpp safe_start_test.cpp)
  target_link_libraries(broker_exec_session_tests
    PRIVATE
      broker_exec_session
      broker_exec_secrets          # real TokenStore for persistence assertions
      broker_exec_refdata          # real InstrumentMaster::require_fresh (safe-start AC-3)
      broker_exec_clock            # TestClock drives the real InstrumentMaster (AC-3)
      broker_exec_domain           # domain::Order construction (IMP-11 legacy-stop guard)
      nlohmann_json::nlohmann_json # json appears directly in the test body
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_session_tests COMMAND broker_exec_session_tests)
endif()
