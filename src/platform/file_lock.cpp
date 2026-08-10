#include "broker_exec/platform/file_lock.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "broker_exec/errors/error.hpp"

// ── The ONLY OS divergence in this file (binding cross-platform convention) ──
// Atomic create-exclusive + the current pid. Everything else is std::filesystem.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace broker_exec::platform {

namespace fs = std::filesystem;

namespace {

using errors::ErrorCategory;
using errors::make_error;

// Outcome of the atomic create-exclusive seam. `Exists` is NOT a failure — it is
// the normal "someone else holds it" answer the policy above is built on.
enum class CreateOutcome { Created, Exists, Failed };

// ── OS SEAM 1/2: atomic create-exclusive ────────────────────────────────────
// Create `path` ONLY if it does not exist and write `payload` into it durably.
// Of N racing callers exactly one gets `Created`; the rest get `Exists`. A
// partially written lock file is removed rather than left as a corpse.
[[nodiscard]] CreateOutcome create_exclusive(const fs::path& path, const std::string& payload) {
#if defined(_WIN32)
  // CREATE_NEW == "fail if it already exists", atomically. Share mode 0 keeps
  // anyone else out for the (very short) window we hold the handle.
  const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD err = ::GetLastError();
    if (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS) {
      return CreateOutcome::Exists;
    }
    return CreateOutcome::Failed;
  }

  DWORD written = 0;
  const BOOL wrote =
      ::WriteFile(handle, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr);
  const BOOL flushed = ::FlushFileBuffers(handle);
  ::CloseHandle(handle);

  if (wrote == FALSE || flushed == FALSE || written != static_cast<DWORD>(payload.size())) {
    std::error_code ec;
    fs::remove(path, ec);  // never leave a half-written lock behind
    return CreateOutcome::Failed;
  }
  return CreateOutcome::Created;
#else
  // O_CREAT|O_EXCL == "fail with EEXIST if it already exists", atomically. 0600:
  // the lock is owner-only like every other file this library writes.
  const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    return (errno == EEXIST) ? CreateOutcome::Exists : CreateOutcome::Failed;
  }

  bool ok = true;
  std::size_t offset = 0;
  while (offset < payload.size()) {
    const ssize_t n = ::write(fd, payload.data() + offset, payload.size() - offset);
    if (n < 0) {
      if (errno == EINTR) {
        continue;  // interrupted before writing anything — retry
      }
      ok = false;
      break;
    }
    if (n == 0) {
      ok = false;
      break;
    }
    offset += static_cast<std::size_t>(n);
  }
  if (ok) {
    ok = (::fsync(fd) == 0);
  }
  ::close(fd);

  if (!ok) {
    std::error_code ec;
    fs::remove(path, ec);  // never leave a half-written lock behind
    return CreateOutcome::Failed;
  }
  return CreateOutcome::Created;
#endif
}

// ── OS SEAM 2/2: the current process id ─────────────────────────────────────
[[nodiscard]] long long current_pid() noexcept {
#if defined(_WIN32)
  return static_cast<long long>(::GetCurrentProcessId());
#else
  return static_cast<long long>(::getpid());
#endif
}

// Zero-pad a non-negative integer to `width` digits.
[[nodiscard]] std::string pad(long long value, std::size_t width) {
  std::string digits = std::to_string(value);
  while (digits.size() < width) {
    digits.insert(digits.begin(), '0');
  }
  return digits;
}

// "YYYY-MM-DDTHH:MM:SSZ" for the current wall instant, via std::chrono only (no
// localtime/strftime, no OS call). This is a DIAGNOSTIC breadcrumb for a human
// reading a stuck lock file — never an input to any decision, which is why it
// does not (and cannot: the platform seam sits below ports) use the ClockPort.
[[nodiscard]] std::string iso_utc_now() {
  const auto now = std::chrono::system_clock::now();
  const auto secs = std::chrono::floor<std::chrono::seconds>(now);
  const auto days = std::chrono::floor<std::chrono::days>(secs);
  const std::chrono::year_month_day ymd{std::chrono::sys_days{days}};

  const long long since_midnight = (secs - days).count();
  const long long hour = since_midnight / 3600;
  const long long minute = (since_midnight / 60) % 60;
  const long long second = since_midnight % 60;

  return pad(static_cast<long long>(static_cast<int>(ymd.year())), 4) + "-" +
         pad(static_cast<long long>(static_cast<unsigned>(ymd.month())), 2) + "-" +
         pad(static_cast<long long>(static_cast<unsigned>(ymd.day())), 2) + "T" + pad(hour, 2) +
         ":" + pad(minute, 2) + ":" + pad(second, 2) + "Z";
}

// A per-acquisition ownership token: pid + epoch-nanoseconds + a process-local
// counter. Two acquisitions can never collide, even in the same nanosecond in
// the same process (the counter), or across processes (the pid).
[[nodiscard]] std::string make_nonce(long long pid) {
  static std::atomic<unsigned long long> counter{0};
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  return std::to_string(pid) + "-" + std::to_string(ns) + "-" +
         std::to_string(counter.fetch_add(1, std::memory_order_relaxed) + 1);
}

// Read a whole (small) file. nullopt when it cannot be read — which callers MUST
// treat as ambiguous and fail closed on, never as "empty".
[[nodiscard]] std::optional<std::string> read_text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Is the file at `path` older than the staleness window?
//   nullopt -> the mtime could not be read: AMBIGUOUS, callers fail closed.
//   false   -> fresh (or `staleness <= 0`, which disables takeover entirely, or
//              the mtime is in the FUTURE, which is clock skew and never stale).
[[nodiscard]] std::optional<bool> path_is_stale(const fs::path& path,
                                                std::chrono::seconds staleness) {
  if (staleness <= std::chrono::seconds::zero()) {
    return false;  // takeover disabled by configuration — the safest posture
  }
  std::error_code ec;
  const fs::file_time_type mtime = fs::last_write_time(path, ec);
  if (ec) {
    return std::nullopt;
  }
  // The mtime is stamped by the FILESYSTEM, so it must be compared against the
  // filesystem's own clock (see the header's clock note).
  const fs::file_time_type now = fs::file_time_type::clock::now();
  if (mtime > now) {
    return false;  // future mtime (skew/copy) — refuse to call it stale
  }
  return (now - mtime) > staleness;
}

// Remove a lock file ONLY if it still carries `nonce`. The single place that is
// ever allowed to delete a lock file, so "never delete someone else's lock" is
// enforced in one spot rather than at every call site.
void remove_if_ours(const fs::path& path, const std::string& nonce) {
  const std::optional<std::string> text = read_text(path);
  if (!text.has_value()) {
    return;
  }
  const std::optional<LockPayload> parsed = parse_lock_payload(*text);
  if (!parsed.has_value() || parsed->nonce != nonce) {
    return;
  }
  std::error_code ec;
  fs::remove(path, ec);
}

// The TEST SEAM (see the header). Unset in production; the call below is a null
// check on the hot path.
LockFaultHook g_lock_fault_hook;  // NOLINT(*-avoid-non-const-global-variables)

void fire_fault(LockFaultPoint point, const fs::path& lock_path, const fs::path& claim_path) {
  if (g_lock_fault_hook) {
    g_lock_fault_hook(point, lock_path, claim_path);
  }
}

// Take over a STALE lock, in the exact order documented as guarantee 2:
//   (a) atomically claim the stale file by renaming it away — one winner;
//   (b) IMMEDIATELY create-exclusive our own lock at the freed name, so no third
//       process can occupy it while we are still deciding;
//   (c) only THEN judge whether the file we claimed was really stale, undoing
//       (nonce-checked) if it was not.
// Returns nullopt when this caller now holds the lock; a typed Error otherwise.
[[nodiscard]] std::optional<errors::Error> takeover_stale_lock(const fs::path& lock_path,
                                                               std::chrono::seconds staleness,
                                                               const std::string& payload_text,
                                                               const std::string& our_nonce) {
  static std::atomic<unsigned long long> claim_counter{0};

  // A private, per-caller destination name. Built with path::operator+= so no
  // narrow/wide conversion of the caller's path is ever performed.
  fs::path claim_path = lock_path;
  claim_path += ".stale-" + std::to_string(current_pid()) + "-" +
                std::to_string(claim_counter.fetch_add(1, std::memory_order_relaxed) + 1);

  // ── (a) THE ARBITER ──
  // rename() is atomic on the SOURCE name: of N contenders racing to move the
  // same stale file away, exactly one succeeds; the others fail with "source not
  // found". We never delete-then-create, precisely so two contenders can never
  // both believe they cleared the way.
  std::error_code ec;
  fs::rename(lock_path, claim_path, ec);
  if (ec) {
    return make_error(ErrorCategory::Transient,
                      "file lock: stale takeover lost the race to another process");
  }
  fire_fault(LockFaultPoint::AfterStaleClaim, lock_path, claim_path);

  // ── (b) TAKE THE NAME IMMEDIATELY ──
  // Nothing is judged until we own the lock path. Deciding first would leave the
  // name unoccupied, and a process that created a lock there could then have it
  // silently replaced by our restore (fs::rename replaces its target).
  const CreateOutcome outcome = create_exclusive(lock_path, payload_text);
  if (outcome != CreateOutcome::Created) {
    // Someone occupied the freed name before us. Their lock is real and current:
    // we drop the corpse we claimed and NEVER restore over them.
    std::error_code drop_ec;
    fs::remove(claim_path, drop_ec);
    if (outcome == CreateOutcome::Exists) {
      return make_error(ErrorCategory::Transient,
                        "file lock: re-acquired by another process during takeover");
    }
    return make_error(ErrorCategory::Internal,
                      "file lock: cannot create lock file after takeover");
  }
  fire_fault(LockFaultPoint::AfterCreate, lock_path, claim_path);

  // ── (c) ONLY NOW judge what we claimed ──
  // The mtime survives a rename, so a "fresh" answer here means a legitimate
  // holder acquired between our staleness observation and step (a) and we just
  // moved a LIVE lock. Undo it: give up our own lock (nonce-checked, so we can
  // only ever delete our own file) and put the victim's file back.
  const std::optional<bool> was_stale = path_is_stale(claim_path, staleness);
  if (!was_stale.has_value() || !*was_stale) {
    remove_if_ours(lock_path, our_nonce);

    std::error_code exists_ec;
    if (fs::exists(lock_path, exists_ec) || exists_ec) {
      // A third party took the name in the instant we gave it up. Restoring would
      // clobber a live lock, so we do not. The victim's lock is lost; it will
      // discover that through still_ours()/release()'s nonce check.
      std::error_code drop_ec;
      fs::remove(claim_path, drop_ec);
      return make_error(ErrorCategory::Internal,
                        "file lock: stale takeover aborted, and the displaced lock could NOT be "
                        "restored (the lock path was re-taken)");
    }

    std::error_code restore_ec;
    fs::rename(claim_path, lock_path, restore_ec);
    if (restore_ec) {
      // The one genuinely ambiguous outcome the protocol can produce. It is
      // reported LOUDLY (Internal ⇒ RaiseAlert) and names the residue, because a
      // silently swallowed restore failure leaves an operator with a live holder
      // whose lock file has vanished and a stray `.stale-*` file beside it.
      return make_error(ErrorCategory::Internal,
                        "file lock: stale takeover aborted and the displaced lock could NOT be "
                        "restored (a claimed-lock '.stale-' file remains beside it)");
    }
    return make_error(ErrorCategory::Transient,
                      "file lock: stale takeover aborted - the lock was live and has been restored");
  }

  fs::remove(claim_path, ec);  // best-effort: the corpse is ours to bury
  return std::nullopt;
}

}  // namespace

std::string format_lock_payload(const LockPayload& payload) {
  return "broker-exec-lock v1\npid=" + std::to_string(payload.pid) +
         "\nacquired_at=" + payload.acquired_at + "\nnonce=" + payload.nonce + "\n";
}

std::optional<LockPayload> parse_lock_payload(std::string_view text) {
  LockPayload payload;
  bool have_pid = false;
  bool have_time = false;
  bool have_nonce = false;

  std::size_t begin = 0;
  while (begin <= text.size()) {
    const std::size_t end = text.find('\n', begin);
    std::string_view line =
        text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);  // CRLF tolerance
    }

    const std::size_t eq = line.find('=');
    if (eq != std::string_view::npos) {
      const std::string_view key = line.substr(0, eq);
      const std::string_view value = line.substr(eq + 1);
      if (key == "pid") {
        long long parsed = 0;
        const char* const first = value.data();
        const char* const last = value.data() + value.size();
        const auto [ptr, ec] = std::from_chars(first, last, parsed);
        if (ec != std::errc{} || ptr != last) {
          return std::nullopt;  // an unparseable pid is a corrupt lock file
        }
        payload.pid = parsed;
        have_pid = true;
      } else if (key == "acquired_at") {
        payload.acquired_at = std::string(value);
        have_time = !payload.acquired_at.empty();
      } else if (key == "nonce") {
        payload.nonce = std::string(value);
        have_nonce = !payload.nonce.empty();
      }
      // Unknown keys are ignored on purpose (forward compatible).
    }

    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }

  if (!have_pid || !have_time || !have_nonce) {
    return std::nullopt;
  }
  return payload;
}

FileLock::FileLock(std::filesystem::path lock_path, LockPayload payload) noexcept
    : path_(std::move(lock_path)), payload_(std::move(payload)), held_(true) {}

FileLock::FileLock(FileLock&& other) noexcept
    : path_(std::move(other.path_)), payload_(std::move(other.payload_)), held_(other.held_) {
  other.held_ = false;  // exactly one owner: the moved-from lock releases nothing
}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
  if (this != &other) {
    release();  // give up whatever we held first
    path_ = std::move(other.path_);
    payload_ = std::move(other.payload_);
    held_ = other.held_;
    other.held_ = false;
  }
  return *this;
}

FileLock::~FileLock() { release(); }

bool FileLock::still_ours() const noexcept {
  if (!held_) {
    return false;
  }
  try {
    const std::optional<std::string> text = read_text(path_);
    if (!text.has_value()) {
      return false;  // gone or unreadable — fail closed
    }
    const std::optional<LockPayload> parsed = parse_lock_payload(*text);
    return parsed.has_value() && parsed->nonce == payload_.nonce;
  } catch (...) {
    return false;  // any ambiguity is "not ours"
  }
}

bool FileLock::touch() noexcept {
  try {
    if (!still_ours()) {
      return false;  // never extend the life of a lock we do not own
    }
    std::error_code ec;
    fs::last_write_time(path_, fs::file_time_type::clock::now(), ec);
    return !ec;
  } catch (...) {
    return false;
  }
}

void FileLock::release() noexcept {
  if (!held_) {
    return;
  }
  held_ = false;

  // Best-effort and STRICTLY no-throw (this runs from a destructor). The removal
  // goes through remove_if_ours(): if a stale-takeover took the file while we
  // were working, it now belongs to someone else and deleting it would hand a
  // third party a lock nobody owns.
  try {
    remove_if_ours(path_, payload_.nonce);
  } catch (...) {
    // Swallow: a failed release degrades to a lock that goes stale and is later
    // taken over. Throwing out of a destructor is never acceptable.
  }
}

void set_lock_fault_hook(LockFaultHook hook) { g_lock_fault_hook = std::move(hook); }

Result<FileLock> try_acquire_file_lock(const std::filesystem::path& lock_path,
                                       std::chrono::seconds staleness) {
  if (lock_path.empty()) {
    return fail(make_error(ErrorCategory::Internal, "file lock: empty lock path"));
  }

  // Idempotent parent creation: the shared cache root may not exist on first run.
  // Every filesystem query uses the error_code overload — the throwing ones would
  // escape this no-throw boundary on a permission error or a race.
  std::error_code ec;
  const fs::path parent = lock_path.parent_path();
  if (!parent.empty()) {
    fs::create_directories(parent, ec);
    std::error_code dir_ec;
    const bool parent_ready = fs::is_directory(parent, dir_ec);
    if (dir_ec || !parent_ready) {
      return fail(make_error(ErrorCategory::Internal, "file lock: cannot create lock directory"));
    }
  }

  LockPayload payload;
  payload.pid = current_pid();
  payload.acquired_at = iso_utc_now();
  payload.nonce = make_nonce(payload.pid);
  const std::string text = format_lock_payload(payload);

  const CreateOutcome outcome = create_exclusive(lock_path, text);
  if (outcome == CreateOutcome::Failed) {
    return fail(make_error(ErrorCategory::Internal, "file lock: cannot create lock file"));
  }

  if (outcome == CreateOutcome::Exists) {
    const std::optional<bool> stale = path_is_stale(lock_path, staleness);
    if (!stale.has_value()) {
      // Cannot read the mtime -> we cannot prove the lock is dead. Fail closed.
      return fail(make_error(ErrorCategory::Internal,
                             "file lock: lock file age unreadable - failing closed"));
    }
    if (!*stale) {
      return fail(make_error(ErrorCategory::Transient,
                             "file lock: already held by another process"));
    }

    // GUARANTEE 4: age alone does not license a takeover. The file must also
    // PARSE as one of our lock files — arbitrary bytes at the lock path may not
    // be a lock at all (a half-written file, an unrelated artifact, a restore
    // gone wrong), and taking those over would be a guess. Fail closed and leave
    // it for a human; deleting it is a deliberate operator action.
    const std::optional<std::string> existing = read_text(lock_path);
    if (!existing.has_value() || !parse_lock_payload(*existing).has_value()) {
      return fail(make_error(ErrorCategory::Internal,
                             "file lock: stale lock file has an unreadable payload - refusing to "
                             "take it over; an operator must remove it"));
    }

    const std::optional<errors::Error> takeover_error =
        takeover_stale_lock(lock_path, staleness, text, payload.nonce);
    if (takeover_error.has_value()) {
      return fail(*takeover_error);  // we did NOT win the takeover — never force it
    }
    // takeover_stale_lock() created our lock as part of step (b).
  } else {
    fire_fault(LockFaultPoint::AfterCreate, lock_path, fs::path{});
  }

  // READ-BACK VERIFICATION (guarantee 5). The create was atomic, but a concurrent
  // stale-takeover could in principle have replaced the file the instant after.
  // We only claim the lock if the bytes on disk are still OURS. This runs on BOTH
  // paths — plain create and takeover.
  const std::optional<std::string> back = read_text(lock_path);
  if (!back.has_value()) {
    return fail(make_error(ErrorCategory::Internal,
                           "file lock: cannot verify lock file after create - failing closed"));
  }
  const std::optional<LockPayload> parsed = parse_lock_payload(*back);
  if (!parsed.has_value() || parsed->nonce != payload.nonce) {
    return fail(make_error(ErrorCategory::Transient,
                           "file lock: lost the lock to a concurrent takeover"));
  }

  return FileLock(lock_path, std::move(payload));
}

}  // namespace broker_exec::platform
