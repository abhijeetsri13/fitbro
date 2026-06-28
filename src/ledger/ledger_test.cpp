#include "broker_exec/ledger/ledger.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/result.hpp"

namespace fs = std::filesystem;

using broker_exec::clock::TestClock;
using broker_exec::errors::ErrorCategory;
using broker_exec::ledger::Ed25519KeyPair;
using broker_exec::ledger::Ledger;
using broker_exec::ledger::PositionHeartbeat;

namespace {

// A 32-char alphanumeric secret-shaped token: >=20 chars mixing letters AND
// digits, so domain::scrub's bare high-entropy rule redacts it. Used to prove
// scrub-before-persist (no token in the chain, the file, or the heartbeat).
constexpr std::string_view kToken = "Ab12Cd34Ef56Gh78Ij90Kl12Mn34Op56";

// A unique temp directory, recursively removed on scope exit so runs never
// collide and leave no artifacts behind (mirrors the secrets test).
struct TempDir {
  fs::path path;

  explicit TempDir(const std::string& tag)
      : path(fs::temp_directory_path() /
             ("broker_exec_ledger_" + tag + "_" +
              std::to_string(reinterpret_cast<std::uintptr_t>(this)))) {
    std::error_code ec;
    fs::create_directories(path, ec);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

[[nodiscard]] std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("append builds a linked hash chain that verifies", "[ledger]") {
  const TempDir dir("chain");
  const TestClock clock;
  Ledger ledger(clock, dir.path / "ledger.jsonl");

  const auto e0 = ledger.append("payload-alpha");
  const auto e1 = ledger.append("payload-bravo");
  const auto e2 = ledger.append("payload-charlie");
  REQUIRE(e0.has_value());
  REQUIRE(e1.has_value());
  REQUIRE(e2.has_value());

  CHECK(e0.value().seq == 0);
  CHECK(e1.value().seq == 1);
  CHECK(e2.value().seq == 2);

  // Genesis has no predecessor; each subsequent prev_hash links to the prior hash.
  CHECK(e0.value().prev_hash.empty());
  CHECK(e1.value().prev_hash == e0.value().hash);
  CHECK(e2.value().prev_hash == e1.value().hash);

  CHECK(ledger.size() == 3);
  CHECK(ledger.head_hash() == e2.value().hash);
  CHECK(ledger.verify_chain().has_value());
}

TEST_CASE("a non-key-holder file edit breaks the chain at the bad seq (AC-1)", "[ledger]") {
  const TempDir dir("tamper");
  const fs::path file = dir.path / "ledger.jsonl";

  {
    const TestClock clock;
    Ledger ledger(clock, file);
    REQUIRE(ledger.append("payload-alpha").has_value());
    REQUIRE(ledger.append("payload-bravo").has_value());
    REQUIRE(ledger.append("payload-charlie").has_value());
  }

  // Byte-edit the middle entry's payload directly in the FILE — a non-key-holder
  // corruption. The line stays valid JSON (so load() succeeds), but the stored
  // hash no longer matches the recomputed hash over the mutated payload.
  std::string content = read_file(file);
  const auto pos = content.find("bravo");
  REQUIRE(pos != std::string::npos);
  content.replace(pos, 5, "bravX");
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << content;
  }

  const TestClock clock;
  Ledger fresh(clock, file);
  REQUIRE(fresh.load().has_value());  // load proves the file rebuilds
  const auto verified = fresh.verify_chain();
  REQUIRE_FALSE(verified.has_value());
  CHECK(verified.error().category == ErrorCategory::Validation);
  // The Error names the first bad seq (the middle entry, seq 1).
  CHECK(verified.error().message.find("seq 1") != std::string::npos);
}

TEST_CASE("the ledger survives a restart (persist + reload)", "[ledger]") {
  const TempDir dir("reload");
  const fs::path file = dir.path / "ledger.jsonl";

  std::string head;
  {
    const TestClock clock;
    Ledger ledger(clock, file);
    REQUIRE(ledger.append("payload-alpha").has_value());
    REQUIRE(ledger.append("payload-bravo").has_value());
    head = ledger.head_hash();
  }

  const TestClock clock;
  Ledger fresh(clock, file);
  REQUIRE(fresh.load().has_value());
  CHECK(fresh.size() == 2);
  CHECK(fresh.head_hash() == head);
  CHECK(fresh.verify_chain().has_value());
}

TEST_CASE("Ed25519 sign/verify the chain head, fail-closed on wrong key/tampered head", "[ledger]") {
  const TempDir dir("ed25519");
  const TestClock clock;
  Ledger ledger(clock, dir.path / "ledger.jsonl");

  const auto kp_result = Ledger::generate_keypair();
  REQUIRE(kp_result.has_value());
  const Ed25519KeyPair kp = kp_result.value();
  CHECK(kp.public_key.size() == 32);
  CHECK(kp.private_key.size() == 32);

  REQUIRE(ledger.append("payload-alpha").has_value());
  REQUIRE(ledger.append("payload-bravo").has_value());

  const auto sig = ledger.sign_head(kp.private_key);
  REQUIRE(sig.has_value());

  // Correct key + correct head verifies.
  CHECK(Ledger::verify_head(ledger.head_hash(), sig.value(), kp.public_key).has_value());

  // A DIFFERENT keypair's public key fails closed.
  const auto other = Ledger::generate_keypair();
  REQUIRE(other.has_value());
  CHECK_FALSE(Ledger::verify_head(ledger.head_hash(), sig.value(), other.value().public_key)
                  .has_value());

  // A tampered head fails closed.
  CHECK_FALSE(Ledger::verify_head("deadbeef", sig.value(), kp.public_key).has_value());

  // Signing an empty ledger is a defined Error.
  Ledger empty(clock, dir.path / "empty.jsonl");
  CHECK_FALSE(empty.sign_head(kp.private_key).has_value());
}

TEST_CASE("key-mismatch is a fail-closed safe-start blocker (AC-3)", "[ledger]") {
  const auto a = Ledger::generate_keypair();
  const auto b = Ledger::generate_keypair();
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());

  CHECK(Ledger::require_key_match(a.value().public_key, a.value().public_key).has_value());
  const auto mismatch = Ledger::require_key_match(a.value().public_key, b.value().public_key);
  REQUIRE_FALSE(mismatch.has_value());
  CHECK(mismatch.error().category == ErrorCategory::Auth);
}

TEST_CASE("EOD report bundles count/head/signature/public key and persists the key (AC-1)",
          "[ledger]") {
  const TempDir dir("eod");
  const TestClock clock;
  Ledger ledger(clock, dir.path / "ledger.jsonl");

  const auto kp = Ledger::generate_keypair();
  REQUIRE(kp.has_value());

  REQUIRE(ledger.append("payload-alpha").has_value());
  REQUIRE(ledger.append("payload-bravo").has_value());
  REQUIRE(ledger.append("payload-charlie").has_value());

  const auto report = ledger.eod_report(kp.value().private_key, kp.value().public_key);
  REQUIRE(report.has_value());
  CHECK(report.value().entry_count == 3);
  CHECK(report.value().head_hash == ledger.head_hash());
  CHECK(report.value().public_key == kp.value().public_key);
  // The bundled signature verifies under the bundled public key.
  CHECK(Ledger::verify_head(report.value().head_hash, report.value().signature,
                            report.value().public_key)
            .has_value());
  // to_json is non-throwing and carries the head hash.
  CHECK(report.value().to_json().find(ledger.head_hash()) != std::string::npos);

  // write_public_key persists <dir>/ledger_public_key.hex == hex(public_key).
  REQUIRE(ledger.write_public_key(kp.value().public_key, dir.path).has_value());
  const fs::path key_file = dir.path / "ledger_public_key.hex";
  REQUIRE(fs::exists(key_file));
  std::string expected_hex;
  for (unsigned char byte : kp.value().public_key) {
    static constexpr char d[] = "0123456789abcdef";
    expected_hex.push_back(d[byte >> 4]);
    expected_hex.push_back(d[byte & 0x0F]);
  }
  CHECK(read_file(key_file) == expected_hex);
}

TEST_CASE("the heartbeat carries ts + exposure and scrubs secrets (AC-2)", "[ledger]") {
  const TestClock clock;
  const auto ts = clock.now_wall();
  const std::string summary = std::string("net=+50 exposure=") + std::string(kToken);

  const PositionHeartbeat hb = Ledger::make_heartbeat(summary, ts);
  const std::string json = hb.to_json();

  CHECK(json.find("net=+50") != std::string::npos);  // exposure preserved
  CHECK(json.find(hb.ts) != std::string::npos);       // timestamp present
  // The token is scrubbed before it reaches the operator sink.
  CHECK(hb.exposure.find(kToken) == std::string::npos);
  CHECK(json.find(kToken) == std::string::npos);
}

TEST_CASE("a wrong-TYPE field on a middle line is a non-throwing Validation Error (AC-1)",
          "[ledger]") {
  const TempDir dir("wrongtype");
  const fs::path file = dir.path / "ledger.jsonl";

  {
    const TestClock clock;
    Ledger ledger(clock, file);
    REQUIRE(ledger.append("payload-alpha").has_value());
    REQUIRE(ledger.append("payload-bravo").has_value());
    REQUIRE(ledger.append("payload-charlie").has_value());
  }

  // Rewrite the MIDDLE line to valid JSON whose `payload` is a NUMBER, not a
  // string. nlohmann value()/get<string> would throw type_error.302 on it; a
  // hardened load() must type-check the field and return a Validation Error
  // (never throw across the no-throw Result boundary).
  std::vector<std::string> lines;
  {
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      lines.push_back(line);
    }
  }
  REQUIRE(lines.size() == 3);
  lines[1] = R"({"seq":1,"prev_hash":"","payload":123,"hash":"x"})";
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    for (const auto& l : lines) {
      out << l << '\n';
    }
  }

  const TestClock clock;
  Ledger fresh(clock, file);
  REQUIRE_NOTHROW(fresh.load());  // the wrong-type field must not escape as a throw
  const auto loaded = fresh.load();
  REQUIRE_FALSE(loaded.has_value());
  CHECK(loaded.error().category == ErrorCategory::Validation);
}

TEST_CASE("a torn trailing line is skipped but the same garbage mid-file is fatal (AC-1)",
          "[ledger]") {
  const TempDir dir("torn");
  const fs::path file = dir.path / "ledger.jsonl";

  {
    const TestClock clock;
    Ledger ledger(clock, file);
    REQUIRE(ledger.append("payload-alpha").has_value());
    REQUIRE(ledger.append("payload-bravo").has_value());
  }
  const std::string good = read_file(file);  // two complete, newline-terminated lines

  // A partial/garbage LAST line — a trailing append that never fsynced — is
  // tolerated: load() succeeds, the torn record is skipped (size stays 2), and
  // the chain still verifies.
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << good << R"({"seq":2,"prev_hash":)";  // truncated, no trailing newline
  }
  {
    const TestClock clock;
    Ledger fresh(clock, file);
    REQUIRE(fresh.load().has_value());
    CHECK(fresh.size() == 2);
    CHECK(fresh.verify_chain().has_value());
  }

  // The SAME garbage placed in the MIDDLE (a real mid-file tamper) stays fatal.
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << R"({"seq":0,"prev_hash":)" << '\n' << good;  // garbage first, good lines after
  }
  {
    const TestClock clock;
    Ledger fresh(clock, file);
    const auto loaded = fresh.load();
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().category == ErrorCategory::Validation);
  }
}

TEST_CASE("verify_head fails closed on a wrong-length signature (no crash)", "[ledger]") {
  const TempDir dir("badsig");
  const TestClock clock;
  Ledger ledger(clock, dir.path / "ledger.jsonl");

  const auto kp = Ledger::generate_keypair();
  REQUIRE(kp.has_value());
  REQUIRE(ledger.append("payload-alpha").has_value());

  // A signature of the wrong size (10 bytes, not Ed25519's 64) must be rejected
  // as a defined Error: OpenSSL returns non-1 and we fail closed, never crash.
  const std::vector<unsigned char> bad_sig(10, 0x00);
  REQUIRE_NOTHROW(Ledger::verify_head(ledger.head_hash(), bad_sig, kp.value().public_key));
  const auto verified = Ledger::verify_head(ledger.head_hash(), bad_sig, kp.value().public_key);
  REQUIRE_FALSE(verified.has_value());
  CHECK(verified.error().category == ErrorCategory::Validation);
}

TEST_CASE("an appended secret is scrubbed before hashing/persist (4.2 lesson)", "[ledger]") {
  const TempDir dir("redaction");
  const fs::path file = dir.path / "ledger.jsonl";
  const TestClock clock;
  Ledger ledger(clock, file);

  const std::string payload = std::string("order token=") + std::string(kToken);
  const auto entry = ledger.append(payload);
  REQUIRE(entry.has_value());

  // The token never lands in the stored payload (scrub ran before hash/persist).
  CHECK(entry.value().payload.find(kToken) == std::string::npos);
  // ...nor in the on-disk file.
  CHECK(read_file(file).find(kToken) == std::string::npos);
  // ...and the chain still verifies (the hash is over the SCRUBBED payload).
  CHECK(ledger.verify_chain().has_value());
}
