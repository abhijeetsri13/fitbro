#include "broker_exec/platform/durable.hpp"

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
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

bool durable_sync_directory(const std::filesystem::path& dir) noexcept {
#if defined(_WIN32)
  // Win32 has no directory handle that can be flushed the way fsync(2) flushes
  // a POSIX directory fd. MoveFileEx already orders the rename's metadata write
  // on NTFS, so there is nothing left for this seam to do. Reported as success
  // because the operation is inapplicable, not because it was performed.
  (void)dir;
  return true;
#else
  // O_RDONLY is the portable way to obtain a directory fd; fsync(2) on it
  // commits the directory entry that a preceding rename() created.
  const int fd = ::open(dir.c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }
  const bool synced = ::fsync(fd) == 0;
  ::close(fd);
  return synced;
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
