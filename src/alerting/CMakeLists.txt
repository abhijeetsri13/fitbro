# broker_exec::alerting — best-effort multi-channel alerting with a dead-man's-
# switch heartbeat (Story 4.3, FR-28, CC-6, ID-3).
#
# Cannot-fail-silently operator notification: a MultiChannelAlertSink delivers a
# scrubbed alert to each channel through an INJECTED PostFn seam (no network in
# this module — the cpr HTTP channel is a thin adapter behind the seam,
# follow-up), succeeding best-effort if >=1 channel delivers; a HeartbeatMonitor
# on the steady ClockPort lets an external watcher alarm on the alerter's
# absence. Depends inward only: ports (AlertSink/ClockPort), errors, domain
# (scrub) + nlohmann (channel body JSON). NO adapter/SDK/transport dependency.

add_library(broker_exec_alerting STATIC
  multi_channel_alert_sink.cpp
)
add_library(broker_exec::alerting ALIAS broker_exec_alerting)

target_include_directories(broker_exec_alerting
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_alerting PUBLIC cxx_std_20)

# ports (AlertSink base + ClockPort) and errors (Result<Ok> carries errors::Error)
# appear in this module's public API, so they are PUBLIC. domain::scrub and the
# nlohmann body building are implementation details of the .cpp, and the
# warnings/sanitizers policy stays PRIVATE per convention.
target_link_libraries(broker_exec_alerting
  PUBLIC
    broker_exec_ports
    broker_exec_errors
  PRIVATE
    broker_exec_domain
    nlohmann_json::nlohmann_json
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_alerting_tests alerting_test.cpp)
  target_link_libraries(broker_exec_alerting_tests
    PRIVATE
      broker_exec_alerting
      broker_exec_clock            # TestClock drives the heartbeat steady clock
      broker_exec_domain           # assert scrub() hid the token in every body
      nlohmann_json::nlohmann_json # parse the captured bodies back in the test
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_alerting_tests COMMAND broker_exec_alerting_tests)
endif()
