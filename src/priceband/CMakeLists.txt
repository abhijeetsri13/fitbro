# broker_exec::priceband — the PRE-SUBMISSION CIRCUIT/LPP PRICE-BAND VALIDATOR.
# Validates an order's price(s) against the instrument's dynamic circuit/LPP/DPR
# band BEFORE submission: blocks an out-of-band ENTRY (it would be exchange-
# rejected "price out of LPP range") and CLAMPS an out-of-band protective EXIT to
# the band edge so it still survives — an exit is never blocked. Pure: no I/O, no
# throw, integer Money paise only (no float). Depends inward only on domain
# (Side/OrderType/Money), errors, and ports. NO new Conan dep.

add_library(broker_exec_priceband STATIC
  band_check.cpp
)
add_library(broker_exec::priceband ALIAS broker_exec_priceband)

target_include_directories(broker_exec_priceband
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_priceband PUBLIC cxx_std_20)

# domain (Side/OrderType/Money) + errors/ports (the Result/Error/Ok decision and
# gate vocabulary) appear in this module's public header, so all three are PUBLIC
# (mirrors the sibling modes/modifyguard modules). warnings/sanitizers PRIVATE.
target_link_libraries(broker_exec_priceband
  PUBLIC
    broker_exec_domain
    broker_exec_errors
    broker_exec_ports
  PRIVATE
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_priceband_tests band_check_test.cpp)
  target_link_libraries(broker_exec_priceband_tests
    PRIVATE
      broker_exec_priceband
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_priceband_tests COMMAND broker_exec_priceband_tests)
endif()
