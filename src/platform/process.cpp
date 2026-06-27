#include "broker_exec/platform/process.hpp"

#include <cstdlib>

namespace broker_exec::platform {

int run_command(const std::string& command) {
#if defined(_WIN32)
  // cmd.exe (invoked by std::system) removes the leading and trailing double
  // quote when the command line both starts and ends with one. Wrapping the
  // entire command in an extra quote pair makes cmd strip THAT pair instead,
  // leaving the intended per-token quoting intact.
  const std::string wrapped = "\"" + command + "\"";
  return std::system(wrapped.c_str());
#else
  return std::system(command.c_str());
#endif
}

}  // namespace broker_exec::platform
