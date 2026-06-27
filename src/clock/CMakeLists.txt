# broker_exec::clock — the injected time source and its health watch.
#
# SystemClock is the ONLY place steady_clock::now()/system_clock::now() are
# called (binding convention; lint-banned elsewhere). TestClock is a
# deterministic ClockPort for tests. SkewStallDetector surfaces a ClockStatus
# (Healthy/Stalled/Skewed) that later drives degrade-to-exit-only.

add_library(broker_exec_clock STATIC
  system_clock.cpp
  skew_stall_detector.cpp
)
add_library(broker_exec::clock ALIAS broker_exec_clock)

target_include_directories(broker_exec_clock
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_clock PUBLIC cxx_std_20)

# ClockPort/ClockStatus are exposed through this module's public headers, so the
# ports dependency is PUBLIC. Warnings/sanitizers stay PRIVATE per convention.
target_link_libraries(broker_exec_clock
  PUBLIC
    broker_exec_ports
  PRIVATE
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_clock_tests clock_test.cpp)
  target_link_libraries(broker_exec_clock_tests
    PRIVATE
      broker_exec_clock
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_clock_tests COMMAND broker_exec_clock_tests)
endif()
