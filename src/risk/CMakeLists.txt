# broker_exec::risk — the pre-submission validation gate (Story 2.8, FR-6).
#
# The single non-bypassable checkpoint every order crosses before any broker
# mutation. It composes already-built modules (refdata::TradingCalendar) plus
# INJECTED predicates for modules not yet built (funds = 2.11, risk-engine = 2.10,
# slicer = 2.9, kill-switch = 3.8, UNKNOWN-pause runtime). Depends INWARD only on
# domain, errors, refdata, capabilities and ports — NO transport/adapter/SDK
# dependency.

add_library(broker_exec_risk STATIC
  validation_gate.cpp
  risk_engine.cpp
  funds_view.cpp
)
add_library(broker_exec::risk ALIAS broker_exec_risk)

target_include_directories(broker_exec_risk
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_risk PUBLIC cxx_std_20)

# domain (Instrument/Quantity/Price value members) and errors (Result<GateOutcome>
# carries errors::Error) appear in this module's public API, so they are PUBLIC.
# refdata (TradingCalendar::require_entry_allowed is called from the .cpp only),
# capabilities, ports and the warnings/sanitizers policy are implementation
# details and stay PRIVATE per convention.
target_link_libraries(broker_exec_risk
  PUBLIC
    broker_exec_domain
    broker_exec_errors
  PRIVATE
    broker_exec_refdata
    broker_exec_ports
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_risk_tests
    validation_gate_test.cpp
    risk_engine_test.cpp
    funds_view_test.cpp)
  target_link_libraries(broker_exec_risk_tests
    PRIVATE
      broker_exec_risk
      broker_exec_refdata    # build a TradingCalendar in-test (require_entry_allowed)
      broker_exec_clock      # TestClock (deterministic ClockPort: calendar + funds view)
      broker_exec_domain     # Instrument/Quantity/Price/Money construction + assertions
      broker_exec_ports      # ports::FundsSnapshot for the funds-view tests
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_risk_tests COMMAND broker_exec_risk_tests)
endif()
