# broker_exec::protection — the library-owned protective-stop supervisor.
# NEVER trust a broker GTT/SL as a durable stop: every tick it re-verifies the
# protected position is still actually protected and RE-ARMS a fresh, band-aware
# protective exit when a stop fired-but-unfilled (LPP/circuit reject or partial).
# Depends inward only: domain (Money/Side/OrderState), ports (AlertSink), errors
# (Result/Error in the alert path). NO new Conan dep.

add_library(broker_exec_protection STATIC
  stop_supervisor.cpp
)
add_library(broker_exec::protection ALIAS broker_exec_protection)

target_include_directories(broker_exec_protection
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_protection PUBLIC cxx_std_20)

# Money/Side/OrderState (domain) and the AlertSink (ports) appear in the public
# header; Result/Error (errors) ride in via ports. All three are PUBLIC. The
# warnings/sanitizers policy stays PRIVATE per convention.
target_link_libraries(broker_exec_protection
  PUBLIC
    broker_exec_domain
    broker_exec_errors
    broker_exec_ports
  PRIVATE
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_protection_tests stop_supervisor_test.cpp)
  target_link_libraries(broker_exec_protection_tests
    PRIVATE
      broker_exec_protection
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_protection_tests COMMAND broker_exec_protection_tests)
endif()
