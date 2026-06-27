# broker_exec::capabilities — per-broker capability model + early rejection
# (Story 2.5, FR-2). Pure value logic: tri-state Support, an immutable
# CapabilitySet, and reject-only require()/require_all() gates. Depends inward
# only on `errors` (Result/Error in the public API); NO transport/adapter/SDK
# dependency.

add_library(broker_exec_capabilities STATIC
  capabilities.cpp
)
add_library(broker_exec::capabilities ALIAS broker_exec_capabilities)

target_include_directories(broker_exec_capabilities
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_capabilities PUBLIC cxx_std_20)

# Result<Ok>/Error appear in the public API (require()/require_all() return
# Result<Ok>), so broker_exec_errors is PUBLIC. The warnings/sanitizers policy
# stays PRIVATE per convention. No domain/ports/adapter link is needed beyond
# the header-only Ok type that ships with errors via result.hpp.
target_link_libraries(broker_exec_capabilities
  PUBLIC
    broker_exec_errors
  PRIVATE
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_capabilities_tests capabilities_test.cpp)
  target_link_libraries(broker_exec_capabilities_tests
    PRIVATE
      broker_exec_capabilities
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_capabilities_tests COMMAND broker_exec_capabilities_tests)
endif()
