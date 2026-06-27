# Opt-in sanitizer support (gcc/clang only; the CI matrix builds ASan+UBSan and
# TSan variants). MSVC is excluded — its sanitizer story differs and the
# canonical sanitizer verification is the Linux CI job.
#
#   -DBROKER_EXEC_SANITIZER=address,undefined
#   -DBROKER_EXEC_SANITIZER=thread

set(BROKER_EXEC_SANITIZER "" CACHE STRING
  "Comma-separated sanitizers to enable (gcc/clang): address,undefined or thread")

add_library(broker_exec_sanitizers INTERFACE)

if(BROKER_EXEC_SANITIZER AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
  message(STATUS "Sanitizers enabled: ${BROKER_EXEC_SANITIZER}")
  target_compile_options(broker_exec_sanitizers INTERFACE
    -fsanitize=${BROKER_EXEC_SANITIZER}
    -fno-omit-frame-pointer
    -g)
  target_link_options(broker_exec_sanitizers INTERFACE
    -fsanitize=${BROKER_EXEC_SANITIZER})
elseif(BROKER_EXEC_SANITIZER)
  message(WARNING "BROKER_EXEC_SANITIZER ignored on ${CMAKE_CXX_COMPILER_ID}")
endif()
