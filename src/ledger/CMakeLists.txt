# broker_exec::ledger — tamper-evident hash-chained ledger + Ed25519-signed EOD
# report + position heartbeat (Story 4.4, FR-29, SE-3/SE-5/SEC-2). Reuses the
# already-built OpenSSL::Crypto (Story 2.2) for BOTH the SHA-256 chain AND
# Ed25519 — NO libsodium, NO new Conan dep. Depends inward only: domain (scrub),
# ports (ClockPort/Ok), platform (durable_sync), errors, nlohmann_json.

add_library(broker_exec_ledger STATIC
  ledger.cpp
)
add_library(broker_exec::ledger ALIAS broker_exec_ledger)

target_include_directories(broker_exec_ledger
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_ledger PUBLIC cxx_std_20)

# Only the Result<T>/Error vocabulary appears in the public headers (every
# fallible op returns Result<T>), so errors is PUBLIC. ClockPort/Ok come from
# ports (header-only vocabulary) but are consumed in the public surface too, so
# ports is PUBLIC. OpenSSL, scrub, the durability seam and JSON are all
# implementation details; warnings/sanitizers stay PRIVATE.
target_link_libraries(broker_exec_ledger
  PUBLIC
    broker_exec_errors
    broker_exec_ports
  PRIVATE
    OpenSSL::Crypto
    broker_exec_domain
    broker_exec_platform
    nlohmann_json::nlohmann_json
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_ledger_tests ledger_test.cpp)
  target_link_libraries(broker_exec_ledger_tests
    PRIVATE
      broker_exec_ledger
      broker_exec_clock
      broker_exec_domain
      OpenSSL::Crypto
      nlohmann_json::nlohmann_json
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_ledger_tests COMMAND broker_exec_ledger_tests)
endif()
