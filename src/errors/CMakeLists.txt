# broker_exec::errors — the typed error taxonomy (Story 1.4, FR-25). Defines the
# stable broker-neutral ErrorCategory/SuggestedAction/Error vocabulary, the
# project-wide Result<T> alias (expected<T, Error>), and the single broker-text
# classification entry points. Pure C++20 stdlib; no OS APIs, no broker SDKs.

add_library(broker_exec_errors STATIC
  error.cpp
)
add_library(broker_exec::errors ALIAS broker_exec_errors)

target_include_directories(broker_exec_errors
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_errors PUBLIC cxx_std_20)

target_link_libraries(broker_exec_errors
  PRIVATE
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_errors_tests error_test.cpp)
  target_link_libraries(broker_exec_errors_tests
    PRIVATE
      broker_exec_errors
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_errors_tests COMMAND broker_exec_errors_tests)
endif()
