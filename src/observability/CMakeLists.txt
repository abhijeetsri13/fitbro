# broker_exec::observability — structured logging, provenance & audit trail
# (Story 4.1, FR-27, NFR-8, SEC-3).
#
# Renders each order-decision step as ONE versioned JSON line, binds
# domain::scrub over the whole line (no token reaches a sink), and reconstructs
# an order's decision path in memory. Depends inward only: domain (scrub +
# pure types), errors, ports (ClockPort). spdlog is an IMPLEMENTATION DETAIL of
# the logger .cpp (kept out of the public headers via forward declaration), so
# it links PRIVATE. nlohmann_json is PUBLIC: AuditEvent::fields is nlohmann::json
# in a public header, so any in-tree consumer needs it on its include path.
# ports is PUBLIC too: ports::ClockPort appears in the public structured_logger.hpp.

add_library(broker_exec_observability STATIC
  audit_event.cpp
  structured_logger.cpp
  audit_trail.cpp
  reports.cpp
)
add_library(broker_exec::observability ALIAS broker_exec_observability)

target_include_directories(broker_exec_observability
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_observability PUBLIC cxx_std_20)

target_link_libraries(broker_exec_observability
  PUBLIC
    broker_exec_domain
    broker_exec_errors
    broker_exec_ports
    nlohmann_json::nlohmann_json
  PRIVATE
    spdlog::spdlog
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_observability_tests
    observability_test.cpp
    reports_test.cpp)
  target_link_libraries(broker_exec_observability_tests
    PRIVATE
      broker_exec_observability
      broker_exec_clock            # TestClock drives the ts stamping
      broker_exec_domain           # assert scrub() hid the token in the line
      spdlog::spdlog               # a capturing sink + make_logger
      nlohmann_json::nlohmann_json # parse the emitted line back in the test body
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_observability_tests COMMAND broker_exec_observability_tests)
endif()
