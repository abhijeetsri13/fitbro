#pragma once

#include <cstdio>
#include <filesystem>

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

// Flush a DIRECTORY's entry to stable storage, so that a rename() into it
// survives a power loss.
//
// Syncing the renamed file is not enough. On POSIX the rename is a change to
// the *directory*, and an unsynced directory can come back after a crash still
// pointing at the old entry — or at no entry at all, losing a file whose
// contents were themselves durable. Anything that publishes by
// write-temp -> sync -> rename must sync the containing directory afterwards
// for the publish to be durable.
//
//   POSIX   -> open(dir, O_RDONLY) + fsync(2) + close
//   Windows -> not applicable, returns true
//
// The Windows return is an honest no-op, not a silent one: Win32 exposes no
// handle on which a directory's metadata can be flushed, and MoveFileEx already
// orders the rename's metadata write on NTFS. Callers get `true` because there
// is nothing further this platform can do, not because durability was verified.
[[nodiscard]] bool durable_sync_directory(const std::filesystem::path& dir) noexcept;

}  // namespace broker_exec::platform
