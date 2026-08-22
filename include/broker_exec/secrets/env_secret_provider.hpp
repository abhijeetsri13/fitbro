#pragma once

// broker_exec::secrets::EnvSecretProvider — the environment-backed concrete
// `ports::SecretProvider` (Story 2.2, FR-35).
//
// Resolves a secret by its stable logical key (e.g. "kite.api_key") from the
// process environment, behind an injected env seam — the same pattern the config
// loader uses, so tests inject a deterministic map and never read the real
// process environment, and there is exactly one `std::getenv` call site.
//
// A missing key surfaces as a typed `errors::Error` (never a token-shaped
// message, and never the secret value). The returned value is sensitive; callers
// MUST scrub it before logging (see domain::scrub).
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "broker_exec/ports/secret_provider.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::secrets {

// The environment seam: maps an env key to its value, or nullopt if unset.
// Tests inject a deterministic lambda; production uses default_env_lookup().
using EnvLookup = std::function<std::optional<std::string>(std::string_view)>;

// The production environment seam — the single `std::getenv` call site in this
// module (portable C++ stdlib; no OS API / no `#ifdef`).
[[nodiscard]] EnvLookup default_env_lookup();

// Concrete env-backed secret provider. The logical key IS the environment
// variable name resolved through the seam.
class EnvSecretProvider final : public ports::SecretProvider {
 public:
  explicit EnvSecretProvider(EnvLookup env = default_env_lookup());

  // Resolve `key` via the seam. Returns the value on success; a missing key is a
  // Validation Error naming the (non-secret) key, never echoing any value.
  [[nodiscard]] Result<std::string> get(std::string_view key) const override;

 private:
  EnvLookup env_;
};

}  // namespace broker_exec::secrets
