# broker_exec::lifecycle — the order lifecycle state machine (Story 1.8, FR-9).
# Owns each order's OrderState: a documented transition table, terminal-absorbing
# + forward-progressing (ordering-key) apply rules, and the parent/child fold for
# sliced orders (any child UNKNOWN -> parent PARTIALLY_PLACED). Applied ONLY by
# the main loop (single writer, NFR-2). Pure C++20 stdlib; no OS APIs, no #ifdef,
# no floating point.

add_library(broker_exec_lifecycle STATIC
  lifecycle.cpp
)
add_library(broker_exec::lifecycle ALIAS broker_exec_lifecycle)

target_include_directories(broker_exec_lifecycle
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_lifecycle PUBLIC cxx_std_20)

# domain types appear in the public API (Order/OrderState/BrokerView fields) ->
# PUBLIC. Everything else is an implementation detail -> PRIVATE: errors
# (Result<>/taxonomy availability for callers), idempotency (parent_of /
# is_child_ref to recover a parent from a child slice ref), warnings/sanitizers.
target_link_libraries(broker_exec_lifecycle
  PUBLIC
    broker_exec_domain
  PRIVATE
    broker_exec_errors
    broker_exec_idempotency
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_lifecycle_tests lifecycle_test.cpp)
  target_link_libraries(broker_exec_lifecycle_tests
    PRIVATE
      broker_exec_lifecycle
      broker_exec_domain
      broker_exec_idempotency
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_lifecycle_tests COMMAND broker_exec_lifecycle_tests)
endif()
