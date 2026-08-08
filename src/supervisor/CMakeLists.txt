# broker_exec::supervisor — the supervisor DECISION CORE (Story 6.5, FR-33,
# AC-3) plus the MULTI-ACCOUNT plan (Story 6.5b).
#
#   supervisor_policy — maps a supervised process's exit code to a
#                       restart-with-backoff / no-restart-escalate / clean-stop
#                       decision plus the absence-alarm policy.
#   supervisor_plan   — the per-account registry over that policy: independent
#                       crash counters, per-account absence alarms, and a
#                       fleet-level summary. Account A's crash loop NEVER changes
#                       account B's decision.
#
# PURE decision logic: no I/O, no ambient clock (time is an injected `now`
# argument), NO process spawning — the OS does supervision through the systemd
# template unit in deploy/systemd/ (that IS the architecture, ID-1). Standalone:
# depends on nothing but the standard library — NO domain/ports/errors, NO new
# Conan dep. (Nothing here is fallible, so there is no Result<T> to return; the
# no-throw contract is met by these being total functions.)

add_library(broker_exec_supervisor STATIC
  supervisor_policy.cpp
  supervisor_plan.cpp
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
  add_executable(broker_exec_supervisor_tests
    supervisor_policy_test.cpp
    supervisor_plan_test.cpp)
  target_link_libraries(broker_exec_supervisor_tests
    PRIVATE
      broker_exec_supervisor
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_supervisor_tests COMMAND broker_exec_supervisor_tests)
endif()
