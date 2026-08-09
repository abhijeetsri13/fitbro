# broker_exec::idempotency — client-ref generation and idempotency (Story 1.7,
# FR-7, NFR-1/NFR-3). Mints "<strategy>-<sig8>-<uuid>" client-refs, detects a
# duplicate strategy signal via a deterministic SHA-256 signature + an in-memory
# index rebuilt from the intent log on boot, and composes with UNIQUE(client_ref)
# in the SQLite projection so "exactly one order" survives a restart. The
# deterministic child form "<parent>#<k>" is reserved here for the freeze-slicer
# (Story 2.9) and the lifecycle FSM (Story 1.8).
#
# UUIDs are generated via <random> (std::random_device seeding std::mt19937_64),
# never time-based — cross-platform, no OS APIs, no #ifdef.

add_library(broker_exec_idempotency STATIC
  idempotency.cpp
  uuid.cpp
)
add_library(broker_exec::idempotency ALIAS broker_exec_idempotency)

target_include_directories(broker_exec_idempotency
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  # The vendored SHA-256 header lives alongside the intentlog sources (it is not a
  # public header). We reuse it for the signal hash, so add that source dir to our
  # PRIVATE include path; the symbol comes from the linked broker_exec_intentlog.
  PRIVATE ${PROJECT_SOURCE_DIR}/src/intentlog)

target_compile_features(broker_exec_idempotency PUBLIC cxx_std_20)

# domain types appear in the public API (OrderIntent/Order) -> PUBLIC. Everything
# else is an implementation detail -> PRIVATE: errors (Result<>), intentlog
# (sha256 + IntentRecord), store (find_order + UNIQUE backstop), nlohmann_json
# (canonical-payload parse on rebuild), warnings/sanitizers.
target_link_libraries(broker_exec_idempotency
  PUBLIC
    broker_exec_domain
  PRIVATE
    broker_exec_errors
    broker_exec_intentlog
    broker_exec_store
    nlohmann_json::nlohmann_json
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_idempotency_tests idempotency_test.cpp)
  target_link_libraries(broker_exec_idempotency_tests
    PRIVATE
      broker_exec_idempotency
      broker_exec_domain
      broker_exec_errors
      broker_exec_intentlog
      broker_exec_store
      broker_exec_clock          # TestClock for the intent-log restart test
      nlohmann_json::nlohmann_json  # IMP-17: parse the payload projection back
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_idempotency_tests COMMAND broker_exec_idempotency_tests)
endif()
