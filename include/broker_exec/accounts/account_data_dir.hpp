#pragma once

// broker_exec::accounts — the PER-ACCOUNT DATA DIRECTORY layout (Story 6.5b,
// FR-33, AC-1).
//
// WHY THIS EXISTS. Multi-account means one OS PROCESS PER ACCOUNT (ID-1). Two
// sibling processes that share a single intent log, SQLite file, token store,
// ledger or kill journal do not "mostly work" — they corrupt each other's
// order state and can produce duplicate orders, which is the one failure this
// library exists to prevent. So the layout is not a convention scattered across
// modules; it is OWNED here, in one place, and every per-account path is derived
// from one validated account id.
//
// ── THE COLLISION-PROOF PROPERTY (AC-1) ─────────────────────────────────────
// Distinct account ids MUST yield disjoint trees. The trap is string
// concatenation: with a two-level layout `root/<a>/<b>`, the pairs ("ab","c")
// and ("a","bc") collide. This type structurally cannot do that:
//   * exactly ONE directory level is derived from the id: `<data_root>/<id>`;
//   * the id is validated against `[a-z0-9_-]{1,64}` — so it contains no
//     path separator, no `.` (hence no `.` / `..`), no drive letter, no NUL and
//     no wildcard/reserved character;
//   * therefore two distinct ids are two distinct SIBLING directory names, and
//     no sibling name can be a path prefix of another.
//
// LOWERCASE ONLY, and that is a CORRECTNESS rule. NTFS and stock APFS/HFS+ are
// case-INSENSITIVE, so "AB" and "ab" are two distinct ids that open the SAME
// directory — two processes sharing one intent log and one SQLite file, which is
// exactly the corruption this type exists to prevent. Rejecting A-Z makes each
// id its own canonical form, so "distinct id ⇒ distinct directory" holds on
// every filesystem rather than only the case-sensitive ones.
//
// An account id is a NAME, never a path. Anything that does not validate is
// REJECTED (fail closed) — it is never sanitized into something "close enough",
// because silently rewriting `../../etc` into `etc` is how a path-traversal bug
// becomes a data-loss bug, and silently lowercasing `AB` would merge two
// accounts rather than refuse them.
//
// ── PERMISSIONS ─────────────────────────────────────────────────────────────
// The account directory holds the encrypted token store, so `ensure()` tightens
// it to owner-only (POSIX 0700) through the EXISTING platform permissions seam
// (`platform::restrict_to_owner_dir`, Story 2.2) — this module contains no
// `#ifdef` and no OS call of its own. The seam is INJECTABLE so tests can assert
// it was actually invoked; production wires the real one by default. A failure
// to tighten permissions FAILS the ensure() (fail closed): a token store in a
// world-readable directory is not an acceptable degraded mode.
//
// Conventions: no-throw across the boundary (`Result<T>`), std::filesystem for
// every path (never a hand-built "/" or "\\"), no OS API outside src/platform.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::accounts {

// The account-id charset and length bound (see the collision-proof note above).
inline constexpr std::size_t kMaxAccountIdLength = 64;

// Validate an account id: non-empty, at most `kMaxAccountIdLength` characters,
// every character in `[a-z0-9_-]` (LOWERCASE ONLY — see the case-insensitive
// filesystem note above), and not a Windows reserved device name (con, prn, aux,
// nul, com1-9, lpt1-9), because the id becomes a directory NAME on every
// platform. Any violation is a typed Validation Error naming the rule that
// failed (never the raw id echoed into a path). Fail closed: no normalization,
// no trimming, no lowercasing, no sanitizing.
[[nodiscard]] Result<ports::Ok> validate_account_id(std::string_view account_id);

// The injectable directory-permission seam. Returns true when the directory was
// successfully restricted to owner-only. Defaults to
// `platform::restrict_to_owner_dir` (POSIX 0700; best-effort on Windows, see
// that function's honest caveat).
using DirPermissionFn = std::function<bool(const std::filesystem::path&)>;

// The production seam: `platform::restrict_to_owner_dir`.
[[nodiscard]] DirPermissionFn default_dir_permission_fn();

// Owns the on-disk layout for ONE account. Value type (copyable/movable); holds
// no OS handle and performs no I/O until `ensure()` is called.
class AccountDataDir {
 public:
  // Build the layout for `account_id` under `data_root`. The id is validated
  // (see `validate_account_id`); an invalid id is REJECTED, never sanitized.
  // `perms` overrides the permission seam (tests inject a spy); an empty
  // std::function means "use the production seam".
  [[nodiscard]] static Result<AccountDataDir> create(std::filesystem::path data_root,
                                                     std::string account_id,
                                                     DirPermissionFn perms = {});

  [[nodiscard]] const std::string& account_id() const noexcept { return account_id_; }

  // The shared parent every account directory is a sibling in.
  [[nodiscard]] const std::filesystem::path& data_root() const noexcept { return data_root_; }

  // This account's private tree: `<data_root>/<account_id>`. The single
  // id-derived directory level (see the collision-proof note).
  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

  // ── The layout. These are the ONLY per-account file locations. ────────────
  // Every one of them is inside root(), so two accounts can never share a file.
  [[nodiscard]] std::filesystem::path intent_log() const;    // fsync'd write-ahead log (1.5)
  [[nodiscard]] std::filesystem::path store_db() const;      // SQLite WAL projection (1.6)
  [[nodiscard]] std::filesystem::path token_store() const;   // AES-256-GCM token blob (2.2)
  [[nodiscard]] std::filesystem::path ledger() const;        // hash-chained audit ledger (4.4)
  [[nodiscard]] std::filesystem::path kill_journal() const;  // kill-switch journal (3.x)

  // Create `data_root` and `root()` if missing and tighten `root()` to
  // owner-only via the permission seam. IDEMPOTENT: calling it on an existing,
  // already-tightened tree succeeds and changes nothing. Any creation failure,
  // an existing non-directory in the way, or a failed permission tightening is a
  // typed Error (fail closed).
  [[nodiscard]] Result<ports::Ok> ensure() const;

 private:
  AccountDataDir(std::filesystem::path data_root, std::string account_id, DirPermissionFn perms);

  std::filesystem::path data_root_;
  std::string account_id_;
  std::filesystem::path root_;
  DirPermissionFn perms_;
};

}  // namespace broker_exec::accounts
