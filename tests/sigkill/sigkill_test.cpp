// SIGKILL durability harness (Story 1.12, NFR-3/NFR-10) — the capstone proof of
// Epic 1. Spawns the plain `sigkill_worker` exe, KILLS it at the single most
// dangerous instant (after the intent fsync, before the broker send), then runs
// recovery — replay, rebuild the idempotency index, and RE-SUBMIT the same signal
// against a broker that already holds the pre-kill order — and asserts the restart
// created ZERO duplicates and issued ZERO broker sends.
//
// HOW THE KILL IS REALIZED: the worker installs a Dispatcher pre-send barrier that
// calls std::_Exit(42) — the portable, uncatchable-kill analog (no destructors, no
// flushing, no `#ifdef`; see sigkill_worker.cpp for the full rationale). The fsync
// inside IntentLog::append() has already made the PlaceOrder record durable, so the
// process dies with the intent ON DISK but the send NEVER attempted.
//
// WHY THIS TEST READS A REPORT FILE AND NOT JUST AN EXIT CODE: run_command hands
// back one integer, and on POSIX not even the child's exit code directly (it is a
// wait-status). An exit code therefore cannot distinguish "survived a kill between
// fsync and send" from "found nothing and had nothing to do" — which is precisely
// how this suite once passed against a recovery that counted duplicates in a
// brand-new, always-empty FakeBroker. The worker now writes its counts to
// <datadir>/recover_report.txt and the assertions below read THOSE.
//
// CROSS-PLATFORM SPAWN: std::system runs on Windows, Linux and macOS with no
// platform branching. The ONE place a platform difference exists is decoding its
// return value: on Windows it is the child's exit code directly; on POSIX it is a
// wait-status whose exit code lives in the high byte. We deliberately AVOID
// platform macros (WEXITSTATUS et al.) and assert only the platform-invariant
// property of the code itself — zero vs non-zero — putting every NUMBER in the
// report file, which is byte-identical on every OS. Paths are quoted to tolerate
// spaces.
//
// Cross-platform: std::filesystem for the temp dir, std::system for the spawn,
// std::ifstream to inspect the durable log and the report. C++20 stdlib only.

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

// The file the worker's `recover` subcommand writes its counts to. Kept in one
// place so a rename cannot leave a test silently reading a file that never exists
// (a missing file must fail loudly, and REQUIRE(fs::exists) below makes it).
constexpr const char* kRecoverReport = "recover_report.txt";

// A unique temp data dir for one harness run, removed on scope exit. Holds the
// worker's intent.log + store.db so the kill and recover phases share state.
struct TempDataDir {
  fs::path path;
  explicit TempDataDir(const std::string& tag)
      : path(fs::temp_directory_path() / ("broker_exec_sigkill_" + tag + "_" +
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
// record after the kill (proving the fsync happened before death), and to read the
// worker's recovery report.
std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// The value of one KEY=<n> line in the worker's recovery report, or -1 if the key
// is absent or not a run of digits. -1 is deliberately a value NO caller expects:
// every use below asserts an exact non-negative number, so a malformed or missing
// report FAILS the check rather than quietly matching it. Parsed by hand (no stoi)
// so a garbled report cannot throw out of a helper.
int report_value(const std::string& report, const std::string& key) {
  std::istringstream lines(report);
  std::string line;
  const std::string prefix = key + "=";
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();  // the worker writes in text mode; tolerate CRLF on Windows
    }
    if (line.rfind(prefix, 0) != 0) {
      continue;
    }
    const std::string value = line.substr(prefix.size());
    if (value.empty()) {
      return -1;
    }
    int out = 0;
    for (const char c : value) {
      if (c < '0' || c > '9') {
        return -1;
      }
      out = out * 10 + (c - '0');
    }
    return out;
  }
  return -1;
}

// The two command lines the harness spawns, quoted to tolerate spaces in paths.
std::string kill_command(const fs::path& datadir) {
  std::ostringstream cmd;
  cmd << '"' << SIGKILL_WORKER_EXE << "\" worker \"" << datadir.string() << "\" kill";
  return cmd.str();
}

std::string recover_command(const fs::path& datadir, const std::string& options = {}) {
  std::ostringstream cmd;
  cmd << '"' << SIGKILL_WORKER_EXE << "\" recover \"" << datadir.string() << '"';
  if (!options.empty()) {
    cmd << ' ' << options;
  }
  return cmd.str();
}

}  // namespace

TEST_CASE("sigkill: a kill between fsync and send leaves zero duplicates on recovery",
          "[sigkill][durability][zero-duplicate]") {
  TempDataDir dir("kill_recover");

  // ── Phase 1: spawn the worker and KILL it in the pre-send barrier ───────────
  const int kill_rc = broker_exec::platform::run_command(kill_command(dir.path));

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

  // ── Phase 2: recover — replay, rebuild, re-submit; ZERO duplicates ──────────
  const int recover_rc = broker_exec::platform::run_command(recover_command(dir.path));

  // Exit 0 iff the re-submit cost zero sends and left zero duplicates.
  CHECK(recover_rc == 0);

  // ...and then check the NUMBERS, because the exit code alone cannot tell a survived
  // kill from a no-op. Every assertion below is on state the recovery actually
  // observed.
  const fs::path report_path = dir.path / kRecoverReport;
  REQUIRE(fs::exists(report_path));
  const std::string report = read_file(report_path);

  // The killed order is still ENUMERABLE from the durable log — exactly one of it.
  CHECK(report_value(report, "ORDERS") == 1);

  // The idempotency index really was rebuilt from that log. After a PRE-SEND kill
  // this is the ONLY dedup layer that can fire (the projection is empty, so
  // UNIQUE(client_ref) has nothing to match), so this number IS the mechanism under
  // test: a gutted IdempotencyIndex::rebuild_from_log reports 0 here.
  CHECK(report_value(report, "INDEX_ENTRIES") == 1);

  // THE HEADLINE: re-submitting the same signal after the restart issued ZERO
  // broker mutations (FR-10 — an already-recorded signal costs no send)...
  CHECK(report_value(report, "RESENDS") == 0);

  // ...so broker truth still holds exactly the ONE order the kill run was about to
  // send, under exactly one client_ref, with no second order for the signal.
  CHECK(report_value(report, "BROKER_ORDERS") == 1);
  CHECK(report_value(report, "DUPLICATES") == 0);
  CHECK(report_value(report, "DUPLICATE_REFS") == 0);
}

TEST_CASE("sigkill: recovery is idempotent — a second recover still reports zero duplicates",
          "[sigkill][durability][idempotent]") {
  TempDataDir dir("double_recover");

  // Kill once.
  const int kill_rc = broker_exec::platform::run_command(kill_command(dir.path));
  CHECK(kill_rc != 0);

  // Recover twice. The first recovery dedups the re-submit and therefore writes
  // NOTHING new — no log append, no store row, no broker send — so the second
  // recovery starts from the identical durable state and must reach the identical
  // conclusion. If the first one had re-fired, the second would see two orders.
  const int first = broker_exec::platform::run_command(recover_command(dir.path));
  const int second = broker_exec::platform::run_command(recover_command(dir.path));
  CHECK(first == 0);
  CHECK(second == 0);

  // The report left by the SECOND run: still one enumerated order, still one order
  // at the broker, still zero sends.
  const std::string report = read_file(dir.path / kRecoverReport);
  REQUIRE_FALSE(report.empty());
  CHECK(report_value(report, "ORDERS") == 1);
  CHECK(report_value(report, "BROKER_ORDERS") == 1);
  CHECK(report_value(report, "RESENDS") == 0);
  CHECK(report_value(report, "DUPLICATES") == 0);
}

// NEGATIVE CONTROL — the assertion that makes every OTHER assertion in this file
// mean something. A durability harness nobody has watched go red is not evidence:
// this suite previously counted duplicates in a freshly constructed, always-empty
// FakeBroker, so DUPLICATES was 0 for every possible build of the library and the
// one fault the suite exists to catch could not turn it red.
//
// `--no-dedup` makes the worker model a build whose restart-dedup is broken: it
// skips the index rebuild AND mints from a different UUID seed, so the restart
// re-submit fires a SECOND order at a broker that already holds the first, under a
// brand-new client_ref. That is the real duplicate — two lots in the account for
// one signal — and the harness must say so.
TEST_CASE("sigkill: NEGATIVE CONTROL — with restart-dedup disabled the harness reports a duplicate",
          "[sigkill][durability][negative-control]") {
  TempDataDir dir("no_dedup");

  const int kill_rc = broker_exec::platform::run_command(kill_command(dir.path));
  CHECK(kill_rc != 0);

  const int rc = broker_exec::platform::run_command(recover_command(dir.path, "--no-dedup"));
  CHECK(rc != 0);  // a duplicate MUST fail the run

  const std::string report = read_file(dir.path / kRecoverReport);
  REQUIRE_FALSE(report.empty());

  // The index was not rebuilt, so the signal was not recognised and the re-submit
  // sent a second order: two orders at the broker for one signal.
  CHECK(report_value(report, "INDEX_ENTRIES") == 0);
  CHECK(report_value(report, "RESENDS") == 1);
  CHECK(report_value(report, "BROKER_ORDERS") == 2);
  CHECK(report_value(report, "DUPLICATES") == 1);

  // AND THE POINT OF COUNTING ON THE SIGNAL SIGNATURE: the second order carries a
  // client_ref nobody has seen, so a ref-keyed counter reports a clean ZERO for the
  // very run that doubled the position. DUPLICATE_REFS is that ref-keyed count, and
  // pinning it at 0 here states the trap in the test itself: the day someone
  // "simplifies" DUPLICATES back onto the client_ref, the DUPLICATES == 1 check
  // above goes red instead of the harness quietly going blind again.
  CHECK(report_value(report, "DUPLICATE_REFS") == 0);
}

TEST_CASE("sigkill: recover REFUSES a datadir where no kill ever happened",
          "[sigkill][durability][no-vacuous-pass]") {
  TempDataDir dir("no_kill");

  // No kill phase — the log is empty, so there is no PlaceOrder record and nothing
  // to recover. A recovery that reports success here is reporting on an order that
  // never existed, which is indistinguishable from reporting on one that survived.
  // Fail closed: refuse, and leave no report behind for a later read to pick up.
  const int rc = broker_exec::platform::run_command(recover_command(dir.path));
  CHECK(rc != 0);
  CHECK_FALSE(fs::exists(dir.path / kRecoverReport));
}
