#include "broker_exec/secrets/env_secret_provider.hpp"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::secrets {

using ::broker_exec::errors::ErrorCategory;
using ::broker_exec::errors::make_error;

EnvLookup default_env_lookup() {
  // The single std::getenv call site in the module — the production seam. This
  // is portable C++ standard library (no OS API / no #ifdef); MSVC's *_s
  // deprecation is suppressed project-wide via _CRT_SECURE_NO_WARNINGS.
  return [](std::string_view key) -> std::optional<std::string> {
    const std::string key_str(key);
    const char* value = std::getenv(key_str.c_str());  // NOLINT(concurrency-mt-unsafe)
    if (value == nullptr) {
      return std::nullopt;
    }
    return std::string(value);
  };
}

EnvSecretProvider::EnvSecretProvider(EnvLookup env) : env_(std::move(env)) {}

Result<std::string> EnvSecretProvider::get(std::string_view key) const {
  if (auto value = env_(key)) {
    return std::move(*value);
  }
  // The key name is a logical, non-secret identifier (e.g. "kite.api_key"); the
  // secret VALUE is never present here and never put into the message. Missing
  // credentials are an operator-fixable input problem -> Validation/DoNotRetry.
  std::string msg = "secret not found for key '";
  msg += key;
  msg += '\'';
  return fail(make_error(ErrorCategory::Validation, std::move(msg)));
}

}  // namespace broker_exec::secrets
