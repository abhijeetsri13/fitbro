#include "broker_exec/platform/permissions.hpp"

#include <sys/stat.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace broker_exec::platform {

// This is the ONLY permitted site for OS `#ifdef` on file permissions (binding
// cross-platform convention): POSIX `chmod` mode bits vs the Windows CRT.

bool restrict_to_owner_file(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
  // HONEST WINDOWS LIMITATION: the CRT has no notion of POSIX owner/group/other
  // mode bits. `_wchmod` can only toggle the read-only file ATTRIBUTE — it
  // cannot grant owner-exclusive access or strip inherited ACEs. So this is
  // best-effort hardening only; true owner-only protection on Windows requires
  // a full ACL rewrite (SetNamedSecurityInfo: replace the DACL with a single
  // owner ACE, disable inheritance), which is deferred. We keep the file
  // owner-read/write and rely on the data dir living under a per-user profile
  // path. `path.c_str()` is `const wchar_t*` on Windows (path::value_type).
  return ::_wchmod(path.c_str(), _S_IREAD | _S_IWRITE) == 0;
#else
  // POSIX 0600 — owner read+write, nothing for group/other.
  return ::chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0;
#endif
}

bool restrict_to_owner_dir(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
  // Same limitation as restrict_to_owner_file: `_wchmod` on a directory does not
  // express an owner-only ACL and is effectively a no-op for access control on
  // Windows. Returned as a best-effort signal; real 0700-equivalent directory
  // ACLs are a documented Windows limitation, deferred to an ACL-based impl.
  return ::_wchmod(path.c_str(), _S_IREAD | _S_IWRITE) == 0;
#else
  // POSIX 0700 — owner read+write+execute (execute = traverse), nothing else.
  return ::chmod(path.c_str(), S_IRUSR | S_IWUSR | S_IXUSR) == 0;
#endif
}

}  // namespace broker_exec::platform
