# broker_exec::store — the SQLite read-model projection (Story 1.6, NFR-4).
# A queryable, crash-safe projection of the durable state derived from the
# write-ahead intent log (Story 1.5). WAL + synchronous=FULL on open; numbered
# transactional migrations behind a schema_version stamp; a corrupt/half-migrated
# projection is rebuilt from the intent log rather than refusing to start.
#
# Links broker_exec_domain PUBLIC (domain row types appear in the API) and
# broker_exec_errors + SQLite::SQLite3 PRIVATE (implementation detail; the public
# header forward-declares sqlite3 and never includes <sqlite3.h>).

add_library(broker_exec_store STATIC
  store.cpp
)
add_library(broker_exec::store ALIAS broker_exec_store)

target_include_directories(broker_exec_store
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_store PUBLIC cxx_std_20)

target_link_libraries(broker_exec_store
  PUBLIC
    broker_exec_domain
  PRIVATE
    broker_exec_errors
    SQLite::SQLite3
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_store_tests store_test.cpp)
  target_link_libraries(broker_exec_store_tests
    PRIVATE
      broker_exec_store
      broker_exec_errors
      SQLite::SQLite3
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_store_tests COMMAND broker_exec_store_tests)
endif()
