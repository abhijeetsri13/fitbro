#include "broker_exec/platform/file_lock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "broker_exec/errors/error.hpp"

namespace fs = std::filesystem;

using broker_exec::Result;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::SuggestedAction;
using broker_exec::platform::FileLock;
using broker_exec::platform::format_lock_payload;
using broker_exec::platform::kDefaultLockStaleness;
using broker_exec::platform::LockFaultHook;
using broker_exec::platform::LockFaultPoint;
using broker_exec::platform::LockPayload;
using broker_exec::platform::parse_lock_payload;
using broker_exec::platform::set_lock_fault_hook;
using broker_exec::platform::try_acquire_file_lock;

namespace {

// A unique temp directory, cleaned up on destruction (RAII) — mirrors the
// pattern the refdata tests use.
struct TempDir {
  fs::path path;
  TempDir() {
    std::random_device rd;
    path = fs::temp_directory_path() /
           ("brexec_filelock_" + std::to_string(rd()) + "_" + std::to_string(rd()));
    fs::create_directories(path);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

[[nodiscard]] std::string read_all(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Backdate a file's mtime by `age` — the ONLY way this suite manipulates time.
// No sleeping against the wall clock anywhere in these tests.
void backdate(const fs::path& path, std::chrono::seconds age) {
  std::error_code ec;
  const fs::file_time_type now = fs::file_time_type::clock::now();
  fs::last_write_time(path, now - age, ec);
  REQUIRE(!ec);
}

void write_text(const fs::path& path, std::string_view text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// Install a fault hook for the duration of a scope. The hook is the ONLY way to
// reach the takeover protocol's concurrent branches deterministically (see the
// TEST SEAM note in file_lock.hpp), and it MUST be cleared however the test
// leaves — a leaked hook would corrupt every later test in the binary.
struct ScopedLockFault {
  explicit ScopedLockFault(LockFaultHook hook) { set_lock_fault_hook(std::move(hook)); }
  ScopedLockFault(const ScopedLockFault&) = delete;
  ScopedLockFault& operator=(const ScopedLockFault&) = delete;
  ~ScopedLockFault() { set_lock_fault_hook({}); }
};

// A payload that is well-formed but belongs to somebody else.
[[nodiscard]] std::string foreign_payload(const std::string& nonce) {
  LockPayload other;
  other.pid = 999999;
  other.acquired_at = "2026-08-09T09:15:00Z";
  other.nonce = nonce;
  return format_lock_payload(other);
}

// Create a lock file that is STALE and well-formed — a plausible corpse left by
// a process that was SIGKILLed. Age alone is not enough to license a takeover
// (guarantee 4): the payload must parse too.
void plant_stale_lock(const fs::path& path, std::chrono::seconds age) {
  write_text(path, foreign_payload("corpse-nonce"));
  backdate(path, age);
}

}  // namespace

TEST_CASE("acquire creates the lock file with a pid + ISO-timestamp payload", "[platform][lock]") {
  const TempDir dir;
  const fs::path lock = dir.path / "refdata.lock";

  Result<FileLock> held = try_acquire_file_lock(lock);
  REQUIRE(held.has_value());
  CHECK(held.value().held());
  CHECK(held.value().path() == lock);
  REQUIRE(fs::exists(lock));

  // The payload shape is a contract: an operator staring at a stuck lock must be
  // able to see WHO took it and WHEN.
  const std::string text = read_all(lock);
  CHECK(text.rfind("broker-exec-lock v1\n", 0) == 0);
  CHECK(text.find("pid=") != std::string::npos);
  CHECK(text.find("acquired_at=") != std::string::npos);
  CHECK(text.find("nonce=") != std::string::npos);

  const auto parsed = parse_lock_payload(text);
  REQUIRE(parsed.has_value());
  CHECK(parsed->pid > 0);
  CHECK(parsed->pid == held.value().payload().pid);
  CHECK(parsed->nonce == held.value().payload().nonce);
  // ISO-8601 UTC "YYYY-MM-DDTHH:MM:SSZ" — fixed width, fixed separators.
  REQUIRE(parsed->acquired_at.size() == 20);
  CHECK(parsed->acquired_at[4] == '-');
  CHECK(parsed->acquired_at[7] == '-');
  CHECK(parsed->acquired_at[10] == 'T');
  CHECK(parsed->acquired_at[13] == ':');
  CHECK(parsed->acquired_at[16] == ':');
  CHECK(parsed->acquired_at.back() == 'Z');
}

TEST_CASE("acquire creates missing parent directories", "[platform][lock]") {
  const TempDir dir;
  const fs::path lock = dir.path / "nested" / "deeper" / "refdata.lock";

  Result<FileLock> held = try_acquire_file_lock(lock);
  REQUIRE(held.has_value());
  CHECK(fs::exists(lock));
}

TEST_CASE("a second acquisition on a live lock fails with a typed Error, never a crash",
          "[platform][lock]") {
  const TempDir dir;
  const fs::path lock = dir.path / "refdata.lock";

  Result<FileLock> first = try_acquire_file_lock(lock);
  REQUIRE(first.has_value());

  // Two FileLock objects on one path behave exactly like two processes would —
  // that is what the cross-process primitive guarantees.
  Result<FileLock> second = try_acquire_file_lock(lock);
  REQUIRE_FALSE(second.has_value());
  CHECK(second.error().category == ErrorCategory::Transient);
  CHECK(second.error().action == SuggestedAction::RetrySafe);

  // The winner still holds it, and the file is untouched.
  CHECK(first.value().held());
  CHECK(fs::exists(lock));
}

TEST_CASE("release removes the lock file and the path can be re-acquired", "[platform][lock]") {
  const TempDir dir;
  const fs::path lock = dir.path / "refdata.lock";

  Result<FileLock> first = try_acquire_file_lock(lock);
  REQUIRE(first.has_value());
  first.value().release();
  CHECK_FALSE(first.value().held());
  CHECK_FALSE(fs::exists(lock));

  // release() is idempotent.
  first.value().release();
  CHECK_FALSE(first.value().held());

  Result<FileLock> second = try_acquire_file_lock(lock);
  REQUIRE(second.has_value());
  CHECK(fs::exists(lock));
  CHECK(second.value().payload().nonce != first.value().payload().nonce);
}

TEST_CASE("RAII: destruction releases the lock", "[platform][lock]") {
  const TempDir dir;
  const fs::path lock = dir.path / "refdata.lock";

  {
    Result<FileLock> held = try_acquire_file_lock(lock);
    REQUIRE(held.has_value());
    REQUIRE(fs::exists(lock));
  }  // <- destructor runs here

  CHECK_FALSE(fs::exists(lock));
  Result<FileLock> again = try_acquire_file_lock(lock);
  CHECK(again.has_value());
}

TEST_CASE("move transfers ownership: the moved-from lock releases nothing", "[platform][lock]") {
  const TempDir dir;
  const fs::path lock = dir.path / "refdata.lock";

  Result<FileLock> held = try_acquire_file_lock(lock);
  REQUIRE(held.has_value());
  const std::string nonce = held.value().payload().nonce;

  {
    FileLock moved = std::move(held).value();
    CHECK(moved.held());
    CHECK(moved.payload().nonce == nonce);
    CHECK(fs::exists(lock));  // still held by `moved`
  }  // <- `moved` releases here

  CHECK_FALSE(fs::exists(lock));
}

TEST_CASE("a FRESH lock is never stolen, however small the staleness window",
          "[platform][lock]") {
  const TempDir dir;
  const fs::path lock = dir.path / "refdata.lock";

  Result<FileLock> first = try_acquire_file_lock(lock, std::chrono::seconds{1});
  REQUIRE(first.has_value());
  const std::string nonce = first.value().payload().nonce;

  Result<FileLock> thief = try_acquire_file_lock(lock, std::chrono::seconds{1});
  REQUIRE_FALSE(thief.has_value());
  CHECK(thief.error().category == ErrorCategory::Transient);

  // The lock file still belongs to the original holder.
  const auto parsed = parse_lock_payload(read_all(lock));
  REQUIRE(parsed.has_value());
  CHECK(parsed->nonce == nonce);
}

TEST_CASE("a STALE lock is taken over exactly once, and only by one winner",
          "[platform][lock]") {
  const TempDir dir;
  const fs::path lock = dir.path / "refdata.lock";

  // Simulate a process that died holding the lock: the file is there, nobody is.
  Result<FileLock> dead = try_acquire_file_lock(lock, kDefaultLockStaleness);
  REQUIRE(dead.has_value());
  const std::string dead_nonce = dead.value().payload().nonce;
  backdate(lock, std::chrono::seconds{3600});  // one hour old

  // A 10-minute staleness window: the corpse qualifies for takeover.
  Result<FileLock> winner = try_acquire_file_lock(lock, std::chrono::minutes{10});
  REQUIRE(winner.has_value());
  CHECK(winner.value().payload().nonce != dead_nonce);

  // The lock file now carries the WINNER's nonce.
  auto parsed = parse_lock_payload(read_all(lock));
  REQUIRE(parsed.has_value());
  CHECK(parsed->nonce == winner.value().payload().nonce);

  // NEVER TWO WINNERS: the freshly taken-over lock is live, so a second
  // contender is refused even though the window is the same.
  Result<FileLock> loser = try_acquire_file_lock(lock, std::chrono::minutes{10});
  REQUIRE_FALSE(loser.has_value());
  CHECK(loser.error().category == ErrorCategory::Transient);

  // And the dead holder's own release must NOT delete the new owner's lock.
  dead.value().release();
  REQUIRE(fs::exists(lock));
  parsed = parse_lock_payload(read_all(lock));
  REQUIRE(parsed.has_value());
  CHECK(parsed->nonce == winner.value().payload().nonce);

  // No stale-claim debris is left behind in the directory.
  for (const auto& entry : fs::directory_iterator(dir.path)) {
    CHECK(entry.path().filename().string().find(".stale-") == std::string::npos);
  }
}

TEST_CASE("staleness <= 0 disables takeover entirely (fail closed)", "[platform][lock]") {
  const TempDir dir;
  const fs::path lock = dir.path / "refdata.lock";

  Result<FileLock> dead = try_acquire_file_lock(lock);
  REQUIRE(dead.has_value());
  backdate(lock, std::chrono::hours{24});  // ancient

  Result<FileLock> thief = try_acquire_file_lock(lock, std::chrono::seconds{0});
  REQUIRE_FALSE(thief.has_value());
  CHECK(thief.error().category == ErrorCategory::Transient);

  Result<FileLock> negative = try_acquire_file_lock(lock, std::chrono::seconds{-5});
  CHECK_FALSE(negative.has_value());
}

TEST_CASE("a lock file just inside the staleness window is not stale", "[platform][lock]") {
  const TempDir dir;
  const fs::path lock = dir.path / "refdata.lock";

  Result<FileLock> dead = try_acquire_file_lock(lock);
  REQUIRE(dead.has_value());
  backdate(lock, std::chrono::seconds{60});

  // 10-minute window, 60-second-old lock ⇒ still live.
  Result<FileLock> thief = try_acquire_file_lock(lock, std::chrono::minutes{10});
  CHECK_FALSE(thief.has_value());

  // 30-second window, 60-second-old lock ⇒ stale, taken over.
  Result<FileLock> winner = try_acquire_file_lock(lock, std::chrono::seconds{30});
  CHECK(winner.has_value());
}

TEST_CASE("a lock with a FUTURE mtime is never treated as stale (clock skew)",
          "[platform][lock]") {
  const TempDir dir;
  const fs::path lock = dir.path / "refdata.lock";

  Result<FileLock> held = try_acquire_file_lock(lock);
  REQUIRE(held.has_value());
  backdate(lock, std::chrono::hours{-24});  // 24h in the FUTURE

  Result<FileLock> thief = try_acquire_file_lock(lock, std::chrono::seconds{1});
  REQUIRE_FALSE(thief.has_value());
  CHECK(thief.error().category == ErrorCategory::Transient);
}

TEST_CASE("release never deletes a lock file that is no longer ours", "[platform][lock]") {
  const TempDir dir;
  const fs::path lock = dir.path / "refdata.lock";

  Result<FileLock> mine = try_acquire_file_lock(lock);
  REQUIRE(mine.has_value());

  // Someone else's payload lands at our path (the takeover we did not notice).
  LockPayload other;
  other.pid = 999999;
  other.acquired_at = "2026-08-09T09:15:00Z";
  other.nonce = "someone-else";
  {
    std::ofstream out(lock, std::ios::binary | std::ios::trunc);
    const std::string text = format_lock_payload(other);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
  }

  mine.value().release();
  REQUIRE(fs::exists(lock));  // we must NOT have removed a lock we do not own
  const auto parsed = parse_lock_payload(read_all(lock));
  REQUIRE(parsed.has_value());
  CHECK(parsed->nonce == "someone-else");
}

TEST_CASE("a FRESH lock file with garbage in it is simply 'held' (age decides first)",
          "[platform][lock]") {
  const TempDir dir;
  const fs::path lock = dir.path / "refdata.lock";
  write_text(lock, "not a lock payload at all");  // fresh mtime

  Result<FileLock> attempt = try_acquire_file_lock(lock, std::chrono::minutes{10});
  REQUIRE_FALSE(attempt.has_value());
  CHECK(attempt.error().category == ErrorCategory::Transient);  // "held by another process"
  CHECK(fs::exists(lock));
}

TEST_CASE("a STALE lock file with an UNPARSEABLE payload is NEVER taken over (guarantee 4)",
          "[platform][lock]") {
  const TempDir dir;
  const fs::path lock = dir.path / "refdata.lock";

  // Old enough to qualify on age, but the bytes are not one of our lock files.
  // Age alone must not license a takeover: this could be any artifact.
  write_text(lock, "not a lock payload at all");
  backdate(lock, std::chrono::hours{2});

  Result<FileLock> attempt = try_acquire_file_lock(lock, std::chrono::minutes{10});
  REQUIRE_FALSE(attempt.has_value());
  // Internal ⇒ RaiseAlert: a human has to look at whatever this file is.
  CHECK(attempt.error().category == ErrorCategory::Internal);
  CHECK(attempt.error().action == SuggestedAction::RaiseAlert);
  // Untouched — we do not guess, and we do not delete.
  CHECK(fs::exists(lock));
  CHECK(read_all(lock) == "not a lock payload at all");

  // A well-formed stale lock of the same age IS taken over, so the refusal above
  // is specifically about the payload, not about the age.
  const fs::path other = dir.path / "other.lock";
  plant_stale_lock(other, std::chrono::hours{2});
  CHECK(try_acquire_file_lock(other, std::chrono::minutes{10}).has_value());
}

TEST_CASE("takeover ABORTS (and restores) when the claimed lock turns out to be live",
          "[platform][lock]") {
  const TempDir dir;
  const fs::path lock = dir.path / "refdata.lock";
  plant_stale_lock(lock, std::chrono::hours{1});
  const std::string victim = read_all(lock);

  // THE RACE, made deterministic: right after we claim the stale file, a
  // legitimate holder is revealed to have been alive all along (its file's mtime
  // is current). This is exactly the interleaving where the staleness
  // observation and the rename straddle another process's acquisition.
  const ScopedLockFault fault([](LockFaultPoint point, const fs::path&, const fs::path& claim) {
    if (point == LockFaultPoint::AfterStaleClaim) {
      std::error_code ec;
      fs::last_write_time(claim, fs::file_time_type::clock::now(), ec);
    }
  });

  Result<FileLock> attempt = try_acquire_file_lock(lock, std::chrono::minutes{10});
  REQUIRE_FALSE(attempt.has_value());
  CHECK(attempt.error().category == ErrorCategory::Transient);
  CHECK(attempt.error().action == SuggestedAction::RetrySafe);

  // The victim's lock is BACK at its own path, byte for byte, and our lock is
  // gone: we never keep a lock we took from a live holder.
  REQUIRE(fs::exists(lock));
  CHECK(read_all(lock) == victim);

  // And no claim debris is left behind.
  for (const auto& entry : fs::directory_iterator(dir.path)) {
    CHECK(entry.path().filename().string().find(".stale-") == std::string::npos);
  }
}

TEST_CASE("takeover ABORTS WITHOUT RESTORING when another process takes the freed name",
          "[platform][lock]") {
  const TempDir dir;
  const fs::path lock = dir.path / "refdata.lock";
  plant_stale_lock(lock, std::chrono::hours{1});

  // THE RACE: the instant we move the corpse aside, a third process creates its
  // own lock at the freed name. Our create-exclusive must lose, and we must NOT
  // put the corpse back — that would overwrite a live lock.
  const std::string intruder = foreign_payload("intruder-nonce");
  const ScopedLockFault fault(
      [&intruder](LockFaultPoint point, const fs::path& lock_path, const fs::path&) {
        if (point == LockFaultPoint::AfterStaleClaim) {
          write_text(lock_path, intruder);
        }
      });

  Result<FileLock> attempt = try_acquire_file_lock(lock, std::chrono::minutes{10});
  REQUIRE_FALSE(attempt.has_value());
  CHECK(attempt.error().category == ErrorCategory::Transient);

  // The INTRUDER's lock survives untouched — never clobbered by a restore.
  REQUIRE(fs::exists(lock));
  CHECK(read_all(lock) == intruder);

  // The corpse we claimed was dropped, not restored over the intruder.
  for (const auto& entry : fs::directory_iterator(dir.path)) {
    CHECK(entry.path().filename().string().find(".stale-") == std::string::npos);
  }
}

TEST_CASE("a lock replaced between the create and the read-back is NOT claimed",
          "[platform][lock]") {
  const TempDir dir;
  const fs::path lock = dir.path / "refdata.lock";

  // THE RACE: our create-exclusive succeeded, and in the very next instant a
  // stale-takeover elsewhere replaced the file with someone else's. The
  // read-back nonce check is what stops us walking away believing we hold it.
  const std::string thief = foreign_payload("thief-nonce");
  const ScopedLockFault fault(
      [&thief](LockFaultPoint point, const fs::path& lock_path, const fs::path&) {
        if (point == LockFaultPoint::AfterCreate) {
          write_text(lock_path, thief);
        }
      });

  Result<FileLock> attempt = try_acquire_file_lock(lock, std::chrono::minutes{10});
  REQUIRE_FALSE(attempt.has_value());
  CHECK(attempt.error().category == ErrorCategory::Transient);

  // We did not delete the thief's lock on the way out either.
  REQUIRE(fs::exists(lock));
  CHECK(read_all(lock) == thief);
}

TEST_CASE("the read-back check also fires on the TAKEOVER path", "[platform][lock]") {
  const TempDir dir;
  const fs::path lock = dir.path / "refdata.lock";
  plant_stale_lock(lock, std::chrono::hours{1});

  const std::string thief = foreign_payload("thief-after-takeover");
  const ScopedLockFault fault(
      [&thief](LockFaultPoint point, const fs::path& lock_path, const fs::path&) {
        if (point == LockFaultPoint::AfterCreate) {
          write_text(lock_path, thief);
        }
      });

  Result<FileLock> attempt = try_acquire_file_lock(lock, std::chrono::minutes{10});
  REQUIRE_FALSE(attempt.has_value());
  CHECK(attempt.error().category == ErrorCategory::Transient);
  CHECK(read_all(lock) == thief);
}

TEST_CASE("still_ours() reports the truth about a lock that was taken", "[platform][lock]") {
  const TempDir dir;
  const fs::path lock = dir.path / "refdata.lock";

  Result<FileLock> mine = try_acquire_file_lock(lock);
  REQUIRE(mine.has_value());
  CHECK(mine.value().still_ours());

  // Replaced by a takeover we did not notice.
  write_text(lock, foreign_payload("someone-else"));
  CHECK_FALSE(mine.value().still_ours());

  // Deleted out from under us — also "not ours" (fail closed, not "assume yes").
  {
    std::error_code ec;
    fs::remove(lock, ec);
  }
  CHECK_FALSE(mine.value().still_ours());

  // A lock we never held is never ours.
  const FileLock empty;
  CHECK_FALSE(empty.still_ours());
}

TEST_CASE("touch() refreshes the mtime of a lock we still own, and only that one",
          "[platform][lock]") {
  const TempDir dir;
  const fs::path lock = dir.path / "refdata.lock";

  Result<FileLock> mine = try_acquire_file_lock(lock, std::chrono::minutes{10});
  REQUIRE(mine.has_value());

  // Age it past the staleness window: a contender would now take it over.
  backdate(lock, std::chrono::hours{1});
  const fs::file_time_type before = fs::last_write_time(lock);

  // A long download finishes and the holder proves it is alive.
  CHECK(mine.value().touch());
  CHECK(fs::last_write_time(lock) > before);

  // ...and the lock is no longer stealable.
  Result<FileLock> thief = try_acquire_file_lock(lock, std::chrono::minutes{10});
  CHECK_FALSE(thief.has_value());

  // touch() must NEVER extend the life of a lock that is not ours.
  write_text(lock, foreign_payload("someone-else"));
  backdate(lock, std::chrono::hours{1});
  const fs::file_time_type foreign_before = fs::last_write_time(lock);
  CHECK_FALSE(mine.value().touch());
  CHECK(fs::last_write_time(lock) == foreign_before);

  // A lock we never held cannot be touched either.
  FileLock empty;
  CHECK_FALSE(empty.touch());
}

TEST_CASE("payload round-trips and rejects malformed text", "[platform][lock]") {
  LockPayload payload;
  payload.pid = 4242;
  payload.acquired_at = "2026-08-09T09:15:00Z";
  payload.nonce = "4242-1234567890-1";

  const std::string text = format_lock_payload(payload);
  const auto parsed = parse_lock_payload(text);
  REQUIRE(parsed.has_value());
  CHECK(parsed->pid == 4242);
  CHECK(parsed->acquired_at == "2026-08-09T09:15:00Z");
  CHECK(parsed->nonce == "4242-1234567890-1");

  // CRLF tolerance (a Windows editor touching the file must not break parsing).
  std::string crlf;
  for (const char c : text) {
    if (c == '\n') {
      crlf.push_back('\r');
    }
    crlf.push_back(c);
  }
  CHECK(parse_lock_payload(crlf).has_value());

  // Forward compatible: unknown keys are ignored.
  CHECK(parse_lock_payload(text + "future_key=whatever\n").has_value());

  // Fail closed on anything incomplete or unparseable.
  CHECK_FALSE(parse_lock_payload("").has_value());
  CHECK_FALSE(parse_lock_payload("garbage").has_value());
  CHECK_FALSE(parse_lock_payload("pid=1\nnonce=n\n").has_value());          // no timestamp
  CHECK_FALSE(parse_lock_payload("pid=1\nacquired_at=t\n").has_value());    // no nonce
  CHECK_FALSE(parse_lock_payload("acquired_at=t\nnonce=n\n").has_value());  // no pid
  CHECK_FALSE(parse_lock_payload("pid=abc\nacquired_at=t\nnonce=n\n").has_value());
  CHECK_FALSE(parse_lock_payload("pid=\nacquired_at=t\nnonce=n\n").has_value());
}

TEST_CASE("an empty lock path is rejected", "[platform][lock]") {
  Result<FileLock> attempt = try_acquire_file_lock(fs::path{});
  REQUIRE_FALSE(attempt.has_value());
  CHECK(attempt.error().category == ErrorCategory::Internal);
}

TEST_CASE("a default-constructed FileLock holds nothing", "[platform][lock]") {
  FileLock empty;
  CHECK_FALSE(empty.held());
  empty.release();  // no-op, no crash
  CHECK_FALSE(empty.held());
}
