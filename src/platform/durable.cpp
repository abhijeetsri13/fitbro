#include "broker_exec/platform/durable.hpp"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace broker_exec::platform {

int portable_fileno(std::FILE* stream) noexcept {
  if (stream == nullptr) {
    return -1;
  }
#if defined(_WIN32)
  return ::_fileno(stream);
#else
  return ::fileno(stream);
#endif
}

bool durable_sync(int fd) noexcept {
  if (fd < 0) {
    return false;
  }
#if defined(_WIN32)
  return ::_commit(fd) == 0;
#else
  return ::fsync(fd) == 0;
#endif
}

}  // namespace broker_exec::platform
