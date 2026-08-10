# broker_exec::sessionguard — the MID-SESSION RE-AUTH GUARD (fail-closed auth
# safety). Detects a session that died DURING the trading session (TokenException
# / 401 mid-flight) by REUSING brokerreason::classify_rejection as the single
# auth-failure signal, then flips to a NeedsReauth posture that FREEZES new
# entries while keeping EXITS and reconcile READS alive — never crash, never
# blindly retry the auth. Pure-decision module: no I/O, no throw, integer enums
# only (no float). NO new Conan dep.

add_library(broker_exec_sessionguard STATIC
  session_guard.cpp
)
add_library(broker_exec::sessionguard ALIAS broker_exec_sessionguard)

target_include_directories(broker_exec_sessionguard
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_sessionguard PUBLIC cxx_std_20)

# session::SessionState plus the Result/Error/Ok gate vocabulary appear in this
# module's public header, so session/errors/ports are PUBLIC (mirrors the sibling
# modes/priceband modules). brokerreason::classify_rejection and domain::scrub are
# used ONLY in the .cpp (the auth-failure signal + redaction seam), so they are
# PRIVATE. warnings/sanitizers PRIVATE per convention.
target_link_libraries(broker_exec_sessionguard
  PUBLIC
    broker_exec_session
    broker_exec_errors
    broker_exec_ports
  PRIVATE
    broker_exec_brokerreason
    broker_exec_domain
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_sessionguard_tests session_guard_test.cpp)
  target_link_libraries(broker_exec_sessionguard_tests
    PRIVATE
      broker_exec_sessionguard
      broker_exec_session          # session::SessionState values drive the cases
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_sessionguard_tests COMMAND broker_exec_sessionguard_tests)
endif()
