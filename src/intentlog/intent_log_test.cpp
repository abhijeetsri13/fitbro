#include "broker_exec/intentlog/intent_log.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <vector>

#include "broker_exec/clock/test_clock.hpp"

namespace fs = std::filesystem;
using broker_exec::clock::TestClock;
using broker_exec::intentlog::IntentLog;
using broker_exec::intentlog::IntentOp;
using broker_exec::intentlog::IntentRecord;

namespace {

// A unique temp path per test, removed on scope exit so runs never collide and
// no artifacts are left behind.
struct TempLog {
  fs::path path;

  explicit TempLog(const std::string& tag)
      : path(fs::temp_directory_path() /
             ("broker_exec_intentlog_" + tag + "_" +
              std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".log")) {
    std::error_code ec;
    fs::remove(path, ec);
  }
  ~TempLog() {
    std::error_code ec;
    fs::remove(path, ec);
  }
  TempLog(const TempLog&) = delete;
  TempLog& operator=(const TempLog&) = delete;
};

// A wall clock fixed at a known instant, given as a count of system_clock ticks
// since epoch (so it is exactly representable regardless of the platform's
// system_clock resolution — ns on some, 100ns/us on others).
TestClock clock_at_ticks(std::int64_t ticks) {
  using sc = std::chrono::system_clock;
  return TestClock(std::chrono::steady_clock::time_point{},
                   sc::time_point(sc::duration(ticks)));
}

// The wall_ts_ns that append() will stamp for a given tick count — derived via
// the SAME duration_cast the implementation uses, so the assertion is exact on
// every platform.
std::int64_t expected_wall_ns(std::int64_t ticks) {
  using sc = std::chrono::system_clock;
  return std::chrono::duration_cast<std::chrono::nanoseconds>(sc::duration(ticks)).count();
}

}  // namespace

TEST_CASE("append N records, then replay rebuilds index and enumerates them",
          "[intentlog][replay]") {
  TempLog tmp("appendN");
  const std::int64_t ticks = 1'700'000'000;
  TestClock clock = clock_at_ticks(ticks);

  auto opened = IntentLog::open(tmp.path, clock);
  REQUIRE(opened.has_value());
  IntentLog log = std::move(opened.value());

  // Empty log replays to nothing; next_seq starts at 1.
  auto empty = log.replay();
  REQUIRE(empty.has_value());
  REQUIRE(empty.value().empty());
  REQUIRE(log.next_seq() == 1);

  const std::vector<std::string> refs = {"strat-aaaa-0001", "strat-bbbb-0002",
                                         "strat-cccc-0003"};
  for (std::size_t i = 0; i < refs.size(); ++i) {
    auto appended = log.append(IntentOp::PlaceOrder, refs[i],
                               R"({"qty":1,"side":"BUY"})");
    REQUIRE(appended.has_value());
    const IntentRecord& rec = appended.value();
    REQUIRE(rec.seq == static_cast<std::int64_t>(i + 1));
    REQUIRE(rec.client_ref == refs[i]);
    REQUIRE(rec.wall_ts_ns == expected_wall_ns(ticks));
    REQUIRE(rec.hash.size() == 64);
    if (i == 0) {
      REQUIRE(rec.prev_hash == "GENESIS");
    }
  }
  REQUIRE(log.next_seq() == 4);

  // Index lookups after appends.
  REQUIRE(log.last_for("strat-bbbb-0002").has_value());
  REQUIRE(log.last_for("strat-bbbb-0002").value().seq == 2);
  REQUIRE_FALSE(log.last_for("nope").has_value());

  // Replay enumerates all records in order and the chain links across them.
  auto replayed = log.replay();
  REQUIRE(replayed.has_value());
  const std::vector<IntentRecord>& recs = replayed.value();
  REQUIRE(recs.size() == 3);
  REQUIRE(recs[0].prev_hash == "GENESIS");
  REQUIRE(recs[1].prev_hash == recs[0].hash);
  REQUIRE(recs[2].prev_hash == recs[1].hash);
}

TEST_CASE("last_for returns the latest record for a repeated client_ref",
          "[intentlog][index]") {
  TempLog tmp("repeat");
  TestClock clock = clock_at_ticks(42);
  auto opened = IntentLog::open(tmp.path, clock);
  REQUIRE(opened.has_value());
  IntentLog log = std::move(opened.value());

  REQUIRE(log.append(IntentOp::PlaceOrder, "ref-1", "{}").has_value());
  REQUIRE(log.append(IntentOp::Result, "ref-1", R"({"status":"FILLED"})").has_value());

  auto latest = log.last_for("ref-1");
  REQUIRE(latest.has_value());
  REQUIRE(latest.value().seq == 2);
  REQUIRE(latest.value().op == IntentOp::Result);
}

TEST_CASE("tampering with a past record breaks the hash chain on replay",
          "[intentlog][tamper]") {
  TempLog tmp("tamper");
  TestClock clock = clock_at_ticks(7);

  {
    auto opened = IntentLog::open(tmp.path, clock);
    REQUIRE(opened.has_value());
    IntentLog log = std::move(opened.value());
    REQUIRE(log.append(IntentOp::PlaceOrder, "ref-a", R"({"qty":5})").has_value());
    REQUIRE(log.append(IntentOp::PlaceOrder, "ref-b", R"({"qty":9})").has_value());
    REQUIRE(log.append(IntentOp::CancelOrder, "ref-a", "{}").has_value());
  }  // closed/flushed

  // Mutate one byte of the payload of the FIRST record on disk without updating
  // its hash — the canonical bytes no longer match the stored hash.
  {
    std::string contents;
    {
      std::ifstream in(tmp.path, std::ios::binary);
      REQUIRE(in.good());
      contents.assign((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    }
    // The first record's payload {"qty":5} is stored as an escaped JSON string,
    // so on disk it contains the literal bytes ":5}" (record 2 has ":9}"). Flip
    // the unique 5 -> 6 to corrupt the payload without touching its stored hash.
    const auto qpos = contents.find(":5}");
    REQUIRE(qpos != std::string::npos);
    contents[qpos + 1] = '6';  // change 5 -> 6 in the first record's payload
    {
      std::ofstream out(tmp.path, std::ios::binary | std::ios::trunc);
      REQUIRE(out.good());
      out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }
  }

  TestClock clock2 = clock_at_ticks(7);
  auto opened = IntentLog::open(tmp.path, clock2);
  REQUIRE(opened.has_value());
  IntentLog log = std::move(opened.value());

  auto replayed = log.replay();
  REQUIRE_FALSE(replayed.has_value());
  REQUIRE(replayed.error().category == broker_exec::errors::ErrorCategory::Internal);
  // The first bad seq is named in the message.
  REQUIRE(replayed.error().message.find("seq 1") != std::string::npos);
}

TEST_CASE("reopen + replay after a restart yields the same index (durability)",
          "[intentlog][durability]") {
  TempLog tmp("restart");
  std::vector<std::string> hashes;

  // Session 1: write records, then drop the log object (simulating shutdown).
  {
    TestClock clock = clock_at_ticks(100);
    auto opened = IntentLog::open(tmp.path, clock);
    REQUIRE(opened.has_value());
    IntentLog log = std::move(opened.value());
    for (int i = 1; i <= 5; ++i) {
      auto r = log.append(IntentOp::PlaceOrder, "ref-" + std::to_string(i),
                          R"({"i":)" + std::to_string(i) + "}");
      REQUIRE(r.has_value());
      hashes.push_back(r.value().hash);
    }
  }

  // Session 2 ("restart"): reopen and replay — same records, same hashes, same
  // index, and next_seq continues from the durable tail.
  {
    TestClock clock = clock_at_ticks(200);
    auto opened = IntentLog::open(tmp.path, clock);
    REQUIRE(opened.has_value());
    IntentLog log = std::move(opened.value());

    auto replayed = log.replay();
    REQUIRE(replayed.has_value());
    const std::vector<IntentRecord>& recs = replayed.value();
    REQUIRE(recs.size() == 5);
    for (std::size_t i = 0; i < recs.size(); ++i) {
      REQUIRE(recs[i].seq == static_cast<std::int64_t>(i + 1));
      REQUIRE(recs[i].hash == hashes[i]);
      REQUIRE(log.last_for("ref-" + std::to_string(i + 1)).has_value());
    }
    REQUIRE(log.next_seq() == 6);

    // A new append after replay continues the chain durably.
    auto cont = log.append(IntentOp::SquareOff, "ref-6", "{}");
    REQUIRE(cont.has_value());
    REQUIRE(cont.value().seq == 6);
    REQUIRE(cont.value().prev_hash == hashes.back());
  }
}

TEST_CASE("IntentOp wire names round-trip", "[intentlog][op]") {
  using broker_exec::intentlog::intent_op_from_string;
  using broker_exec::intentlog::to_string;
  const IntentOp ops[] = {IntentOp::PlaceOrder, IntentOp::ModifyOrder, IntentOp::CancelOrder,
                          IntentOp::SquareOff,  IntentOp::Result,      IntentOp::ChildSlice};
  for (IntentOp op : ops) {
    auto back = intent_op_from_string(to_string(op));
    REQUIRE(back.has_value());
    REQUIRE(back.value() == op);
  }
  REQUIRE_FALSE(intent_op_from_string("bogus").has_value());
}
