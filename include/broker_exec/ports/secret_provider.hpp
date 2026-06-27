#pragma once

// broker_exec::ports::SecretProvider — the abstract secret-retrieval seam.
//
// Credentials/API keys/tokens are fetched by key through this port, never read
// from ambient env vars or files directly by business logic. A concrete
// provider (env, OS keychain, secrets file, vault) is injected at composition
// time. Keeping this abstract means the core never hard-codes a secret source
// and tests inject fakes. Abstract only this story; concrete providers land
// later.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <string>
#include <string_view>

#include "broker_exec/result.hpp"

namespace broker_exec::ports {

// Abstract secret store, keyed by a stable logical name (e.g. "kite.api_key").
// Header-only, pure-virtual.
class SecretProvider {
 public:
  virtual ~SecretProvider() = default;

  // Resolve a secret by key. Returns the secret value on success. A missing key
  // or backend failure is surfaced as an Error (never a token-shaped message).
  // The returned value is sensitive — callers MUST NOT log it.
  [[nodiscard]] virtual Result<std::string> get(std::string_view key) const = 0;
};

}  // namespace broker_exec::ports
