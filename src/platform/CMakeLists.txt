# broker_exec::platform — the cross-platform OS seam. This is the ONLY module
# permitted to use platform `#ifdef`s / OS APIs (fsync vs _commit, chmod vs
# ACLs, AF_UNIX vs localhost TCP, process control). Everything else depends on
# this portable surface so domain/business logic stays OS-agnostic.

add_library(broker_exec_platform STATIC
  durable.cpp
  permissions.cpp
  process.cpp
)
add_library(broker_exec::platform ALIAS broker_exec_platform)

target_include_directories(broker_exec_platform
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_platform PUBLIC cxx_std_20)

target_link_libraries(broker_exec_platform
  PRIVATE
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_platform_tests durable_test.cpp)
  target_link_libraries(broker_exec_platform_tests
    PRIVATE
      broker_exec_platform
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_platform_tests COMMAND broker_exec_platform_tests)
endif()
