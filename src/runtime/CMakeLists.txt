# broker_exec::runtime — the dispatch() send chokepoint (Story 1.9, FR-8/FR-10,
# NFR-1/NFR-2/NFR-3). THE SAFETY CENTERPIECE.
#
# Every broker mutation (place/modify/cancel/square-off) flows through ONE
# synchronous path: record-intent -> fsync -> [pre_send_barrier] -> send ->
# record-result|UNKNOWN, with NO thread hand-off between the fsync and the send.
# A timeout/network failure marks the order UNKNOWN and returns for reconciliation
# — it is NEVER blindly retried. The Dispatcher holds the runtime's ONLY
# BrokerPort reference, so no code path can send to a broker except through it
# (the chokepoint invariant; enforced structurally + by conventions.md + proven by
# the behavioral request_count() test).

# Story 1.10 adds two read-only resilience pieces to the SAME runtime module:
#   * unknown_resolver — resolves an UNKNOWN order against broker truth by a strict
#     match-key precedence (broker order_id > correlation token > attribute
#     corroboration > fail-closed). Read-only on the broker (fetch_orders only):
#     never a second fire. A NoMatch raises a Critical alert and stays UNKNOWN.
#   * unknown_pause — a process-wide latch: while ANY order is unresolved-UNKNOWN,
#     new risk-increasing entries are blocked; risk-reducing exits always pass.
add_library(broker_exec_runtime STATIC
  dispatcher.cpp
  unknown_resolver.cpp
  unknown_pause.cpp
)
add_library(broker_exec::runtime ALIAS broker_exec_runtime)

target_include_directories(broker_exec_runtime
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_runtime PUBLIC cxx_std_20)

# ports (BrokerPort/ClockPort/AlertSink/Ok) and domain (Order/OrderIntent) appear
# in the public Dispatcher API -> PUBLIC. Story 1.10's UnknownResolver is also a
# PUBLIC header and takes store::Store& and lifecycle::LifecycleEngine& in its
# constructor, so those two move from PRIVATE to PUBLIC (their headers are part of
# this module's public surface now). The rest stay implementation-only PRIVATE:
# intentlog (append/fsync), idempotency (reserve + canonical payload), errors
# (taxonomy / Result<>), warnings/sanitizers.
target_link_libraries(broker_exec_runtime
  PUBLIC
    broker_exec_ports
    broker_exec_domain
    broker_exec_store
    broker_exec_lifecycle
  PRIVATE
    broker_exec_intentlog
    broker_exec_idempotency
    broker_exec_errors
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  # The test ALSO links the fake broker (adversarial faults), the clock impls
  # (TestClock), and the idempotency target's SeededUuidGenerator (already pulled
  # via broker_exec_idempotency). It drives place/modify/cancel/square-off through
  # a temp-dir IntentLog + ":memory:" Store and asserts the safety invariants.
  add_executable(broker_exec_runtime_tests dispatcher_test.cpp)
  target_link_libraries(broker_exec_runtime_tests
    PRIVATE
      broker_exec_runtime
      broker_exec_domain
      broker_exec_errors
      broker_exec_intentlog
      broker_exec_store
      broker_exec_idempotency
      broker_exec_lifecycle
      broker_exec_fake_broker
      broker_exec_clock
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_runtime_tests COMMAND broker_exec_runtime_tests)

  # Story 1.10 — UNKNOWN resolver tests. Drives resolve()/resolve_all() against a
  # FakeBroker seeded with broker truth and an in-test RecordingAlertSink, proving
  # each precedence rung, the fail-closed NoMatch (alert raised, NOTHING sent —
  # request_count() advances only by the fetch read), and store adoption.
  add_executable(broker_exec_runtime_unknown_resolver_tests unknown_resolver_test.cpp)
  target_link_libraries(broker_exec_runtime_unknown_resolver_tests
    PRIVATE
      broker_exec_runtime
      broker_exec_domain
      broker_exec_store
      broker_exec_lifecycle
      broker_exec_fake_broker
      broker_exec_clock
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_runtime_unknown_resolver_tests
           COMMAND broker_exec_runtime_unknown_resolver_tests)

  # Story 1.10 — UNKNOWN process-wide pause gate tests. Pure value-type unit tests
  # (no broker/store), so they link only the runtime target.
  add_executable(broker_exec_runtime_unknown_pause_tests unknown_pause_test.cpp)
  target_link_libraries(broker_exec_runtime_unknown_pause_tests
    PRIVATE
      broker_exec_runtime
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_runtime_unknown_pause_tests
           COMMAND broker_exec_runtime_unknown_pause_tests)
endif()
