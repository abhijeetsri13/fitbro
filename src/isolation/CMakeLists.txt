# broker_exec::isolation — per-strategy virtual position book (Story 6.4, FR-32).
# Pure in-memory isolation kept ALONGSIDE the broker's single real position: each
# strategy gets a separate virtual book (positions/tags/P&L) keyed by strategy_id,
# so strategies sharing an account never interfere; a global limit backstops the
# COMBINED exposure and strategies stop independently. Depends inward only on
# domain (Money/Quantity/Side), errors (Result/Error), and ports (Ok). No I/O, no
# clock, no threads. Mirrors src/modes/CMakeLists.txt. NO new Conan dep.

add_library(broker_exec_isolation STATIC
  strategy_book.cpp
)
add_library(broker_exec::isolation ALIAS broker_exec_isolation)

target_include_directories(broker_exec_isolation
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_isolation PUBLIC cxx_std_20)

# Position/FlattenLeg use domain::Money/Quantity/Side and the public API returns
# Result<ports::Ok>, so domain + errors + ports are PUBLIC. The warnings/sanitizers
# policy stays PRIVATE per convention.
target_link_libraries(broker_exec_isolation
  PUBLIC
    broker_exec_domain
    broker_exec_errors
    broker_exec_ports
  PRIVATE
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_isolation_tests strategy_book_test.cpp)
  target_link_libraries(broker_exec_isolation_tests
    PRIVATE
      broker_exec_isolation
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_isolation_tests COMMAND broker_exec_isolation_tests)
endif()
