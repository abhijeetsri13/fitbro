#pragma once

#include <string_view>

namespace broker_exec::domain {

// Compile-time library version string (semantic version of the execution core).
[[nodiscard]] std::string_view library_version() noexcept;

}  // namespace broker_exec::domain
