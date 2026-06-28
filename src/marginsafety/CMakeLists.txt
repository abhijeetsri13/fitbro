# broker_exec::marginsafety — the MARGIN SAFETY BUFFER (never trust the broker
# margin/basket-margin API at face value). Applies a fail-closed buffer OVER the
# reported requirement, sizes an untrusted multi-leg basket against the summed
# worst case (no leg benefit), and flags staleness near the 9:20/expiry SPAN
# boundary. Pure decision over integer Money paise. Depends inward only on domain
# (Money), errors (Result/Error), ports (Ok). NO new Conan dep. Mirrors
# src/modes/CMakeLists.txt.

add_library(broker_exec_marginsafety STATIC
  margin_buffer.cpp
)
add_library(broker_exec::marginsafety ALIAS broker_exec_marginsafety)

target_include_directories(broker_exec_marginsafety
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_marginsafety PUBLIC cxx_std_20)

# Money (domain), Result/Error (errors), and Ok (ports) all appear in this
# module's public header, so domain + errors + ports are PUBLIC. The
# warnings/sanitizers policy stays PRIVATE per convention.
target_link_libraries(broker_exec_marginsafety
  PUBLIC
    broker_exec_domain
    broker_exec_errors
    broker_exec_ports
  PRIVATE
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_marginsafety_tests margin_buffer_test.cpp)
  target_link_libraries(broker_exec_marginsafety_tests
    PRIVATE
      broker_exec_marginsafety
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_marginsafety_tests COMMAND broker_exec_marginsafety_tests)
endif()
