# broker_exec::reconcile — continuous reconciliation (Story 3.1, FR-11) +
# manual-intervention detection (Story 3.2, FR-12).
#
# 3.1: two structurally separated halves: the FETCH side (Reconciler) reads the
# broker only and produces an immutable ReconcileResult off the main loop; the
# APPLY side (ReconcileApplier) is the sole writer, folding a result into local
# state by reusing the lifecycle FSM and raising an alert + blocking new orders on
# a mismatch.
#
# 3.2: the ManualInterventionDetector CLASSIFIES a 3.1 ReconcileResult into named
# manual-intervention events (position closed/reduced manually, order cancelled
# manually), alerts on each, and reconciles the local position book to broker
# truth so a manually-closed position reads flat (the structural no-duplicate-exit
# guarantee).
#
# Depends INWARD only on domain/ports/errors (public API) and lifecycle (used
# inside the .cpp only). No transport/adapter/SDK dependency. No new Conan dep.

add_library(broker_exec_reconcile STATIC
  reconciler.cpp
  manual_intervention.cpp
)
add_library(broker_exec::reconcile ALIAS broker_exec_reconcile)

target_include_directories(broker_exec_reconcile
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_reconcile PUBLIC cxx_std_20)

# domain (Order/Trade/Position members of ReconcileResult), ports
# (FundsSnapshot/BrokerPort/AlertSink/ClockPort in the public API) and errors
# (Result<ReconcileResult> carries errors::Error) appear in the public headers,
# so they are PUBLIC. lifecycle is forward-declared in the header and only its
# apply()/BrokerView/is_terminal are used inside reconciler.cpp, so it stays
# PRIVATE. Warnings/sanitizers PRIVATE per convention.
target_link_libraries(broker_exec_reconcile
  PUBLIC
    broker_exec_domain
    broker_exec_ports
    broker_exec_errors
  PRIVATE
    broker_exec_lifecycle
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_reconcile_tests
    reconcile_test.cpp
    manual_intervention_test.cpp)
  target_link_libraries(broker_exec_reconcile_tests
    PRIVATE
      broker_exec_reconcile
      broker_exec_lifecycle     # build a LifecycleEngine + BrokerView in-test
      broker_exec_clock         # TestClock (deterministic ClockPort)
      broker_exec_fake_broker   # drive fetch() through a FakeBroker
      broker_exec_domain        # Order/Trade/Position/Quantity construction
      broker_exec_ports         # FundsSnapshot / AlertSink / AlertLevel
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_reconcile_tests COMMAND broker_exec_reconcile_tests)
endif()
