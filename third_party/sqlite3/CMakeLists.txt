# Vendored SQLite amalgamation (public domain, v3.45.3). Built as a STATIC
# library and exposed under the canonical CMake target name `SQLite::SQLite3`,
# so consuming modules (store, idempotency) need no change. Hermetic and
# byte-identical on every platform — no shared library / runtime DLL resolution,
# which is exactly the cross-platform fragility we are eliminating.

add_library(sqlite3_vendored STATIC sqlite3.c)
add_library(SQLite::SQLite3 ALIAS sqlite3_vendored)

target_include_directories(sqlite3_vendored
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Lean, safe build options. WAL + serialized threading are what the engine uses.
target_compile_definitions(sqlite3_vendored PRIVATE
  SQLITE_THREADSAFE=1
  SQLITE_DEFAULT_MEMSTATUS=0
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_DQS=0)

# Do NOT apply the project's -Werror policy to third-party C; silence its warnings.
if(MSVC)
  target_compile_options(sqlite3_vendored PRIVATE /w)
else()
  target_compile_options(sqlite3_vendored PRIVATE -w)
endif()

# POSIX needs pthreads + dl for a thread-safe build; harmless/no-op on Windows.
find_package(Threads REQUIRED)
target_link_libraries(sqlite3_vendored PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
