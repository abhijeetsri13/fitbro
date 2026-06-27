# broker_exec::refdata — the instrument-master lifecycle (Story 2.6, FR-3).
#
# Parses the per-broker instrument master (Kite's /instruments CSV, injected via
# a fetch_csv seam so the module stays transport-free), date-versioned-caches it,
# resolves symbol -> current token + lot/tick/freeze/expiry, and gates safe-start
# on freshness (a stale/undownloaded master blocks trading with a typed DataStale
# Error). Depends INWARD only on domain (Instrument/Price/Quantity), ports
# (ClockPort) and errors — NO transport/adapter/SDK dependency.

add_library(broker_exec_refdata STATIC
  instrument_master.cpp
)
add_library(broker_exec::refdata ALIAS broker_exec_refdata)

target_include_directories(broker_exec_refdata
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_refdata PUBLIC cxx_std_20)

# domain::Instrument, ports::Ok/ClockPort and errors::Error/Result all appear in
# this module's public API (parse_instruments_csv / InstrumentMaster), so those
# dependencies are PUBLIC. Warnings/sanitizers stay PRIVATE per convention.
target_link_libraries(broker_exec_refdata
  PUBLIC
    broker_exec_domain
    broker_exec_ports
    broker_exec_errors
  PRIVATE
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_refdata_tests instrument_master_test.cpp)
  target_link_libraries(broker_exec_refdata_tests
    PRIVATE
      broker_exec_refdata
      broker_exec_clock      # TestClock (deterministic ClockPort for tests)
      broker_exec_domain     # Instrument/Price/Quantity assertions
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_refdata_tests COMMAND broker_exec_refdata_tests)
endif()
