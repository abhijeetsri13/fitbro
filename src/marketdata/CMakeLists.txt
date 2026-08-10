# broker_exec::marketdata — tradability-aware market-data view (Story 3.5,
# FR-19, IBR-3). Ingests ticks through an injected seam (on_tick/on_connect/
# on_disconnect + ClockPort) and classifies each symbol Live/Stale/Delayed/
# Disconnected/Unknown; only Live is tradable. The IXWebSocket transport is a
# thin adapter behind the seam (follow-up) — NO websocket/Conan dep here. Depends
# inward only: domain (Price), ports (ClockPort, AlertSink), errors.

add_library(broker_exec_marketdata STATIC
  market_data.cpp
)
add_library(broker_exec::marketdata ALIAS broker_exec_marketdata)

target_include_directories(broker_exec_marketdata
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_marketdata PUBLIC cxx_std_20)

# Price (domain), ClockPort/AlertSink/Ok (ports) and Error/Result (errors) all
# appear in this module's public headers, so those dependencies are PUBLIC.
# Warnings/sanitizers stay PRIVATE per convention.
target_link_libraries(broker_exec_marketdata
  PUBLIC
    broker_exec_domain
    broker_exec_ports
    broker_exec_errors
  PRIVATE
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  # The test exe ALSO links broker_exec_clock for the deterministic TestClock
  # (controls both the steady and wall clocks).
  add_executable(broker_exec_marketdata_tests market_data_test.cpp)
  target_link_libraries(broker_exec_marketdata_tests
    PRIVATE
      broker_exec_marketdata
      broker_exec_clock
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_marketdata_tests COMMAND broker_exec_marketdata_tests)
endif()
