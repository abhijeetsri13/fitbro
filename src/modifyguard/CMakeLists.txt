# broker_exec::modifyguard — the MODIFY-ORDER SAFETY GUARD (a pure decision).
# Gates an order-modify against the order's last-reconciled fill state so a modify
# can never silently cancel a working/partially-filled remainder (Kite: a quantity
# modify sets the TOTAL, not the pending qty, and can flip a partial to CANCELLED).
# Pure: no I/O, no throw, integer qty only. Depends inward only on domain
# (OrderState), errors, and ports. NO new Conan dep.

add_library(broker_exec_modifyguard STATIC
  modify_guard.cpp
)
add_library(broker_exec::modifyguard ALIAS broker_exec_modifyguard)

target_include_directories(broker_exec_modifyguard
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_modifyguard PUBLIC cxx_std_20)

# domain::OrderState appears in this module's public header, so domain is PUBLIC.
# errors + ports are the shared decision/result vocabulary kept PUBLIC to mirror
# the sibling modes module. The warnings/sanitizers policy stays PRIVATE.
target_link_libraries(broker_exec_modifyguard
  PUBLIC
    broker_exec_domain
    broker_exec_errors
    broker_exec_ports
  PRIVATE
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_modifyguard_tests modify_guard_test.cpp)
  target_link_libraries(broker_exec_modifyguard_tests
    PRIVATE
      broker_exec_modifyguard
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_modifyguard_tests COMMAND broker_exec_modifyguard_tests)
endif()
