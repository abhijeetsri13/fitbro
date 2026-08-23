# broker_exec::secrets — env SecretProvider + AES-256-GCM token store at rest
# (Story 2.2, FR-35, SE-2/SE-5, SEC-1). EnvSecretProvider resolves credentials
# through an injected env seam; TokenStore encrypts per-account tokens with
# OpenSSL AES-256-GCM (random IV per record, auth tag verified, fail-closed on
# tamper) and persists 0600 blobs in a 0700 dir via the platform seam. Depends
# inward only (ports/errors/platform/domain) + OpenSSL::Crypto; no adapters.

add_library(broker_exec_secrets STATIC
  env_secret_provider.cpp
  token_store.cpp
)
add_library(broker_exec::secrets ALIAS broker_exec_secrets)

target_include_directories(broker_exec_secrets
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_secrets PUBLIC cxx_std_20)

# ports (SecretProvider) and the Result<T>/Error vocabulary appear in the public
# headers (TokenStore/EnvSecretProvider return Result<T> over the port), so they
# are PUBLIC. OpenSSL and the platform perms seam are implementation details;
# warnings/sanitizers stay PRIVATE. No domain dependency: secrets errors are
# hand-curated secret-free, so nothing here includes redaction.hpp / calls scrub().
target_link_libraries(broker_exec_secrets
  PUBLIC
    broker_exec_ports
    broker_exec_errors
  PRIVATE
    OpenSSL::Crypto
    broker_exec_platform
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_secrets_tests secrets_test.cpp)
  target_link_libraries(broker_exec_secrets_tests
    PRIVATE
      broker_exec_secrets
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_secrets_tests COMMAND broker_exec_secrets_tests)
endif()
