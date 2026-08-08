# broker_exec::accounts — multi-account process wiring (Story 6.5b, FR-33).
#
# Two collaborators for the "one OS process per account" deployment (ID-1):
#   * AccountDataDir    — the per-account on-disk layout (intent log, sqlite,
#                         token store, ledger, kill journal) under ONE validated
#                         id-derived directory, created idempotently and
#                         tightened to 0700 through the platform perms seam.
#                         Distinct ids are provably disjoint trees.
#   * SharedRefdataCache— the (broker, segment, trading_date)-keyed reference-data
#                         cache SHARED by every sibling process, guarded by the
#                         cross-process file lock in `platform`. One process wins
#                         the download; the rest read its atomically published
#                         artifact.
#
# It depends INWARD only: errors (Result/Error) and ports (Ok) are PUBLIC because
# they appear in the headers; `platform` is PRIVATE — the file lock, the perms
# seam and durable_sync are implementation details, so no caller inherits an OS
# dependency. It deliberately does NOT link `refdata`: composition happens through
# refdata's injected `std::function<Result<std::string>()>` fetch seams, so this
# module never touches InstrumentMaster/TradingCalendar internals. NO new Conan dep.

add_library(broker_exec_accounts STATIC
  account_data_dir.cpp
  shared_refdata_cache.cpp
)
add_library(broker_exec::accounts ALIAS broker_exec_accounts)

target_include_directories(broker_exec_accounts
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_accounts PUBLIC cxx_std_20)

target_link_libraries(broker_exec_accounts
  PUBLIC
    broker_exec_errors
    broker_exec_ports
  PRIVATE
    broker_exec_platform
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  # The suite links the REAL refdata classes (and broker_exec_clock's TestClock)
  # to prove the composition smoke test drives InstrumentMaster/TradingCalendar
  # through their genuine fetch seams — a private stand-in would only prove the
  # stand-in works. This is a TEST-only dependency; the library above has none.
  add_executable(broker_exec_accounts_tests
    account_data_dir_test.cpp
    shared_refdata_cache_test.cpp)
  target_link_libraries(broker_exec_accounts_tests
    PRIVATE
      broker_exec_accounts
      broker_exec_platform   # holds the lock directly to simulate a busy sibling
      broker_exec_refdata    # InstrumentMaster / TradingCalendar composition smoke
      broker_exec_clock      # TestClock (deterministic ClockPort)
      broker_exec_domain     # domain::Instrument assertions
      broker_exec_errors
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_accounts_tests COMMAND broker_exec_accounts_tests)
endif()
