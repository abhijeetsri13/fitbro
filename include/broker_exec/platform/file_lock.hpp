#pragma once

// broker_exec::platform — the cross-process ADVISORY FILE LOCK (Story 6.5b,
// FR-33, architecture COH-2/IBR-4).
//
// WHY THIS EXISTS. The multi-account deployment is one OS PROCESS PER ACCOUNT
// (ID-1). Those sibling processes share one reference-data cache — the
// instrument master and the trading calendar are identical per
// (broker, segment, trading_date), so N processes downloading N copies at 09:00
// is both wasteful and a rate-limit hazard. Exactly ONE sibling must win the
// download; the others must read the artifact it wrote. Arbitrating that across
// PROCESSES needs an OS primitive, so it lives here — the only module permitted
// OS `#ifdef`s / OS APIs.
//
// ── THE PRIMITIVE ───────────────────────────────────────────────────────────
// Ownership is conferred by an ATOMIC CREATE-EXCLUSIVE of the lock path:
//   POSIX   -> open(O_CREAT | O_EXCL)      (fails with EEXIST if it exists)
//   Windows -> CreateFileW(..., CREATE_NEW) (fails with ERROR_FILE_EXISTS)
// Both are atomic at the filesystem level: of N concurrent callers racing on
// one path, EXACTLY ONE create succeeds. (C++23's std::ios::noreplace would
// express this portably; we target C++20, hence the seam.)
//
// The lock file carries a human-readable pid + ISO-8601 timestamp + nonce
// payload so an operator finding a stuck lock can see who took it and when.
//
// ── THE GUARANTEE (read this before changing anything) ──────────────────────
//  1. NEVER TWO WINNERS ON A LIVE LOCK. A holder is a caller whose
//     create-exclusive succeeded. Two of those cannot coexist: the OS serializes
//     the create, and the loser is told "exists" — it gets a typed Error, never
//     a lock and never a crash.
//  2. STALE TAKEOVER IS ITSELF ARBITRATED ATOMICALLY, AND THE NAME IS TAKEN
//     BEFORE ANYTHING IS JUDGED. A process can die (SIGKILL, power loss) leaving
//     its lock file behind forever, so a lock whose file mtime is older than
//     `staleness` MAY be taken over. The takeover does NOT "delete then create"
//     (two contenders could both delete, and one could delete a lock the other
//     had just legitimately re-created). It runs in this EXACT order:
//
//       a. RENAME the stale file to a private per-caller name. rename(2) /
//          MoveFileExW fail with "source not found" for every contender but the
//          first, so exactly ONE caller gets past this step.
//       b. IMMEDIATELY create-exclusive our own lock at the now-free lock path.
//          This closes the window in which a third process could occupy the name
//          while we were still deciding. If the create says "exists", someone
//          beat us to the free name: we ABORT, drop the file we claimed, and
//          NEVER restore it (restoring would overwrite the new owner's lock).
//       c. ONLY NOW re-check the age of the file we claimed. If it turns out to
//          have been FRESH (a legitimate holder acquired it between our staleness
//          observation and step (a)), we UNDO: remove our own lock (nonce-checked,
//          so we can only ever delete our own file) and rename the victim's file
//          back. The restore's error_code is INSPECTED — a failed restore is
//          reported as a typed Internal Error naming the residue, never silently
//          swallowed.
//
//     Doing (b) before (c) is the load-bearing detail. The earlier
//     check-then-restore ordering left the lock path UNOCCUPIED while the mtime
//     was re-read, so a fourth process could create a lock there and then have it
//     silently replaced by the aborting process's restore (fs::rename replaces).
//     Holding the name throughout makes that impossible.
//  3. THE STALENESS WINDOW IS A LIVENESS ASSUMPTION, NOT A CORRECTNESS ESCAPE.
//     Taking over a lock is safe only if no live holder can leave its file
//     untouched for longer than `staleness`. Keep it generous (default 10 min —
//     far longer than any refdata download) and treat shrinking it as a
//     correctness change. `staleness <= 0` disables takeover entirely. A holder
//     that legitimately runs long should call `touch()` to refresh its mtime
//     rather than rely on the window being wide enough.
//  4. A STALE LOCK WITH AN UNREADABLE PAYLOAD IS NEVER TAKEN OVER. Age alone does
//     not license a takeover: the file must also PARSE as one of our lock files.
//     Arbitrary bytes sitting at the lock path may not be a lock at all, so we
//     fail closed with a typed Error and leave it for a human. (Deleting it is a
//     deliberate operator action, not something this code guesses at.)
//  5. RELEASE NEVER DELETES SOMEONE ELSE'S LOCK. `release()` re-reads the file
//     and removes it ONLY if the nonce is still ours. Destruction releases
//     (best-effort, no-throw). `still_ours()` exposes the same check so a holder
//     can re-verify immediately before it publishes anything — the residual
//     TOCTOU window between the check and the publish is the caller's to keep
//     short, and it cannot be closed by an advisory lock.
//
// This is an ADVISORY lock: it binds only processes that agree to consult it. It
// is not a mandatory OS lock on the protected artifact, and it is NOT a mutex for
// threads within one process (it is a cross-PROCESS primitive; within a process,
// two FileLocks on one path behave exactly like two processes would).
//
// NOTE ON CLOCKS: staleness compares the lock file's mtime against
// `std::filesystem::file_time_type::clock::now()` — the filesystem's own clock —
// because the mtime is stamped by the OS, not by us, and the two must be in the
// same clock domain. This is the one place the injected `ClockPort` cannot be
// used; the payload's human-facing timestamp is a diagnostic breadcrumb only and
// is never used for a decision.
//
// Conventions: no-throw across the boundary (Result<T>), std::filesystem for all
// paths, no float. Errors are typed values.

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "broker_exec/result.hpp"

namespace broker_exec::platform {

// Default staleness window for a takeover: deliberately generous (see guarantee
// 3 above). A refdata download that takes ten minutes is already an incident.
inline constexpr std::chrono::seconds kDefaultLockStaleness{600};

// The lock file's payload — operator-facing diagnostics plus the ownership
// nonce. Redaction-safe: a pid, a timestamp and a synthetic id, never a secret.
struct LockPayload {
  long long pid = 0;         // the acquiring process id
  std::string acquired_at;   // ISO-8601 UTC, "YYYY-MM-DDTHH:MM:SSZ"
  std::string nonce;         // unique per acquisition: "<pid>-<epoch_ns>-<counter>"
};

// The on-disk payload text (exactly this shape; tests pin it):
//
//   broker-exec-lock v1\n
//   pid=<pid>\n
//   acquired_at=<ISO-8601 UTC>\n
//   nonce=<nonce>\n
//
[[nodiscard]] std::string format_lock_payload(const LockPayload& payload);

// Parse the payload text back. Unknown keys are ignored (forward compatible);
// a missing pid/acquired_at/nonce, or an unparseable pid, yields nullopt — an
// unreadable lock file is AMBIGUOUS and callers must fail closed on it.
[[nodiscard]] std::optional<LockPayload> parse_lock_payload(std::string_view text);

class FileLock;

// Try to acquire the cross-process lock at `lock_path`.
//
//   * Parent directories are created if missing (idempotent).
//   * Success  -> a held FileLock (RAII: released on destruction).
//   * Held by a live sibling -> Transient Error (RetrySafe). NOT a crash.
//   * Stale (mtime older than `staleness`) -> ONE caller takes over atomically;
//     every other contender gets a Transient Error and retries later.
//   * Any ambiguity (unreadable lock file, failed create, lost read-back
//     verification) -> a typed Error. FAIL CLOSED: the caller does not hold it.
//
// `staleness <= 0` disables takeover (a stale lock then blocks forever, which is
// the safest possible posture and is what tests use to pin the "no steal" case).
[[nodiscard]] Result<FileLock> try_acquire_file_lock(
    const std::filesystem::path& lock_path,
    std::chrono::seconds staleness = kDefaultLockStaleness);

// RAII guard over an acquired lock file. Move-only: a lock has exactly one
// owner, and moving transfers the responsibility to release it.
class FileLock {
 public:
  // A default-constructed FileLock holds nothing (held() == false). Useful as an
  // "empty" slot; only try_acquire_file_lock() can produce a held one.
  FileLock() noexcept = default;

  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  FileLock(FileLock&& other) noexcept;
  FileLock& operator=(FileLock&& other) noexcept;

  // Releases (best-effort, no-throw). Never throws out of a destructor.
  ~FileLock();

  [[nodiscard]] bool held() const noexcept { return held_; }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  // The payload THIS lock wrote (its nonce is the ownership token).
  [[nodiscard]] const LockPayload& payload() const noexcept { return payload_; }

  // Is the lock file on disk STILL ours? Re-reads it and compares the nonce.
  // False when we never held it, when the file is gone, when it cannot be read
  // or parsed, or when it carries someone else's nonce — i.e. false is always
  // the fail-closed answer. A holder that is about to PUBLISH something the lock
  // protects should call this immediately beforehand and abandon the publish if
  // it returns false. It cannot close the window entirely (an advisory lock has
  // no such power), it makes it small and makes the theft observable.
  [[nodiscard]] bool still_ours() const noexcept;

  // Refresh the lock file's modification time so a legitimately long-running
  // holder is not mistaken for a corpse and taken over (guarantee 3). Only
  // touches a file that is STILL ours; returns false otherwise, or if the
  // timestamp could not be written. No-throw. Call it around any step that can
  // approach the staleness window (a slow reference-data download).
  bool touch() noexcept;

  // Release the lock: remove the lock file IFF it still carries our nonce (see
  // guarantee 5). Idempotent, no-throw. After this, held() == false.
  void release() noexcept;

 private:
  friend Result<FileLock> try_acquire_file_lock(const std::filesystem::path&,
                                                std::chrono::seconds);

  FileLock(std::filesystem::path lock_path, LockPayload payload) noexcept;

  std::filesystem::path path_;
  LockPayload payload_;
  bool held_ = false;
};

// ── TEST SEAM: deterministic fault injection ────────────────────────────────
//
// The takeover protocol's failure branches (another process occupies the freed
// name; the file we claimed turns out to be live; our lock is replaced between
// the create and the read-back) are CONCURRENT by construction — reproducing
// them needs a second process to interleave at an exact instant, which no
// single-process test can arrange. Leaving them untested is worse than exposing
// a hook, so the three interleaving points are routed through one.
//
// This mirrors the established `adapters/fake` FaultConfig pattern: production
// NEVER sets a hook (the default is unset and the call is a null check), and the
// hook cannot make the protocol UNSAFE — it runs BETWEEN steps and can only
// arrange the world so that a step reports a failure it was already able to
// report. NOT thread-safe, and not intended to be: it is a test seam.
enum class LockFaultPoint {
  AfterCreate,       // a create-exclusive at the lock path just succeeded
  AfterStaleClaim,   // the atomic rename-claim of a stale lock just succeeded
};

// `lock_path` is the lock being acquired; `claim_path` is the private name the
// stale file was moved to (empty for AfterCreate).
using LockFaultHook = std::function<void(LockFaultPoint lock_fault_point,
                                         const std::filesystem::path& lock_path,
                                         const std::filesystem::path& claim_path)>;

// Install (or, with an empty function, remove) the fault hook. TEST ONLY.
void set_lock_fault_hook(LockFaultHook hook);

}  // namespace broker_exec::platform
