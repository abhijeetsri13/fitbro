# broker_exec::supervisor — the supervisor DECISION CORE (Story 6.5, FR-33,
# AC-3). PURE decision logic: maps a supervised process's exit code to a
# restart-with-backoff / no-restart-escalate / clean-stop decision plus the
# absence-alarm policy. No I/O, no clock, no process APIs (that is the deferred
# OS-level wiring's job). Standalone: depends on nothing but the standard library
# — NO domain/ports/errors, NO new Conan dep.

add_library(broker_exec_supervisor STATIC
  supervisor_policy.cpp
)
add_library(broker_exec::supervisor ALIAS broker_exec_supervisor)

target_include_directories(broker_exec_supervisor
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_supervisor PUBLIC cxx_std_20)

# This module is self-contained; only the warnings/sanitizers policy links in,
# PRIVATE per convention.
target_link_libraries(broker_exec_supervisor
  PRIVATE
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_supervisor_tests supervisor_policy_test.cpp)
  target_link_libraries(broker_exec_supervisor_tests
    PRIVATE
      broker_exec_supervisor
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_supervisor_tests COMMAND broker_exec_supervisor_tests)
endif()
