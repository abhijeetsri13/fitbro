# broker_exec::health — liveness and process self-health watchdogs (Story 3.6,
# FR-20). DETECTS connected-but-mute feeds, resource-headroom breaches (disk/
# memory/handles), and clock skew/stall, and signals a single fail-closed
# degrade-to-exit-only verdict (the posture mapping is Story 3.7). Depends inward
# only: ports (AlertSink), errors (Result/Error), marketdata (the mute-feed
# helper reuses the Story-3.5 MarketDataView). NO new Conan dep.

add_library(broker_exec_health STATIC
  watchdog.cpp
)
add_library(broker_exec::health ALIAS broker_exec_health)

target_include_directories(broker_exec_health
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_health PUBLIC cxx_std_20)

# AlertSink/AlertLevel (ports) and Error/Result (errors) appear in this module's
# public header, so those are PUBLIC. The marketdata MarketDataView also appears
# in the public header (the mute-feed helper takes it by const-ref), but the
# implementation links it; per convention we keep marketdata PRIVATE — the helper
# is the only coupling and callers already depend on marketdata directly. The
# warnings/sanitizers policy stays PRIVATE.
target_link_libraries(broker_exec_health
  PUBLIC
    broker_exec_ports
    broker_exec_errors
  PRIVATE
    broker_exec_marketdata
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  # The test exe ALSO links broker_exec_marketdata (to build a MarketDataView for
  # the mute-feed helper test) and broker_exec_clock (the deterministic TestClock
  # driving that view).
  add_executable(broker_exec_health_tests watchdog_test.cpp)
  target_link_libraries(broker_exec_health_tests
    PRIVATE
      broker_exec_health
      broker_exec_marketdata
      broker_exec_clock
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_health_tests COMMAND broker_exec_health_tests)
endif()
