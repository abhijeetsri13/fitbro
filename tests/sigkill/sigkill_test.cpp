// SIGKILL durability harness (Story 1.12, NFR-3/NFR-10) — the capstone proof of
// Epic 1. Spawns the plain `sigkill_worker` exe, KILLS it at the single most
// dangerous instant (after the intent fsync, before the broker send), then runs
// recovery and asserts ZERO duplicate orders on replay + reconcile.
//
// HOW THE KILL IS REALIZED: the worker installs a Dispatcher pre-send barrier that
// calls std::_Exit(42) — the portable, uncatchable-kill analog (no destructors, no
// flushing, no `#ifdef`; see sigkill_worker.cpp for the full rationale). The fsync
// inside IntentLog::append() has already made the PlaceOrder record durable, so the
// process dies with the intent ON DISK but the send NEVER attempted.
//
// CROSS-PLATFORM SPAWN: std::system runs on Windows, Linux and macOS with no
// platform branching. The ONE place a platform difference exists is decoding its
// return value: on Windows it is the child's exit code directly; on POSIX it is a
// wait-status whose exit code lives in the high byte. We deliberately AVOID
// platform macros (WEXITSTATUS et al.) and assert only the platform-invariant
// properties: the kill run returns NON-ZERO (the child died in the barrier, it did
// not exit 0) and the recover run returns ZERO (DUPLICATES=0). Both hold
// identically on every OS. Paths are quoted to tolerate spaces.
//
// Cross-platform: std::filesystem for the temp dir, std::system for the spawn,
// std::ifstream to inspect the durable log. C++20 stdlib only.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include "broker_exec/platform/process.hpp"

#ifndef SIGKILL_WORKER_EXE
#error "SIGKILL_WORKER_EXE must be defined by CMake (path to broker_exec_sigkill_worker)"
#endif

namespace fs = std::filesystem;

namespace {

// A unique temp data dir for one harness run, removed on scope exit. Holds the
// worker's intent.log + store.db so the kill and recover phases share state.
struct TempDataDir {
  fs::path path;
  explicit TempDataDir(const std::string& tag)
      : path(fs::temp_directory_path() /
             ("broker_exec_sigkill_" + tag + "_" +
              std::to_string(reinterpret_cast<std::uintptr_t>(this)))) {
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
  }
  ~TempDataDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TempDataDir(const TempDataDir&) = delete;
  TempDataDir& operator=(const TempDataDir&) = delete;
};

// Read a file fully (binary). Used to assert the intent log holds the PlaceOrder
// record after the kill (proving the fsync happened before death).
std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

TEST_CASE("sigkill: a kill between fsync and send leaves zero duplicates on recovery",
          "[sigkill][durability][zero-duplicate]") {
  TempDataDir dir("kill_recover");

  // ── Phase 1: spawn the worker and KILL it in the pre-send barrier ───────────
  // Command: "<exe>" worker "<datadir>" kill
  std::ostringstream kill_cmd;
  kill_cmd << '"' << SIGKILL_WORKER_EXE << "\" worker \"" << dir.path.string() << "\" kill";
  const int kill_rc = broker_exec::platform::run_command(kill_cmd.str());

  // The child must NOT have exited 0 — it died inside the std::_Exit barrier
  // (POSIX: wait-status with code 42 in the high byte; Windows: 42 directly). The
  // platform-invariant assertion is simply "non-zero".
  CHECK(kill_rc != 0);

  // The fsync happened BEFORE the kill: the durable intent log must already contain
  // the PlaceOrder record (op name "place_order"), proving durability precedes the
  // send.
  const fs::path log_path = dir.path / "intent.log";
  REQUIRE(fs::exists(log_path));
  const std::string log_bytes = read_file(log_path);
  REQUIRE_FALSE(log_bytes.empty());
  CHECK(log_bytes.find("place_order") != std::string::npos);

  // ── Phase 2: recover — replay + reconcile must yield ZERO duplicates ─────────
  // Command: "<exe>" recover "<datadir>"
  std::ostringstream recover_cmd;
  recover_cmd << '"' << SIGKILL_WORKER_EXE << "\" recover \"" << dir.path.string() << "\"";
  const int recover_rc = broker_exec::platform::run_command(recover_cmd.str());

  // Exit 0 iff DUPLICATES == 0 — the headline invariant survived the SIGKILL.
  CHECK(recover_rc == 0);
}

TEST_CASE("sigkill: recovery is idempotent — a second recover still reports zero duplicates",
          "[sigkill][durability][idempotent]") {
  TempDataDir dir("double_recover");

  // Kill once.
  std::ostringstream kill_cmd;
  kill_cmd << '"' << SIGKILL_WORKER_EXE << "\" worker \"" << dir.path.string() << "\" kill";
  const int kill_rc = broker_exec::platform::run_command(kill_cmd.str());
  CHECK(kill_rc != 0);

  // Recover twice. Replay + reconcile is read-only on the broker and never fires a
  // mutation, so a second recovery must ALSO report zero duplicates (it cannot
  // worsen the state it just proved clean).
  std::ostringstream recover_cmd;
  recover_cmd << '"' << SIGKILL_WORKER_EXE << "\" recover \"" << dir.path.string() << "\"";
  const int first = broker_exec::platform::run_command(recover_cmd.str());
  const int second = broker_exec::platform::run_command(recover_cmd.str());
  CHECK(first == 0);
  CHECK(second == 0);
}
