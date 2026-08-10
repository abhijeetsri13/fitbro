#include "broker_exec/intentlog/intent_log.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// IMP-17: the round-trip assertions read the REAL bytes back off disk and parse
// them with the REAL serialiser, rather than trusting a paraphrase of it.
#include <nlohmann/json.hpp>

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

// ── IMP-17: THE HASHED BYTES AND THE STORED BYTES ARE THE SAME BYTES ─────────
//
// THE DEFECT: to_json_line() called j.dump() with the DEFAULT error handler,
// which is error_handler_t::strict — it THROWS json::type_error.316 ("invalid
// UTF-8 byte") on the first ill-formed byte. IntentLog::append() returns
// Result<IntentRecord> (a NO-THROW BOUNDARY) and sits on the ORDER DISPATCH HOT
// PATH (runtime/dispatcher.cpp), fsync'ing the DURABLE TRUTH *before* the broker
// socket write. An uncaught throw there lands exactly between "decided to trade"
// and "recorded that we decided". The vector is CALLER- and STORE-supplied text —
// a strategy name, a client_ref, an instrument symbol carried in payload_json —
// NOT broker JSON, which nlohmann already validated on parse.
//
// Merely switching the handler to ::replace would trade a throw for a SILENT
// CORRUPTION: the ill-formed sequence would be rewritten to U+FFFD on the way out,
// so the line on disk would no longer be the text the SHA-256 chain hashed and
// replay() would report our own record "tampered".
//
// THE FIX (the same one, from the same implementation, as the ledger's):
// domain::canonical_text() normalises the two caller-supplied fields ONCE, up
// front, so the preimage and the stored bytes are one std::string. ::replace stays
// on the dump as belt and braces.

namespace {

// Ill-formed UTF-8, spelled BYTE-EXACTLY. Every hex escape is terminated by a
// SPACE or a string-literal break, never by a hex digit — a C++ hex escape is
// GREEDY ("\xE2" followed by 'f' would be read as ONE escape, not two chars).
constexpr std::string_view kLoneContinuation = "lone \x80 byte";      // bare 0x80
constexpr std::string_view kInvalidLead = "invalid \xFF byte";        // 0xFF, never a lead
constexpr std::string_view kTruncatedRun = "truncated \xE2\x82 run";  // 2 of a 3-byte run

// The canonical form of each: ONE U+FFFD (EF BF BD) per MAXIMAL ill-formed
// subpart. Written out literally so these pin the exact replacement semantics —
// note the truncated two-byte run is ONE subpart and yields ONE replacement.
// These are the SAME literals ledger_test.cpp pins, which is the point: one
// implementation, so both chains canonicalise identical input identically.
constexpr std::string_view kLoneContinuationCanonical = "lone \xEF\xBF\xBD byte";
constexpr std::string_view kInvalidLeadCanonical = "invalid \xEF\xBF\xBD byte";
constexpr std::string_view kTruncatedRunCanonical = "truncated \xEF\xBF\xBD run";

constexpr std::string_view kReplacementChar = "\xEF\xBF\xBD";

// The three vectors the reviewer named, as the fields they actually arrive in.
// A strategy name is the FIRST SEGMENT of every minted client_ref
// (idempotency::make_client_ref), and a symbol travels inside payload_json
// (idempotency::intent_payload_json) — so both reach this log as raw bytes.
constexpr std::string_view kBadStrategyRef = "al\x80" "pha-1a2b3c4d-0001";
constexpr std::string_view kBadStrategyRefCanonical = "al\xEF\xBF\xBD" "pha-1a2b3c4d-0001";

// A symbol inside payload_json, which is how a symbol actually reaches this log
// (idempotency::intent_payload_json projects intent.symbol into it). NOTE the
// literal concatenation: a RAW string literal does NOT process \x escapes, so the
// ill-formed byte must come from an ordinary literal spliced between two raw ones.
constexpr std::string_view kBadSymbolPayload = R"({"symbol":"NIFTY24)" "\xFF" R"(JUN"})";
constexpr std::string_view kBadSymbolPayloadCanonical =
    R"({"symbol":"NIFTY24)" "\xEF\xBF\xBD" R"(JUN"})";
constexpr std::string_view kTruncatedSymbolPayload = R"({"symbol":"BANK)" "\xE2\x82" R"("})";
constexpr std::string_view kTruncatedSymbolPayloadCanonical =
    R"({"symbol":"BANK)" "\xEF\xBF\xBD" R"("})";

// Read every JSON line back off disk and hand out one parsed object per record,
// so a test can assert that the BYTES IN THE FILE are the bytes we hashed.
[[nodiscard]] std::vector<nlohmann::json> stored_lines(const fs::path& p) {
  std::vector<nlohmann::json> out;
  std::ifstream in(p, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    nlohmann::json parsed = nlohmann::json::parse(line, nullptr, false);
    REQUIRE_FALSE(parsed.is_discarded());
    out.push_back(std::move(parsed));
  }
  return out;
}

// The outcome of ONE append, with "did it throw?" captured as DATA.
//
// Catch2's REQUIRE_NOTHROW cannot both run a call once AND yield its value, and
// this suite must assert BOTH halves of the contract on the SAME append: it did
// not throw, AND it recorded the intent. So the guard is written out. Before
// IMP-17, `threw` was true for every input below — nlohmann's default (strict)
// dump handler raised [json.exception.type_error.316] straight through append()'s
// Result<T> boundary, on the order dispatch hot path.
struct AppendOutcome {
  bool threw = false;
  bool ok = false;
  std::string client_ref;
  std::string payload_json;
  std::string hash;
};

[[nodiscard]] AppendOutcome append_guarded(IntentLog& log, IntentOp op, std::string_view ref,
                                           std::string_view payload) {
  AppendOutcome out;
  try {
    auto appended = log.append(op, std::string(ref), std::string(payload));
    out.ok = appended.has_value();
    if (out.ok) {
      out.client_ref = appended.value().client_ref;
      out.payload_json = appended.value().payload_json;
      out.hash = appended.value().hash;
    }
  } catch (...) {
    out.threw = true;  // pre-IMP-17: this is where json::type_error.316 landed
  }
  return out;
}

}  // namespace

TEST_CASE("IMP-17: ill-formed UTF-8 on the order hot path APPENDS and NEVER THROWS",
          "[intentlog][utf8]") {
  TempLog tmp("utf8_nothrow");
  TestClock clock = clock_at_ticks(0);

  auto opened = IntentLog::open(tmp.path, clock);
  REQUIRE(opened.has_value());
  IntentLog log = std::move(opened.value());

  // (b) THE NO-THROW CONTRACT, ASSERTED EXPLICITLY — the idiomatic form first, on
  // the shape that reproduced it: `j["client_ref"] = "al\x80pha..."` then dump().
  // REQUIRE_NOTHROW is the whole point of this line; do not "simplify" it away.
  REQUIRE_NOTHROW(log.append(IntentOp::PlaceOrder, std::string(kBadStrategyRef), "{}"));

  // ...and then per-append, so "did not throw" and "was actually RECORDED" are
  // asserted on the SAME call for all three ill-formed shapes in BOTH of the
  // caller-supplied fields (client_ref — whose first segment is the strategy
  // name — and payload_json, which is where a symbol travels).
  const AppendOutcome sym =
      append_guarded(log, IntentOp::PlaceOrder, "ref-symbol", kBadSymbolPayload);
  const AppendOutcome trunc_ref =
      append_guarded(log, IntentOp::ModifyOrder, kTruncatedRun, "{}");
  const AppendOutcome trunc_payload =
      append_guarded(log, IntentOp::CancelOrder, "ref-trunc-payload", kTruncatedSymbolPayload);
  const AppendOutcome bad_lead_ref = append_guarded(log, IntentOp::Result, kInvalidLead, "{}");
  const AppendOutcome lone_payload =
      append_guarded(log, IntentOp::SquareOff, "ref-lone-payload", kLoneContinuation);

  for (const AppendOutcome* o :
       {&sym, &trunc_ref, &trunc_payload, &bad_lead_ref, &lone_payload}) {
    CHECK_FALSE(o->threw);  // (b) NO-THROW across the Result<T> boundary
    CHECK(o->ok);           // (a) NORMALISED AND RECORDED, never rejected
    CHECK(o->hash.size() == 64);
  }

  // (a) Losing an order's write-ahead record because a strategy name carried a
  // stray byte would leave a possibly-sent order un-enumerable, which is the exact
  // failure this log exists to prevent — so all six are on the chain.
  REQUIRE(log.next_seq() == 7);

  // The stored text is the canonical form, byte-exactly: ONE U+FFFD per MAXIMAL
  // subpart (the truncated two-byte run yields ONE replacement, not two).
  CHECK(sym.payload_json == kBadSymbolPayloadCanonical);
  CHECK(trunc_ref.client_ref == kTruncatedRunCanonical);
  CHECK(trunc_payload.payload_json == kTruncatedSymbolPayloadCanonical);
  CHECK(bad_lead_ref.client_ref == kInvalidLeadCanonical);
  CHECK(lone_payload.payload_json == kLoneContinuationCanonical);

  const auto first = log.last_for(kBadStrategyRef);
  REQUIRE(first.has_value());
  CHECK(first.value().client_ref == kBadStrategyRefCanonical);

  // THE INVARIANT, ASSERTED DIRECTLY AGAINST THE FILE: the bytes on disk ARE the
  // bytes that went into the hash. (This is what a bare ::replace would have
  // broken: the file would hold U+FFFD while the chain hashed the raw byte.)
  const std::vector<nlohmann::json> on_disk = stored_lines(tmp.path);
  REQUIRE(on_disk.size() == 6);
  CHECK(on_disk[0].at("client_ref").get<std::string>() == kBadStrategyRefCanonical);
  CHECK(on_disk[1].at("payload").get<std::string>() == kBadSymbolPayloadCanonical);
  CHECK(on_disk[2].at("client_ref").get<std::string>() == kTruncatedRunCanonical);
  CHECK(on_disk[3].at("payload").get<std::string>() == kTruncatedSymbolPayloadCanonical);
  CHECK(on_disk[4].at("client_ref").get<std::string>() == kInvalidLeadCanonical);
  CHECK(on_disk[5].at("payload").get<std::string>() == kLoneContinuationCanonical);
  // Every hash on disk is the hash the record carried in memory — preimage and
  // stored bytes are one string.
  CHECK(on_disk[1].at("hash").get<std::string>() == sym.hash);
  CHECK(on_disk[5].at("hash").get<std::string>() == lone_payload.hash);
}

TEST_CASE("IMP-17: ill-formed records survive a write/reload cycle and the chain VERIFIES",
          "[intentlog][utf8][replay]") {
  TempLog tmp("utf8_reload");
  std::vector<std::string> hashes;

  // Session 1: write the three ill-formed shapes across both caller fields.
  {
    TestClock clock = clock_at_ticks(0);
    auto opened = IntentLog::open(tmp.path, clock);
    REQUIRE(opened.has_value());
    IntentLog log = std::move(opened.value());
    const std::string_view refs[] = {kBadStrategyRef, kInvalidLead, kTruncatedRun};
    const std::string_view payloads[] = {kBadSymbolPayload, kTruncatedSymbolPayload,
                                         kLoneContinuation};
    for (std::size_t i = 0; i < 3; ++i) {
      auto r = log.append(IntentOp::PlaceOrder, std::string(refs[i]), std::string(payloads[i]));
      REQUIRE(r.has_value());
      hashes.push_back(r.value().hash);
    }
  }

  // Session 2 ("restart"): (c) it reloads, and (d) replay() — which recomputes
  // every record's SHA-256 over its canonical bytes and re-links the chain — is
  // CLEAN. THIS IS WHAT WOULD HAVE FAILED had we only switched the dump handler:
  // the bytes read back would not be the bytes hashed, and every one of these
  // records would report "hash mismatch — record tampered" on its own log.
  {
    TestClock clock = clock_at_ticks(0);
    auto opened = IntentLog::open(tmp.path, clock);
    REQUIRE(opened.has_value());
    IntentLog log = std::move(opened.value());

    auto replayed = log.replay();
    REQUIRE(replayed.has_value());
    const std::vector<IntentRecord>& recs = replayed.value();
    REQUIRE(recs.size() == 3);
    for (std::size_t i = 0; i < recs.size(); ++i) {
      CHECK(recs[i].seq == static_cast<std::int64_t>(i + 1));
      CHECK(recs[i].hash == hashes[i]);  // identical hash across the restart
    }
    CHECK(recs[0].prev_hash == "GENESIS");
    CHECK(recs[1].prev_hash == recs[0].hash);
    CHECK(recs[2].prev_hash == recs[1].hash);
    CHECK(recs[0].client_ref == kBadStrategyRefCanonical);
    CHECK(recs[0].payload_json == kBadSymbolPayloadCanonical);

    // The idempotency lookup still resolves from the RAW bytes the caller holds —
    // last_for() canonicalises its query exactly as append() canonicalised the
    // stored ref, so restart-dedup cannot silently miss and place a second order.
    REQUIRE(log.last_for(kBadStrategyRef).has_value());
    CHECK(log.last_for(kBadStrategyRef).value().seq == 1);
    // ...and equally from the canonical form (canonical_text is idempotent).
    REQUIRE(log.last_for(kBadStrategyRefCanonical).has_value());
    CHECK(log.last_for(kBadStrategyRefCanonical).value().seq == 1);

    // A tamper is STILL a tamper: the chain check is not weakened by any of this.
    CHECK(log.next_seq() == 4);
  }
}

TEST_CASE("IMP-17: valid UTF-8 hashes BIT-IDENTICALLY — no existing record moves",
          "[intentlog][utf8][compat]") {
  TempLog tmp("utf8_identity");
  TestClock clock = clock_at_ticks(0);  // wall_ts_ns == 0 on EVERY platform,
                                        // whatever system_clock's tick period is
  auto opened = IntentLog::open(tmp.path, clock);
  REQUIRE(opened.has_value());
  IntentLog log = std::move(opened.value());

  // THE BACKWARD-COMPATIBILITY GUARD. This is sha256 of the FROZEN canonical byte
  // stream for a plain ASCII genesis record:
  //
  //   "v1\ns1\noplace_order\nt0\nc9:ref-alpha\np9:{\"qty\":1}\nh7:GENESIS\n"
  //
  // Pinned as a LITERAL so that if canonicalisation ever touched a well-formed
  // byte — or if the framing/ordering of the canonical form drifted — this fails
  // loudly here instead of silently invalidating every intent log ever written.
  constexpr std::string_view kGenesisAsciiHash =
      "a013641f30f6194b3c79bfeaa0299733c9f5d351be383b4734e809b582d59e25";

  auto genesis = log.append(IntentOp::PlaceOrder, "ref-alpha", R"({"qty":1})");
  REQUIRE(genesis.has_value());
  CHECK(genesis.value().client_ref == "ref-alpha");            // untouched
  CHECK(genesis.value().payload_json == R"({"qty":1})");       // untouched
  CHECK(genesis.value().wall_ts_ns == 0);
  CHECK(genesis.value().hash == kGenesisAsciiHash);

  // WELL-FORMED MULTI-BYTE text is equally untouched: U+20B9 INDIAN RUPEE SIGN
  // (E2 82 B9) inside a payload, U+00E9 (C3 A9) inside a ref. Normalisation
  // rewrites ill-formed sequences ONLY. (Spelled as hex escapes, never as literal
  // non-ASCII source characters, so the assertion cannot depend on this file's
  // encoding or on the compiler's execution charset.)
  auto rupee = log.append(IntentOp::Result, "caf\xC3\xA9" "-1a2b3c4d-0002",
                          R"({"exposure":")" "\xE2\x82\xB9" R"("})");
  REQUIRE(rupee.has_value());
  CHECK(rupee.value().client_ref == "caf\xC3\xA9" "-1a2b3c4d-0002");
  CHECK(rupee.value().payload_json.find(kReplacementChar) == std::string::npos);
  CHECK(rupee.value().payload_json.find("\xE2\x82\xB9") != std::string::npos);

  // And it all still replays.
  auto replayed = log.replay();
  REQUIRE(replayed.has_value());
  REQUIRE(replayed.value().size() == 2);
  CHECK(replayed.value()[0].hash == kGenesisAsciiHash);
}

TEST_CASE("IMP-17: normalisation is IDEMPOTENT — the canonical form hashes the same",
          "[intentlog][utf8]") {
  // Appending the ILL-FORMED text and appending its ALREADY-CANONICAL form must
  // produce the same stored bytes AND the same hash. That is what "normalise
  // exactly once, up front" buys: a second pass anywhere is provably a no-op, so
  // no later transform can move the preimage out from under the chain.
  TempLog raw_log("utf8_idem_raw");
  TempLog pre_log("utf8_idem_pre");
  TestClock clock_a = clock_at_ticks(0);
  TestClock clock_b = clock_at_ticks(0);

  auto a = IntentLog::open(raw_log.path, clock_a);
  auto b = IntentLog::open(pre_log.path, clock_b);
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  IntentLog raw = std::move(a.value());
  IntentLog pre = std::move(b.value());

  auto ra = raw.append(IntentOp::PlaceOrder, std::string(kTruncatedRun),
                       std::string(kBadSymbolPayload));
  auto rb = pre.append(IntentOp::PlaceOrder, std::string(kTruncatedRunCanonical),
                       std::string(kBadSymbolPayloadCanonical));
  REQUIRE(ra.has_value());
  REQUIRE(rb.has_value());
  CHECK(ra.value().client_ref == rb.value().client_ref);
  CHECK(ra.value().payload_json == rb.value().payload_json);
  CHECK(ra.value().hash == rb.value().hash);
}

TEST_CASE("IMP-17: a genuine edit to a normalised record is STILL detected",
          "[intentlog][utf8][tamper]") {
  // Normalisation must not buy an attacker anything: a record whose payload
  // contains U+FFFD is exactly as tamper-evident as any other.
  TempLog tmp("utf8_tamper");
  {
    TestClock clock = clock_at_ticks(0);
    auto opened = IntentLog::open(tmp.path, clock);
    REQUIRE(opened.has_value());
    IntentLog log = std::move(opened.value());
    REQUIRE(log.append(IntentOp::PlaceOrder, "ref-a", R"({"qty":5})").has_value());
    REQUIRE(log.append(IntentOp::PlaceOrder, "ref-b", std::string(kLoneContinuation))
                .has_value());
    REQUIRE(log.append(IntentOp::CancelOrder, "ref-c", "{}").has_value());
  }

  std::string contents;
  {
    std::ifstream in(tmp.path, std::ios::binary);
    REQUIRE(in.good());
    contents.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  const auto pos = contents.find("lone");
  REQUIRE(pos != std::string::npos);
  contents.replace(pos, 4, "lonX");  // payload-only edit, hash left untouched
  {
    std::ofstream out(tmp.path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  }

  TestClock clock = clock_at_ticks(0);
  auto opened = IntentLog::open(tmp.path, clock);
  REQUIRE(opened.has_value());
  IntentLog log = std::move(opened.value());
  auto replayed = log.replay();
  REQUIRE_FALSE(replayed.has_value());
  CHECK(replayed.error().category == broker_exec::errors::ErrorCategory::Internal);
  CHECK(replayed.error().message.find("seq 2") != std::string::npos);
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
