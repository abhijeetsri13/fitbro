# broker_exec::fake_broker — the adversarial in-test broker (Story 1.11, FR-37).
#
# An ADAPTER (outer layer): it implements the `ports::BrokerPort` seam and may
# depend on ports/domain/errors/clock. It is NOT linked by domain/ports (the
# hexagonal boundary check forbids that). It injects the dangerous faults the
# safety core must survive and that the conformance kit (Story 1.12) drives.

add_library(broker_exec_fake_broker STATIC
  fake_broker.cpp
)
add_library(broker_exec::fake_broker ALIAS broker_exec_fake_broker)

target_include_directories(broker_exec_fake_broker
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_fake_broker PUBLIC cxx_std_20)

target_link_libraries(broker_exec_fake_broker
  PUBLIC
    broker_exec_ports
  PRIVATE
    broker_exec_domain
    broker_exec_errors
    broker_exec_clock
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_fake_broker_tests fake_broker_test.cpp)
  target_link_libraries(broker_exec_fake_broker_tests
    PRIVATE
      broker_exec_fake_broker
      broker_exec_domain
      broker_exec_errors
      broker_exec_clock
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_fake_broker_tests COMMAND broker_exec_fake_broker_tests)
endif()
