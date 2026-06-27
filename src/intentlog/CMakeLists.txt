# broker_exec::intentlog — the write-ahead intent log (Story 1.5, FR-8/NFR-1).
#
# Every order intent is durably recorded (one JSON line, SHA-256 hash-chained)
# and fsync'd BEFORE any broker send, so a crash never yields a duplicate or an
# un-enumerable order; on boot the log replays head→tail to rebuild the
# client_ref index. Durability goes through broker_exec::platform (durable_sync),
# never a direct fsync/_commit — this module stays OS-agnostic (no #ifdef here).

add_library(broker_exec_intentlog STATIC
  intent_log.cpp
  sha256.cpp
)
add_library(broker_exec::intentlog ALIAS broker_exec_intentlog)

target_include_directories(broker_exec_intentlog
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_intentlog PUBLIC cxx_std_20)

# ClockPort appears in the public API (open() takes ports::ClockPort&), so
# broker_exec_ports is PUBLIC. The platform durability seam, JSON serializer, and
# warnings/sanitizers are implementation details → PRIVATE.
target_link_libraries(broker_exec_intentlog
  PUBLIC
    broker_exec_ports
  PRIVATE
    broker_exec_platform
    nlohmann_json::nlohmann_json
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_intentlog_tests
    intent_log_test.cpp
    sha256_test.cpp
  )
  target_link_libraries(broker_exec_intentlog_tests
    PRIVATE
      broker_exec_intentlog
      broker_exec_clock          # TestClock for deterministic timestamps
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_intentlog_tests COMMAND broker_exec_intentlog_tests)
endif()
