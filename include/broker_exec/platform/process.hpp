#pragma once

#include <string>

namespace broker_exec::platform {

// Run a shell command line, blocking until it completes, and return the value
// std::system reports (0 == success on every platform; non-zero otherwise). This
// is the portable seam over the one genuinely platform-specific wrinkle of
// std::system: cmd.exe strips the outer quotes of a command that BOTH begins and
// ends with a double quote, which silently mangles `"<exe>" arg "<path>"`. On
// Windows we wrap the whole command in an extra quote pair (the documented
// `cmd /c` workaround); POSIX shells pass it through unchanged.
//
// Used by the SIGKILL durability harness (Story 1.12) and, later, the
// process-per-account supervisor (FR-33). Keeping the OS quirk here is exactly
// why the platform layer exists.
[[nodiscard]] int run_command(const std::string& command);

}  // namespace broker_exec::platform
