#pragma once

// broker_exec::platform — file/directory permission hardening (Story 2.2, AC-1).
//
// The encrypted token store must persist its key/blob files as mode 0600 inside
// a 0700 directory. POSIX mode bits and Windows ACLs diverge sharply, so this
// lives behind the platform seam — the ONLY module permitted OS `#ifdef`s.
// Business logic (the `secrets` module) calls this portable surface and never
// touches chmod/ACLs directly.
//
// Both calls are best-effort and report success/failure as a bool (no throw);
// callers decide whether a failure to tighten permissions is fatal. See the
// honest Windows caveat in permissions.cpp — the CRT cannot express owner-only
// ACLs, so on Windows these are partial hardening, not a guarantee.

#include <filesystem>

namespace broker_exec::platform {

// Restrict a file to owner read+write only (POSIX 0600). Returns true on
// success. Best-effort on Windows (see permissions.cpp).
[[nodiscard]] bool restrict_to_owner_file(const std::filesystem::path& path) noexcept;

// Restrict a directory to owner read+write+execute only (POSIX 0700). Returns
// true on success. Best-effort on Windows (see permissions.cpp).
[[nodiscard]] bool restrict_to_owner_dir(const std::filesystem::path& path) noexcept;

}  // namespace broker_exec::platform
