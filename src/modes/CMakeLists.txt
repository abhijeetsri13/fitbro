# broker_exec::modes — the degradation-posture coordinator (Story 3.7, FR-26).
# The SINGLE posture authority: detectors FEED it DetectorSignals and it resolves
# the severest posture (a total order, std::max over ascending enum values); the
# gate (2.8) enforces it. Depends inward only: ports (AlertSink), errors
# (RiskRejected), health (HealthSignal, for from_health). NO new Conan dep.

add_library(broker_exec_modes STATIC
  posture.cpp
  killswitch.cpp
  trading_mode.cpp
)
add_library(broker_exec::modes ALIAS broker_exec_modes)

target_include_directories(broker_exec_modes
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_modes PUBLIC cxx_std_20)

# Posture/DetectorSignal plus the AlertSink (ports) and Error/Result (errors)
# appear in this module's public header, so ports + errors are PUBLIC. health is
# used only by `from_health` whose parameter (health::HealthSignal) appears in the
# public header, but the coupling is the single mapping function and callers wire
# the watchdog directly; per convention (mirroring health's marketdata link) it
# stays PRIVATE. The warnings/sanitizers policy stays PRIVATE.
target_link_libraries(broker_exec_modes
  PUBLIC
    broker_exec_ports
    broker_exec_errors
  PRIVATE
    broker_exec_health
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  # The test exe ALSO links broker_exec_health (to drive the from_health mapping
  # test with real health::HealthSignal values).
  add_executable(broker_exec_modes_tests posture_test.cpp killswitch_test.cpp trading_mode_test.cpp)
  target_link_libraries(broker_exec_modes_tests
    PRIVATE
      broker_exec_modes
      broker_exec_health
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_modes_tests COMMAND broker_exec_modes_tests)
endif()
