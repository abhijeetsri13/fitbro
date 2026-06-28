# broker_exec::options — hedge-first option-spread execution (Story 5.1, FR-16).
# Pure, ordered, fail-closed orchestration over INJECTED SEAMS: the protective
# hedge is placed + confirmed BEFORE the naked-risk short, so the seller is never
# momentarily naked. Depends inward only on ports (BrokerAck/AlertSink/Ok) and
# errors (Result/Error); no broker SDK, no transport. NO new Conan dep. Mirrors
# src/modes/CMakeLists.txt.

add_library(broker_exec_options STATIC
  hedge_first.cpp
  basket.cpp
  sliced_leg.cpp
)
add_library(broker_exec::options ALIAS broker_exec_options)

target_include_directories(broker_exec_options
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_options PUBLIC cxx_std_20)

# BrokerAck/AlertSink/Ok (ports) and Error/Result (errors) appear in this module's
# public header, so ports + errors are PUBLIC. Story 5.3's sliced_leg.cpp calls
# slicing::FreezeSlicer::slice — used only in the .cpp, so broker_exec_slicing is a
# PRIVATE link dependency. The warnings/sanitizers policy stays PRIVATE per convention.
target_link_libraries(broker_exec_options
  PUBLIC
    broker_exec_domain
    broker_exec_errors
    broker_exec_ports
  PRIVATE
    broker_exec_slicing
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_options_tests hedge_first_test.cpp basket_test.cpp sliced_leg_test.cpp)
  target_link_libraries(broker_exec_options_tests
    PRIVATE
      broker_exec_options
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_options_tests COMMAND broker_exec_options_tests)
endif()
