# broker_exec::slicing — the freeze-quantity slicer (Story 2.9, FR-6, IBR-2).
# A PURE, deterministic function that fans an over-freeze parent OrderIntent into
# lot-aligned child intents under the exchange freeze ceiling, each with a
# deterministic "<parent>#<k>" client-ref. Depends inward ONLY on domain + errors
# (the child-ref format is reproduced inline to avoid pulling idempotency's
# store/SQLite deps); a test asserts parity with idempotency::child_ref. No OS
# APIs, no #ifdef, no floating point.

add_library(broker_exec_slicing STATIC
  freeze_slicer.cpp
)
add_library(broker_exec::slicing ALIAS broker_exec_slicing)

target_include_directories(broker_exec_slicing
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_slicing PUBLIC cxx_std_20)

# domain types appear in the public API (slice() takes OrderIntent/Instrument and
# returns a vector<OrderIntent>) and Error/Result are in the return type -> both
# PUBLIC. The warnings/sanitizers policy stays PRIVATE per convention. The
# production target deliberately does NOT link broker_exec_idempotency.
target_link_libraries(broker_exec_slicing
  PUBLIC
    broker_exec_domain
    broker_exec_errors
  PRIVATE
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  # The test ALSO links broker_exec_idempotency (and domain) to assert the inline
  # "<parent>#<k>" refs are parity-identical to idempotency::child_ref and are
  # recognized by is_child_ref / parent_of (AC-3 doc). Production code does not.
  add_executable(broker_exec_slicing_tests freeze_slicer_test.cpp)
  target_link_libraries(broker_exec_slicing_tests
    PRIVATE
      broker_exec_slicing
      broker_exec_domain
      broker_exec_idempotency
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_slicing_tests COMMAND broker_exec_slicing_tests)
endif()
