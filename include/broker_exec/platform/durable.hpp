#pragma once

#include <cstdio>

namespace broker_exec::platform {

// Flush the operating system's write buffers for an open file to stable
// storage. This is the portable durability primitive behind the write-ahead
// intent log (Story 1.5): the record must be on disk before the broker socket
// write, on every supported OS.
//
//   POSIX   -> fsync(2)
//   Windows -> _commit()
//
// Both operate on a C-runtime file descriptor, so the cross-platform seam is a
// single integer fd obtained from std::fopen + portable_fileno().
//
// NOTE (NFR-7, fsync honesty): durability is only as strong as the underlying
// storage honoring the flush. A startup probe / production checklist item
// verifies this on the target storage class; this function returns the OS
// result, it does not certify the hardware.
[[nodiscard]] bool durable_sync(int fd) noexcept;

// Portable wrapper over fileno()/_fileno(): returns the underlying file
// descriptor for an open C stream, or -1 on error.
[[nodiscard]] int portable_fileno(std::FILE* stream) noexcept;

}  // namespace broker_exec::platform
