# A reusable INTERFACE target carrying the project's warning policy.
# Link it PRIVATE into every first-party target so the flags apply to our code
# but never leak into third-party dependency headers.

add_library(broker_exec_warnings INTERFACE)

if(MSVC)
  target_compile_options(broker_exec_warnings INTERFACE
    /W4
    /permissive-
    /WX)
  # We deliberately use portable ISO C stdio (fopen, etc.) for cross-platform
  # parity with POSIX. Suppress MSVC's non-standard "use *_s" deprecations so
  # /WX does not reject portable code.
  target_compile_definitions(broker_exec_warnings INTERFACE
    _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_WARNINGS)
else()
  target_compile_options(broker_exec_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wshadow
    -Wnon-virtual-dtor
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wnull-dereference
    -Wdouble-promotion)
endif()
