# broker_exec::domain — the pure core. No adapter, transport, or third-party
# broker SDK may ever be linked here (enforced by enforce_no_dependency()).

add_library(broker_exec_domain STATIC
  version.cpp
  enums.cpp
  money.cpp
  types.cpp
  redaction.cpp
)
add_library(broker_exec::domain ALIAS broker_exec_domain)

target_include_directories(broker_exec_domain
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_domain PUBLIC cxx_std_20)

target_link_libraries(broker_exec_domain
  PRIVATE
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_domain_tests value_types_test.cpp redaction_test.cpp)
  target_link_libraries(broker_exec_domain_tests
    PRIVATE
      broker_exec_domain
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_domain_tests COMMAND broker_exec_domain_tests)
endif()
