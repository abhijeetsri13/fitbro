# broker_exec::ports — pure abstract interfaces (header-only). May depend on
# domain + errors only, never on adapters. ClockPort/BrokerPort/StorePort/
# AlertSink/SecretProvider/RefDataPort are the only surface the core depends on.

add_library(broker_exec_ports INTERFACE)
add_library(broker_exec::ports ALIAS broker_exec_ports)

target_include_directories(broker_exec_ports
  INTERFACE ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_ports INTERFACE cxx_std_20)

# Port headers include domain types AND the Result<T>/Error vocabulary, so both
# are part of the public interface of this target.
target_link_libraries(broker_exec_ports
  INTERFACE
    broker_exec_domain
    broker_exec_errors)

if(BROKER_EXEC_BUILD_TESTS)
  # Contract test: a mock implements every port. Proves the abstractions are
  # complete and implementable (and exercises the Ok/void substitute).
  add_executable(broker_exec_ports_tests ports_test.cpp)
  target_link_libraries(broker_exec_ports_tests
    PRIVATE
      broker_exec_ports
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_ports_tests COMMAND broker_exec_ports_tests)
endif()
