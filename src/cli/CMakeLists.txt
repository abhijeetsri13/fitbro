# broker_exec::cli — the thin operator CLI + localhost health endpoint (Story 4.6,
# FR-36). A SHELL over the public API: it owns no business logic. The CLI verbs are
# injected seams (OperatorApi) and the endpoint serves an immutable, scrubbed
# HealthSnapshot published by the main loop (HealthState). Depends inward only:
# domain (scrub), errors (Result/Error), ports, session (SessionState), nlohmann_json.
# Two header-only transport deps — CLI11 and cpp-httplib — are CONFINED to the .cpp
# files (cli_app.cpp / health_endpoint.cpp), so they link PRIVATE.

add_library(broker_exec_cli STATIC
  health_snapshot.cpp
  health_state.cpp
  health_endpoint.cpp
  cli_app.cpp
)
add_library(broker_exec::cli ALIAS broker_exec_cli)

target_include_directories(broker_exec_cli
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_cli PUBLIC cxx_std_20)

# The public headers surface Result<T>/Error (errors), SessionState (session) and
# the snapshot value, so errors/ports/session/domain are PUBLIC. CLI11 and httplib
# appear ONLY in the .cpp files (transport detail), so they link PRIVATE — their
# headers never leak into a consumer. nlohmann_json renders the snapshot in the .cpp
# (to_json) so it is PRIVATE. warnings/sanitizers stay PRIVATE per convention.
target_link_libraries(broker_exec_cli
  PUBLIC
    broker_exec_domain
    broker_exec_errors
    broker_exec_ports
    broker_exec_session
  PRIVATE
    nlohmann_json::nlohmann_json
    CLI11::CLI11
    httplib::httplib
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  # No socket bind and no argv parsing in the suite — the tests exercise the pure
  # seams directly: route(), dispatch(), to_json, is_ready/is_live, is_mutating.
  # The test exe links broker_exec_session to build real SessionState values.
  add_executable(broker_exec_cli_tests
    health_snapshot_test.cpp
    health_endpoint_test.cpp
    cli_app_test.cpp)
  target_link_libraries(broker_exec_cli_tests
    PRIVATE
      broker_exec_cli
      broker_exec_session
      nlohmann_json::nlohmann_json  # the suite PARSES the served JSON body to assert structure
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_cli_tests COMMAND broker_exec_cli_tests)
endif()
