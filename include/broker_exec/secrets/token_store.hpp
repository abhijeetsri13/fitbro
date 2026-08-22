#pragma once

// broker_exec::secrets::TokenStore — AES-256-GCM token encryption at rest
// (Story 2.2, AC-1, SE-2/SE-5, SEC-1).
//
// Persists per-account secrets (session/access tokens) encrypted at rest. The
// 256-bit key is NOT co-located with the ciphertext (SEC-1): it is fetched from
// an injected `ports::SecretProvider` by an account-scoped key name, while the
// ciphertext lives under a data directory. Every record gets a fresh random
// 96-bit IV; the on-disk layout is:
//
//     IV (12 bytes) ‖ ciphertext (len(plaintext)) ‖ GCM tag (16 bytes)
//
// `load()` verifies the GCM auth tag and FAILS CLOSED on any mismatch (tamper,
// truncation, or wrong key) — it never returns unauthenticated plaintext.
//
// NO-THROW: all fallible calls return `Result<T>` (`expected<T, Error>`); errors
// are values. OpenSSL `EVP_CIPHER_CTX` is owned by a RAII guard freed on every
// path. Key bytes are zeroed after use. No secret material ever appears in an
// Error message or log.
//
// Cross-platform: paths via std::filesystem; OS permission bits via the
// platform seam. No OS APIs / `#ifdef` here (OpenSSL is portable).

#include <filesystem>
#include <string>
#include <string_view>

#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/ports/secret_provider.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::secrets {

class TokenStore {
 public:
  // `key_source` supplies the per-account 256-bit key (looked up by an
  // account-scoped key name) and must outlive this store. `data_dir` is the root
  // under which `<account>/<name>.enc` blobs are written.
  TokenStore(const ports::SecretProvider& key_source, std::filesystem::path data_dir);

  // Encrypt `plaintext` for (`account`, `name`) and persist it. Creates the
  // account directory (0700) and writes the blob file (0600) via the platform
  // seam. Returns ok() on success.
  [[nodiscard]] Result<ports::Ok> save(std::string_view account, std::string_view name,
                                       std::string_view plaintext) const;

  // Read and decrypt the blob for (`account`, `name`). Fails closed (Error) if
  // the file is missing/short or the GCM tag does not verify.
  [[nodiscard]] Result<std::string> load(std::string_view account, std::string_view name) const;

 private:
  // Resolve the per-account 256-bit key bytes from the SecretProvider.
  [[nodiscard]] Result<std::string> resolve_key(std::string_view account) const;

  const ports::SecretProvider* key_source_;
  std::filesystem::path data_dir_;
};

}  // namespace broker_exec::secrets
