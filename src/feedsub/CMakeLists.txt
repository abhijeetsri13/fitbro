# broker_exec::feedsub — the websocket resubscribe guard (connected-but-mute
# defense). Owns the desired subscription set and, on EVERY (re)connect, hands
# back the FULL set to re-issue + arms a first-tick deadline per token; a token
# still silent past its deadline is MuteAfterResubscribe (connected-but-mute) and
# NOT tradable. Complementary to marketdata (which classifies staleness once ticks
# flow) but does NOT depend on it. NO websocket/Conan dep — the transport is a
# thin adapter behind the caller's seam. Depends inward only: ports (ClockPort).

add_library(broker_exec_feedsub STATIC
  resubscribe_guard.cpp
)
add_library(broker_exec::feedsub ALIAS broker_exec_feedsub)

target_include_directories(broker_exec_feedsub
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_feedsub PUBLIC cxx_std_20)

# Only ClockPort (ports) appears in this module's public header, so ports is the
# sole PUBLIC dependency. Warnings/sanitizers stay PRIVATE per convention.
target_link_libraries(broker_exec_feedsub
  PUBLIC
    broker_exec_ports
  PRIVATE
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  # The test exe ALSO links broker_exec_clock for the deterministic TestClock
  # (advances the monotonic clock past the first-tick deadline to force mute).
  add_executable(broker_exec_feedsub_tests resubscribe_guard_test.cpp)
  target_link_libraries(broker_exec_feedsub_tests
    PRIVATE
      broker_exec_feedsub
      broker_exec_clock
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_feedsub_tests COMMAND broker_exec_feedsub_tests)
endif()
