# broker_exec::ratelimit — the token-bucket send-rate limiter with a reserved
# exit lane (Story 2.12, FR-24, architecture CC-1/CC-5). Admission-only: it
# decides which request enters dispatch and when a token is free; it never owns
# the broker socket (the per-send timeout => UNKNOWN is the dispatcher, Story 1.9).
# Depends inward only on ports (ClockPort/Ok) and errors. No new Conan dep.

add_library(broker_exec_ratelimit STATIC
  rate_limiter.cpp
)
add_library(broker_exec::ratelimit ALIAS broker_exec_ratelimit)

target_include_directories(broker_exec_ratelimit
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_ratelimit PUBLIC cxx_std_20)

# ClockPort/Ok (ports) and Result<Ok>/Error (errors) appear in the public API
# (the ctor takes a ClockPort&; acquire() returns Result<Ok>), so both are
# PUBLIC. Warnings/sanitizers stay PRIVATE per convention.
target_link_libraries(broker_exec_ratelimit
  PUBLIC
    broker_exec_ports
    broker_exec_errors
  PRIVATE
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_ratelimit_tests rate_limiter_test.cpp)
  target_link_libraries(broker_exec_ratelimit_tests
    PRIVATE
      broker_exec_ratelimit
      broker_exec_clock      # TestClock (deterministic ClockPort: steady advance)
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_ratelimit_tests COMMAND broker_exec_ratelimit_tests)
endif()
