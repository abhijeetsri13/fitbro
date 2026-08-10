#include "broker_exec/ledger/ledger.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

// IMP-17: the tests assert the dump-identity claim against the REAL serialiser,
// so they use nlohmann directly rather than trusting a paraphrase of it.
#include <nlohmann/json.hpp>

#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/domain/redaction.hpp"  // kRedactionMarker, for the IMP-16 assertions
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

void write_file(const fs::path& p, const std::string& content) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << content;
}

// Append each payload to the ledger, one entry per element.
void append_all(Ledger& ledger, std::initializer_list<std::string_view> payloads) {
  for (std::string_view payload : payloads) {
    REQUIRE(ledger.append(std::string(payload)).has_value());
  }
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

TEST_CASE("write_checkpoint then verify_against_checkpoint passes; high-water advances", "[ledger]") {
  const TempDir dir("checkpoint_intact");
  const TestClock clock;
  Ledger ledger(clock, dir.path / "ledger.jsonl");

  const auto kp = Ledger::generate_keypair();
  REQUIRE(kp.has_value());

  append_all(ledger, {"p0", "p1", "p2", "p3", "p4"});
  REQUIRE(ledger.size() == 5);
  REQUIRE(ledger.write_checkpoint(kp.value().private_key, kp.value().public_key).has_value());
  CHECK(ledger.verify_against_checkpoint(kp.value().public_key).has_value());

  // The high-water mark advances: append 3 more, re-checkpoint at size 8, verify.
  append_all(ledger, {"p5", "p6", "p7"});
  REQUIRE(ledger.size() == 8);
  REQUIRE(ledger.write_checkpoint(kp.value().private_key, kp.value().public_key).has_value());
  CHECK(ledger.verify_against_checkpoint(kp.value().public_key).has_value());
}

TEST_CASE("a chain that only GREW since the checkpoint still verifies", "[ledger]") {
  const TempDir dir("checkpoint_growth");
  const TestClock clock;
  Ledger ledger(clock, dir.path / "ledger.jsonl");

  const auto kp = Ledger::generate_keypair();
  REQUIRE(kp.has_value());

  append_all(ledger, {"p0", "p1", "p2", "p3", "p4"});
  REQUIRE(ledger.write_checkpoint(kp.value().private_key, kp.value().public_key).has_value());

  // Append 3 more WITHOUT a new checkpoint: the size-5 checkpoint must still
  // verify because entry[4] is unchanged and the chain only grew.
  append_all(ledger, {"p5", "p6", "p7"});
  REQUIRE(ledger.size() == 8);
  CHECK(ledger.verify_against_checkpoint(kp.value().public_key).has_value());
}

TEST_CASE("a chopped tail is detected against the signed checkpoint (truncation)", "[ledger]") {
  const TempDir dir("checkpoint_truncate");
  const fs::path file = dir.path / "ledger.jsonl";
  const TestClock clock;

  const auto kp = Ledger::generate_keypair();
  REQUIRE(kp.has_value());

  {
    Ledger ledger(clock, file);
    append_all(ledger, {"p0", "p1", "p2", "p3", "p4"});
    REQUIRE(ledger.write_checkpoint(kp.value().private_key, kp.value().public_key).has_value());
  }

  // Simulate a tail chop: keep only the first 3 of the 5 ledger lines. The
  // size-5 checkpoint sibling is left in place.
  std::vector<std::string> lines;
  {
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      lines.push_back(line);
    }
  }
  REQUIRE(lines.size() == 5);
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    for (std::size_t i = 0; i < 3; ++i) {
      out << lines[i] << '\n';
    }
  }

  Ledger fresh(clock, file);
  REQUIRE(fresh.load().has_value());
  CHECK(fresh.size() == 3);
  CHECK(fresh.verify_chain().has_value());  // the surviving 3 are self-consistent
  const auto verified = fresh.verify_against_checkpoint(kp.value().public_key);
  REQUIRE_FALSE(verified.has_value());      // ...but truncation IS caught
  CHECK(verified.error().category == ErrorCategory::Validation);
  CHECK(verified.error().message.find("TRUNCATED") != std::string::npos);
}

TEST_CASE("a substituted/rolled-back chain is detected against the checkpoint", "[ledger]") {
  const TempDir dir("checkpoint_rollback");
  const fs::path file = dir.path / "ledger.jsonl";
  const fs::path other = dir.path / "other.jsonl";
  const TestClock clock;

  const auto kp = Ledger::generate_keypair();
  REQUIRE(kp.has_value());

  // Chain A: signs a size-5 checkpoint into file's sibling.
  {
    Ledger ledger_a(clock, file);
    append_all(ledger_a, {"A0", "A1", "A2", "A3", "A4"});
    REQUIRE(ledger_a.write_checkpoint(kp.value().private_key, kp.value().public_key).has_value());
  }

  // Chain B: a DIFFERENT 5-entry chain (different payloads => different hash at
  // seq 4). Overwrite the ledger file with B's lines, leaving A's checkpoint.
  {
    Ledger ledger_b(clock, other);
    append_all(ledger_b, {"B0", "B1", "B2", "B3", "B4"});
  }
  write_file(file, read_file(other));

  Ledger fresh(clock, file);
  REQUIRE(fresh.load().has_value());
  CHECK(fresh.size() == 5);
  CHECK(fresh.verify_chain().has_value());  // chain B is internally consistent
  const auto verified = fresh.verify_against_checkpoint(kp.value().public_key);
  REQUIRE_FALSE(verified.has_value());      // ...but it is NOT the signed chain
  CHECK(verified.error().category == ErrorCategory::Validation);
  CHECK(verified.error().message.find("ROLLED BACK") != std::string::npos);
}

TEST_CASE("a forged (byte-flipped) checkpoint fails closed", "[ledger]") {
  const TempDir dir("checkpoint_forged");
  const fs::path file = dir.path / "ledger.jsonl";
  const TestClock clock;

  const auto kp = Ledger::generate_keypair();
  REQUIRE(kp.has_value());

  Ledger ledger(clock, file);
  append_all(ledger, {"p0", "p1", "p2", "p3", "p4"});
  REQUIRE(ledger.write_checkpoint(kp.value().private_key, kp.value().public_key).has_value());

  // Flip one hex digit inside the checkpoint's signature on disk: still valid
  // JSON and valid hex, but the signature no longer verifies.
  fs::path cp = file;
  cp += ".checkpoint";
  REQUIRE(fs::exists(cp));
  std::string content = read_file(cp);
  const auto pos = content.find("\"signature\":\"");
  REQUIRE(pos != std::string::npos);
  const std::size_t digit = pos + std::string("\"signature\":\"").size();
  REQUIRE(digit < content.size());
  content[digit] = (content[digit] == '0') ? '1' : '0';  // a different, valid hex digit
  write_file(cp, content);

  const auto verified = ledger.verify_against_checkpoint(kp.value().public_key);
  REQUIRE_FALSE(verified.has_value());
  CHECK(verified.error().category == ErrorCategory::Validation);
  CHECK(verified.error().message.find("signature invalid") != std::string::npos);
}

TEST_CASE("absent checkpoint is ok (documented no-baseline default)", "[ledger]") {
  const TempDir dir("checkpoint_absent");
  const TestClock clock;
  Ledger ledger(clock, dir.path / "ledger.jsonl");

  const auto kp = Ledger::generate_keypair();
  REQUIRE(kp.has_value());

  append_all(ledger, {"p0", "p1", "p2"});
  // No checkpoint was ever written -> no prior signed state to measure against.
  CHECK(ledger.verify_against_checkpoint(kp.value().public_key).has_value());
}

TEST_CASE("checkpointing an empty ledger is a defined Error", "[ledger]") {
  const TempDir dir("checkpoint_empty");
  const TestClock clock;
  Ledger ledger(clock, dir.path / "ledger.jsonl");

  const auto kp = Ledger::generate_keypair();
  REQUIRE(kp.has_value());

  const auto written = ledger.write_checkpoint(kp.value().private_key, kp.value().public_key);
  REQUIRE_FALSE(written.has_value());
  CHECK(written.error().category == ErrorCategory::Validation);
  CHECK(written.error().message.find("empty") != std::string::npos);
}

TEST_CASE("KEY-SUBSTITUTION attack is caught by pinning (a self-consistent checkpoint under a "
          "DIFFERENT key is rejected)",
          "[ledger]") {
  const TempDir dir("checkpoint_substitution");
  const TestClock clock;
  Ledger ledger(clock, dir.path / "ledger.jsonl");

  // The legitimate, pinned key (e.g. from the EOD report / ledger_public_key.hex).
  const auto pinned = Ledger::generate_keypair();
  REQUIRE(pinned.has_value());
  // The ATTACKER's freshly generated key — they do NOT hold the pinned private key.
  const auto attacker = Ledger::generate_keypair();
  REQUIRE(attacker.has_value());

  append_all(ledger, {"p0", "p1", "p2"});
  // The attacker writes a checkpoint that is PERFECTLY self-consistent under their
  // OWN key (signature verifies, size + head match the on-disk chain). Pre-pinning
  // this passed; with key pinning it must be rejected because the embedded key is
  // not the pinned key.
  REQUIRE(
      ledger.write_checkpoint(attacker.value().private_key, attacker.value().public_key).has_value());

  const auto verified = ledger.verify_against_checkpoint(pinned.value().public_key);
  REQUIRE_FALSE(verified.has_value());
  CHECK(verified.error().category == ErrorCategory::Validation);
  CHECK(verified.error().message.find("pinned key") != std::string::npos);
}

TEST_CASE("a present-but-malformed checkpoint fails closed (not ok)", "[ledger]") {
  const TempDir dir("checkpoint_malformed");
  const fs::path file = dir.path / "ledger.jsonl";
  const TestClock clock;

  const auto kp = Ledger::generate_keypair();
  REQUIRE(kp.has_value());

  Ledger ledger(clock, file);
  append_all(ledger, {"p0", "p1", "p2"});
  REQUIRE(ledger.write_checkpoint(kp.value().private_key, kp.value().public_key).has_value());

  fs::path cp = file;
  cp += ".checkpoint";
  REQUIRE(fs::exists(cp));
  // Garbage that is present but not parseable -> a present checkpoint that won't
  // parse is itself evidence of tamper; must fail CLOSED, never ok().
  write_file(cp, "this is not json {");

  const auto verified = ledger.verify_against_checkpoint(kp.value().public_key);
  REQUIRE_FALSE(verified.has_value());
  CHECK(verified.error().category == ErrorCategory::Validation);
  CHECK(verified.error().message.find("malformed") != std::string::npos);
}

// ── IMP-16: typed provenance on a ledger entry ───────────────────────────────
//
// The defect: append() scrubs its FREE-FORM payload, and a minted client_ref is
// one long token-shaped run — so a payload carrying `client_ref=<ref>` was
// PERSISTED AND HASHED as `client_ref=***REDACTED***`, and the tamper-evident
// chain could not be joined back to the log, the store or the intent log. The fix
// passes the ids beside the payload as typed columns. The payload's own scrubbing
// is UNCHANGED (re-asserted below).

namespace {

// A real minted client_ref, EXACTLY as make_client_ref() spells it:
// `<strategy>-<8 hex sig>-<canonical RFC-4122 v4 uuid>` (idempotency/uuid.cpp).
constexpr std::string_view kMintedRef = "alpha-1a2b3c4d-deadbeef-cafe-4bab-8abe-0123456789ab";

[[nodiscard]] broker_exec::ledger::ProvenanceContext order_ctx() {
  broker_exec::ledger::ProvenanceContext ctx;
  ctx.client_ref = std::string(kMintedRef);
  ctx.broker_order_id = "240627000123456";  // a Kite 15-digit order id
  ctx.strategy = "alpha";
  return ctx;
}

}  // namespace

TEST_CASE("IMP-16: a REAL minted client_ref survives a ledger entry end-to-end",
          "[ledger][provenance]") {
  const TempDir dir("provenance");
  const fs::path file = dir.path / "ledger.jsonl";
  const TestClock clock;
  Ledger ledger(clock, file);

  const auto entry = ledger.append("position closed per policy", order_ctx());
  REQUIRE(entry.has_value());

  // The ref is in the STORED payload (and therefore in the hash preimage), in the
  // structured, greppable block — this is what makes the chain joinable to the log.
  CHECK(entry.value().payload ==
        "position closed per policy"
        " [client_ref=alpha-1a2b3c4d-deadbeef-cafe-4bab-8abe-0123456789ab"
        " broker_order_id=240627000123456 strategy=alpha]");
  // ...and on disk.
  CHECK(read_file(file).find(kMintedRef) != std::string::npos);
  // ...and the chain still verifies, because the hash is over exactly these bytes.
  CHECK(ledger.verify_chain().has_value());

  // It also survives a restart: reload from the file and re-verify.
  Ledger fresh(clock, file);
  REQUIRE(fresh.load().has_value());
  CHECK(fresh.size() == 1);
  CHECK(fresh.verify_chain().has_value());
  CHECK(fresh.head_hash() == entry.value().hash);

  // THE BASELINE THIS FIXES, pinned: the SAME ref interpolated into the FREE-FORM
  // payload is still destroyed. The exemption reaches typed columns only.
  Ledger legacy(clock, dir.path / "legacy.jsonl");
  const auto legacy_entry = legacy.append("position closed; ref=" + std::string(kMintedRef));
  REQUIRE(legacy_entry.has_value());
  CHECK(legacy_entry.value().payload.find(kMintedRef) == std::string::npos);
}

TEST_CASE("IMP-16: an EMPTY context is hash-identical to a plain append (no chain can break)",
          "[ledger][provenance]") {
  const TempDir dir("provenance_hash");
  const TestClock clock;
  Ledger one_arg(clock, dir.path / "one.jsonl");
  Ledger two_arg(clock, dir.path / "two.jsonl");

  // The load-bearing guarantee: adding the overload cannot change ANY byte of a
  // record written without provenance, so no existing on-disk chain is invalidated.
  const auto a = one_arg.append("payload-alpha");
  const auto b = two_arg.append("payload-alpha", broker_exec::ledger::ProvenanceContext{});
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  CHECK(a.value().payload == b.value().payload);
  CHECK(a.value().payload == "payload-alpha");  // no block, no trailing space
  CHECK(a.value().hash == b.value().hash);      // ...therefore the same preimage and hash
}

TEST_CASE("IMP-16: verify_chain passes over entries written BEFORE and AFTER provenance",
          "[ledger][provenance]") {
  const TempDir dir("provenance_mixed");
  const fs::path file = dir.path / "ledger.jsonl";
  const TestClock clock;

  std::string head_before;
  {
    // Phase 1 — a chain written the old way (no provenance at all).
    Ledger ledger(clock, file);
    append_all(ledger, {"p0", "p1", "p2"});
    head_before = ledger.head_hash();
    REQUIRE(ledger.verify_chain().has_value());
  }

  {
    // Phase 2 — the SAME file grown with provenance-carrying entries.
    Ledger ledger(clock, file);
    REQUIRE(ledger.load().has_value());
    CHECK(ledger.size() == 3);
    CHECK(ledger.head_hash() == head_before);  // the pre-existing tail is untouched
    CHECK(ledger.verify_chain().has_value());  // ...and still verifies as loaded

    REQUIRE(ledger.append("order filled", order_ctx()).has_value());
    REQUIRE(ledger.append("position squared off", order_ctx()).has_value());
    CHECK(ledger.size() == 5);
    CHECK(ledger.verify_chain().has_value());  // mixed old + new links verify
  }

  // Phase 3 — a cold reload of the mixed file verifies too, and the old entries
  // are byte-for-byte what they were.
  Ledger fresh(clock, file);
  REQUIRE(fresh.load().has_value());
  CHECK(fresh.size() == 5);
  CHECK(fresh.verify_chain().has_value());
  CHECK(read_file(file).find(kMintedRef) != std::string::npos);
}

TEST_CASE("IMP-16: a token in a ledger provenance column is still REDACTED (fail closed)",
          "[ledger][provenance][redaction]") {
  const TempDir dir("provenance_redaction");
  const fs::path file = dir.path / "ledger.jsonl";
  const TestClock clock;
  Ledger ledger(clock, file);

  // A credential parked in every id column, AND a credential in the free-form
  // payload: both must be gone, proving the typed block is no escape hatch and
  // that the payload's own scrubbing is unchanged.
  broker_exec::ledger::ProvenanceContext ctx;
  ctx.client_ref = std::string(kToken);
  ctx.broker_order_id = std::string(kToken);
  ctx.strategy = std::string(kToken);

  const auto entry = ledger.append(std::string("order token=") + std::string(kToken), ctx);
  REQUIRE(entry.has_value());

  CHECK(entry.value().payload.find(kToken) == std::string::npos);
  CHECK(read_file(file).find(kToken) == std::string::npos);
  CHECK(entry.value().payload.find(broker_exec::domain::kRedactionMarker) != std::string::npos);
  CHECK(ledger.verify_chain().has_value());
}

TEST_CASE("IMP-16: the heartbeat can name what it reports on, and still scrubs",
          "[ledger][provenance]") {
  const TestClock clock;
  const auto ts = clock.now_wall();

  const std::string summary = std::string("net=+50 exposure=") + std::string(kToken);
  const PositionHeartbeat hb = Ledger::make_heartbeat(summary, ts, order_ctx());

  CHECK(hb.exposure.find("net=+50") != std::string::npos);   // exposure preserved
  CHECK(hb.exposure.find(kToken) == std::string::npos);      // the secret is still scrubbed
  CHECK(hb.exposure.find(kMintedRef) != std::string::npos);  // ...and the ref now survives
  CHECK(hb.to_json().find(kMintedRef) != std::string::npos);
  CHECK(hb.to_json().find(kToken) == std::string::npos);

  // An empty context leaves the heartbeat byte-identical to the 2-argument form.
  const PositionHeartbeat plain = Ledger::make_heartbeat("net=+50", ts);
  const PositionHeartbeat plain_ctx =
      Ledger::make_heartbeat("net=+50", ts, broker_exec::ledger::ProvenanceContext{});
  CHECK(plain.exposure == plain_ctx.exposure);
  CHECK(plain.to_json() == plain_ctx.to_json());
}

// ── IMP-17: the stored bytes and the hash preimage are IDENTICAL BY CONSTRUCTION
//
// The defect: append() hashed the RAW bytes of the payload, but the line written
// to disk was serialised through nlohmann with error_handler_t::replace, which
// rewrites every ill-formed UTF-8 sequence as U+FFFD on the way out. So for a
// payload carrying an invalid byte (a lone 0x80-0xFF, a truncated multi-byte run)
// THE LINE ON DISK WAS NOT THE TEXT WE HASHED: load() read back the replaced form,
// verify_chain() recomputed sha256(prev_hash + STORED payload), the hashes
// disagreed, and the ledger reported ITSELF broken — a safe-start blocker any
// untrusted byte could trigger. Denial of audit.
//
// The fix normalises to valid UTF-8 EXACTLY ONCE, up front, so the normalised text
// IS both the preimage and the stored bytes and a later dump() has nothing left to
// replace. NORMALISE, not REJECT: dropping the append would lose the audit entry.

namespace {

// Ill-formed UTF-8, spelled BYTE-EXACTLY. Every hex escape is followed by a SPACE
// and never by a hex digit, because a C++ hex escape is greedy ("\xE2" + 'f' would
// be read as one escape, not two characters).
constexpr std::string_view kLoneContinuation = "lone \x80 byte";      // bare 0x80
constexpr std::string_view kInvalidLead = "invalid \xFF byte";        // 0xFF, never a lead
constexpr std::string_view kTruncatedRun = "truncated \xE2\x82 run";  // 2 of a 3-byte run

// The canonical form of each: ONE U+FFFD (EF BF BD) per MAXIMAL ill-formed
// subpart. Written out literally so these tests pin the exact replacement
// semantics rather than merely "something was replaced" — note the truncated
// two-byte run is ONE subpart and yields ONE replacement, not two.
constexpr std::string_view kLoneContinuationCanonical = "lone \xEF\xBF\xBD byte";
constexpr std::string_view kInvalidLeadCanonical = "invalid \xEF\xBF\xBD byte";
constexpr std::string_view kTruncatedRunCanonical = "truncated \xEF\xBF\xBD run";

// U+FFFD REPLACEMENT CHARACTER as the three bytes it actually occupies.
constexpr std::string_view kReplacementChar = "\xEF\xBF\xBD";

// VALID multi-byte UTF-8: U+20B9 INDIAN RUPEE SIGN (E2 82 B9). Must survive
// normalisation byte-identically — it is well-formed, and normalisation touches
// ill-formed sequences ONLY.
constexpr std::string_view kRupeePayload = "net exposure \xE2\x82\xB9 125000";

// sha256("payload-alpha") — the genesis hash of the oldest payload in this suite
// (prev_hash is "" at seq 0). Pinned as a LITERAL so that if normalisation ever
// touched a well-formed byte, this fails loudly instead of silently invalidating
// every ledger ever written.
constexpr std::string_view kGenesisAlphaHash =
    "3d443e312f8216a5473df30a199b4b4814c077b5bc4fa7091d6c3f997e17c210";

// The `payload` field of every line ON DISK, so a test can assert that the bytes
// in the file are the bytes we hashed.
[[nodiscard]] std::vector<std::string> stored_payloads(const fs::path& p) {
  std::vector<std::string> out;
  std::ifstream in(p, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    const nlohmann::json parsed = nlohmann::json::parse(line, nullptr, false);
    REQUIRE_FALSE(parsed.is_discarded());
    out.push_back(parsed.at("payload").get<std::string>());
  }
  return out;
}

}  // namespace

TEST_CASE("IMP-17: an ill-formed UTF-8 payload appends, reloads and STILL VERIFIES",
          "[ledger][utf8]") {
  const TempDir dir("utf8_roundtrip");
  const fs::path file = dir.path / "ledger.jsonl";
  const TestClock clock;

  std::string head;
  {
    Ledger ledger(clock, file);
    // (a) IT APPENDS. An untrusted byte is normalised, never a rejected record.
    const auto a = ledger.append(std::string(kLoneContinuation));
    const auto b = ledger.append(std::string(kInvalidLead));
    const auto c = ledger.append(std::string(kTruncatedRun));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(c.has_value());

    // The stored payload is the canonical form, byte-exactly.
    CHECK(a.value().payload == kLoneContinuationCanonical);
    CHECK(b.value().payload == kInvalidLeadCanonical);
    CHECK(c.value().payload == kTruncatedRunCanonical);

    CHECK(ledger.verify_chain().has_value());
    head = ledger.head_hash();
  }

  // (b) IT SURVIVES A WRITE/RELOAD CYCLE...
  Ledger fresh(clock, file);
  REQUIRE(fresh.load().has_value());
  CHECK(fresh.size() == 3);
  CHECK(fresh.head_hash() == head);
  // (c) ...AND PASSES verify_chain. THIS IS WHAT FAILED BEFORE IMP-17: the line on
  // disk had been silently rewritten, so the recomputed hash could not match.
  CHECK(fresh.verify_chain().has_value());

  // THE INVARIANT, ASSERTED DIRECTLY AGAINST THE FILE: the bytes on disk ARE the
  // bytes that went into the hash.
  const std::vector<std::string> on_disk = stored_payloads(file);
  REQUIRE(on_disk.size() == 3);
  CHECK(on_disk[0] == kLoneContinuationCanonical);
  CHECK(on_disk[1] == kInvalidLeadCanonical);
  CHECK(on_disk[2] == kTruncatedRunCanonical);
}

TEST_CASE("IMP-17: valid UTF-8 is byte-identical, so NO existing chain hash moves",
          "[ledger][utf8]") {
  const TempDir dir("utf8_identity");
  const TestClock clock;
  Ledger ledger(clock, dir.path / "ledger.jsonl");

  // THE BACKWARD-COMPATIBILITY GUARD: an ordinary ASCII payload hashes to exactly
  // what it hashed before IMP-17 existed.
  const auto genesis = ledger.append("payload-alpha");
  REQUIRE(genesis.has_value());
  CHECK(genesis.value().payload == "payload-alpha");
  CHECK(genesis.value().hash == kGenesisAlphaHash);

  // ...and so does WELL-FORMED MULTI-BYTE text (U+20B9 RUPEE SIGN): normalisation
  // rewrites ill-formed sequences only, so not one byte of it changes.
  const auto rupee = ledger.append(std::string(kRupeePayload));
  REQUIRE(rupee.has_value());
  CHECK(rupee.value().payload == kRupeePayload);
  CHECK(rupee.value().payload.find(kReplacementChar) == std::string::npos);
  CHECK(ledger.verify_chain().has_value());
}

TEST_CASE("IMP-17: normalisation is idempotent (normalise ONCE, up front)", "[ledger][utf8]") {
  const TempDir dir("utf8_idempotent");
  const TestClock clock;
  Ledger raw(clock, dir.path / "raw.jsonl");
  Ledger pre(clock, dir.path / "pre.jsonl");

  // Appending the ILL-FORMED text and appending its ALREADY-CANONICAL form must
  // yield the same stored bytes AND the same hash — a second pass changes nothing.
  const auto a = raw.append(std::string(kTruncatedRun));
  const auto b = pre.append(std::string(kTruncatedRunCanonical));
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  CHECK(a.value().payload == b.value().payload);
  CHECK(a.value().hash == b.value().hash);
}

TEST_CASE("IMP-17: a dump of the canonical payload is a NO-OP (the identity the fix rests on)",
          "[ledger][utf8]") {
  const TempDir dir("utf8_dump_identity");
  const TestClock clock;
  Ledger ledger(clock, dir.path / "ledger.jsonl");

  // THE CLAIM, TESTED AGAINST THE REAL SERIALISER: after normalisation, nlohmann's
  // error_handler_t::replace — the very transform that used to rewrite the payload
  // behind the hash's back — has nothing left to replace, so dump()->parse()
  // returns the identical bytes. That is what makes "stored == preimage" hold.
  for (const std::string_view payload : {kLoneContinuation, kInvalidLead, kTruncatedRun,
                                         kRupeePayload, std::string_view("plain ascii payload")}) {
    const auto entry = ledger.append(std::string(payload));
    REQUIRE(entry.has_value());

    const nlohmann::json as_json = entry.value().payload;
    const std::string dumped =
        as_json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    const nlohmann::json back = nlohmann::json::parse(dumped, nullptr, false);
    REQUIRE_FALSE(back.is_discarded());
    CHECK(back.get<std::string>() == entry.value().payload);
  }
  CHECK(ledger.verify_chain().has_value());
}

TEST_CASE("IMP-17: a genuine edit is STILL detected, and the triage hint never excuses one",
          "[ledger][utf8][tamper]") {
  const TempDir dir("utf8_tamper");
  const TestClock clock;

  // (1) A well-formed payload edited on disk: BROKEN, and NO hint — nothing is
  //     said that could soften a real edit.
  {
    const fs::path file = dir.path / "plain.jsonl";
    {
      Ledger ledger(clock, file);
      REQUIRE(ledger.append("payload-alpha").has_value());
      REQUIRE(ledger.append("payload-bravo").has_value());
    }
    std::string content = read_file(file);
    const auto pos = content.find("bravo");
    REQUIRE(pos != std::string::npos);
    content.replace(pos, 5, "bravX");
    write_file(file, content);

    Ledger fresh(clock, file);
    REQUIRE(fresh.load().has_value());
    const auto verified = fresh.verify_chain();
    REQUIRE_FALSE(verified.has_value());
    CHECK(verified.error().category == ErrorCategory::Validation);
    CHECK(verified.error().message.find("seq 1") != std::string::npos);
    CHECK(verified.error().message.find("U+FFFD") == std::string::npos);
  }

  // (2) A payload that DOES contain U+FFFD, edited on disk: ALSO BROKEN. The hint
  //     fires (this is the legacy signature: payload hash alone fails, link and seq
  //     intact) but it is WORDING ONLY — same fail, same category, same "seq <n>",
  //     and the text says out loud that it is not an exoneration.
  {
    const fs::path file = dir.path / "replaced.jsonl";
    {
      Ledger ledger(clock, file);
      REQUIRE(ledger.append("payload-alpha").has_value());
      REQUIRE(ledger.append(std::string(kLoneContinuation)).has_value());
      REQUIRE(ledger.append("payload-charlie").has_value());
    }
    std::string content = read_file(file);
    const auto pos = content.find("lone");
    REQUIRE(pos != std::string::npos);
    content.replace(pos, 4, "lonX");
    write_file(file, content);

    Ledger fresh(clock, file);
    REQUIRE(fresh.load().has_value());
    const auto verified = fresh.verify_chain();
    REQUIRE_FALSE(verified.has_value());  // tamper detection is NOT weakened
    CHECK(verified.error().category == ErrorCategory::Validation);
    CHECK(verified.error().message.find("seq 1") != std::string::npos);
    CHECK(verified.error().message.find("U+FFFD") != std::string::npos);
    CHECK(verified.error().message.find("NOT an exoneration") != std::string::npos);

    // IMP-17/C3: THE HINT REPORTS OBSERVED FACTS AND ASSERTS NO PROVENANCE. It
    // used to tell the operator the entry "MAY PREDATE the IMP-17 fix" — an
    // unverifiable claim about a record we cannot date, and one an ATTACKER
    // triggers at will by including the bytes EF BF BD in any payload-only edit
    // (which is exactly what this arm of the test just did). Interpretation
    // belongs to the runbook; the message states only what was measured.
    CHECK(verified.error().message.find("PREDATE") == std::string::npos);
    CHECK(verified.error().message.find("predate") == std::string::npos);
    CHECK(verified.error().message.find("observed:") != std::string::npos);
    CHECK(verified.error().message.find("link and seq intact") != std::string::npos);
  }
}

TEST_CASE("IMP-17: the provenance overload and make_heartbeat canonicalise too",
          "[ledger][utf8][provenance]") {
  const TempDir dir("utf8_provenance");
  const fs::path file = dir.path / "ledger.jsonl";
  const TestClock clock;

  {
    Ledger ledger(clock, file);
    const auto entry = ledger.append(std::string(kTruncatedRun), order_ctx());
    REQUIRE(entry.has_value());
    // Canonical body + the unchanged IMP-16 block; the minted ref still survives.
    CHECK(entry.value().payload ==
          std::string(kTruncatedRunCanonical) +
              " [client_ref=alpha-1a2b3c4d-deadbeef-cafe-4bab-8abe-0123456789ab"
              " broker_order_id=240627000123456 strategy=alpha]");
    CHECK(ledger.verify_chain().has_value());
  }
  Ledger fresh(clock, file);
  REQUIRE(fresh.load().has_value());
  CHECK(fresh.size() == 1);
  CHECK(fresh.verify_chain().has_value());
  CHECK(read_file(file).find(kMintedRef) != std::string::npos);

  // make_heartbeat runs the SAME normalisation in the SAME position, so the struct
  // in memory and the JSON the operator reads can never disagree.
  const auto ts = clock.now_wall();
  const PositionHeartbeat hb = Ledger::make_heartbeat(kTruncatedRun, ts, order_ctx());
  CHECK(hb.exposure.starts_with(kTruncatedRunCanonical));
  CHECK(hb.exposure.find(kMintedRef) != std::string::npos);
  const nlohmann::json parsed = nlohmann::json::parse(hb.to_json(), nullptr, false);
  REQUIRE_FALSE(parsed.is_discarded());
  CHECK(parsed.at("exposure").get<std::string>() == hb.exposure);

  // The 2-argument overload canonicalises identically.
  const PositionHeartbeat plain = Ledger::make_heartbeat(kLoneContinuation, ts);
  CHECK(plain.exposure == kLoneContinuationCanonical);
}

TEST_CASE("IMP-17/C2: PositionHeartbeat::to_json normalises BOTH hand-fillable fields",
          "[ledger][utf8]") {
  // `ts` and `exposure` are the same KIND of field — two std::strings in a plain
  // aggregate a caller or a test can fill by hand — and to_json() is the operator's
  // view of both. Canonicalising one and not the other was an asymmetry with no
  // justification: neither field is hashed or signed, so normalising is free, and
  // "what we render is what we hold" must hold for the WHOLE struct or it is not a
  // rule at all. (Contrast EodReport::head_hash, which is SIGNED and therefore must
  // NOT be transformed on one side — see the B2 test.)
  PositionHeartbeat hb;
  hb.ts = std::string("2026-08-09T00:00:0\x80") + "Z";  // ill-formed, hand-filled
  hb.exposure = std::string(kInvalidLead);

  const std::string rendered = hb.to_json();
  const nlohmann::json parsed = nlohmann::json::parse(rendered, nullptr, false);
  REQUIRE_FALSE(parsed.is_discarded());
  CHECK(parsed.at("ts").get<std::string>() == std::string("2026-08-09T00:00:0") +
                                                  std::string(kReplacementChar) + "Z");
  CHECK(parsed.at("exposure").get<std::string>() == kInvalidLeadCanonical);

  // A heartbeat built the normal way is untouched by either call (both are no-ops
  // on ASCII / already-canonical text), so no existing operator output changes.
  const TestClock clock;
  const PositionHeartbeat normal = Ledger::make_heartbeat("flat", clock.now_wall());
  const nlohmann::json normal_json = nlohmann::json::parse(normal.to_json(), nullptr, false);
  REQUIRE_FALSE(normal_json.is_discarded());
  CHECK(normal_json.at("ts").get<std::string>() == normal.ts);
  CHECK(normal_json.at("exposure").get<std::string>() == "flat");
}

TEST_CASE("IMP-17: scrub still runs FIRST, and normalisation changes nothing it redacts",
          "[ledger][utf8][redaction]") {
  const TempDir dir("utf8_scrub");
  const fs::path file = dir.path / "ledger.jsonl";
  const TestClock clock;
  Ledger ledger(clock, file);

  // An ill-formed byte sitting NEXT TO a secret: the secret is still destroyed
  // (scrub runs on the raw text, exactly where it always ran) and the ill-formed
  // byte is still normalised (canonicalisation runs after, on the scrubbed text).
  const auto entry = ledger.append(std::string("\x80 order token=") + std::string(kToken));
  REQUIRE(entry.has_value());
  CHECK(entry.value().payload == std::string(kReplacementChar) + " order token=" +
                                     std::string(broker_exec::domain::kRedactionMarker));
  CHECK(entry.value().payload.find(kToken) == std::string::npos);
  CHECK(read_file(file).find(kToken) == std::string::npos);
  CHECK(ledger.verify_chain().has_value());

  // THE TOKEN-SHAPED RULES ARE ORDER-INVARIANT, PINNED. Normalisation preserves
  // the ASCII subsequence exactly (domain/utf8.hpp P4) and never deletes a run to
  // nothing (P5), so it can neither JOIN two of scrub()'s ASCII token runs nor
  // SPLIT one; scrub() tokenises identically on either side of it, and `key=value`
  // plus the >=20-char high-entropy rule fire on exactly the same runs. Feeding
  // the ALREADY-CANONICAL text (i.e. the normalise-then-scrub order) therefore
  // gives byte-identical output HERE.
  //
  //   !! THIS IS NOT A GENERAL ORDER-EQUIVALENCE, AND IT MUST NOT BE READ AS ONE.
  //   !! See the counterexample test immediately below, which is the REASON the
  //   !! order is fixed. This case is non-discriminating: it would pass under
  //   !! either order, so on its own it certifies nothing about the ordering.
  Ledger swapped(clock, dir.path / "swapped.jsonl");
  const auto other =
      swapped.append(std::string(kReplacementChar) + " order token=" + std::string(kToken));
  REQUIRE(other.has_value());
  CHECK(other.value().payload == entry.value().payload);
}

TEST_CASE("IMP-17: the scrub/normalise ORDER IS OBSERVABLE — the counterexample that fixes it",
          "[ledger][utf8][redaction]") {
  // ── WHY THIS TEST EXISTS ───────────────────────────────────────────────────
  //
  // append()'s comment used to claim that scrub-then-canonicalise and
  // canonicalise-then-scrub "redact identically". THAT CLAIM IS FALSE, and the
  // green test that accompanied it certified the claim from a single
  // non-discriminating example. Left standing, it would have licensed a future
  // refactor to hoist the canonicalisation above the scrub — silently changing
  // what gets redacted from an audit record.
  //
  // WHY IT IS FALSE: canonical_text is NOT length-preserving. One ill-formed byte
  // becomes THREE (U+FFFD), and domain::auth_context_before() looks back a FIXED
  // 10-BYTE window for an auth keyword — so normalising first MOVES the keyword
  // relative to that byte window and flips the bare MPIN/TOTP digit-run rule.
  //
  // The library ships scrub-FIRST (scrub sees the RAW bytes, exactly where it has
  // always run) and canonicalise-LAST (so the preimage==stored invariant holds for
  // the FINAL string). These two cases pin that decision as a DECISION.
  const TempDir dir("utf8_order");
  const TestClock clock;

  // ── (1) THE SHIPPED ORDER REDACTS; THE OTHER ORDER WOULD LEAK ──────────────
  //
  // "mpin " (5) + two bad bytes (2) + " " (1) puts the '1' at byte offset 8, so
  // the 10-byte lookbehind is s[0..7] == "mpin \x80\x80 " and still reaches
  // "mpin" — the PIN is destroyed. Normalise first and the two U+FFFDs occupy SIX
  // bytes: the '1' moves to offset 12, the window becomes s[2..11], and "mpin"
  // has fallen out of it entirely.
  constexpr std::string_view kMpinRaw = "mpin \x80\x80 1234";
  constexpr std::string_view kMpinCanonical = "mpin \xEF\xBF\xBD\xEF\xBF\xBD 1234";

  Ledger shipped(clock, dir.path / "shipped.jsonl");
  const auto shipped_entry = shipped.append(std::string(kMpinRaw));
  REQUIRE(shipped_entry.has_value());
  CHECK(shipped_entry.value().payload.find(broker_exec::domain::kRedactionMarker) !=
        std::string::npos);
  CHECK(shipped_entry.value().payload.find("1234") == std::string::npos);  // the PIN is GONE

  // Appending the ALREADY-CANONICAL text is exactly the canonicalise-then-scrub
  // order (canonical_text is idempotent, so the second pass is a no-op).
  Ledger hoisted(clock, dir.path / "hoisted.jsonl");
  const auto hoisted_entry = hoisted.append(std::string(kMpinCanonical));
  REQUIRE(hoisted_entry.has_value());
  CHECK(hoisted_entry.value().payload.find(broker_exec::domain::kRedactionMarker) ==
        std::string::npos);
  CHECK(hoisted_entry.value().payload.find("1234") != std::string::npos);  // ...it LEAKS

  // The two orders therefore produce DIFFERENT ledger payloads. This single
  // inequality is the whole point of the test.
  CHECK(shipped_entry.value().payload != hoisted_entry.value().payload);

  // ── (2) AND THE CONVERSE, so nobody "fixes" this by hoisting the call ──────
  //
  // Do NOT read (1) as "scrub-first always redacts more". Here "totp" is embedded
  // inside a longer word, so auth_context_before's whole-word check rejects it on
  // the RAW bytes and the 8-digit run SURVIVES the shipped order; after
  // normalisation the expansion separates the keyword and it matches. The order is
  // simply OBSERVABLE in both directions, which is precisely why it must be FIXED
  // and stated rather than assumed away.
  constexpr std::string_view kEmbeddedRaw = "passwordtokentotp\xC0\x80" "12345678";
  constexpr std::string_view kEmbeddedCanonical =
      "passwordtokentotp\xEF\xBF\xBD\xEF\xBF\xBD" "12345678";

  Ledger shipped2(clock, dir.path / "shipped2.jsonl");
  const auto shipped2_entry = shipped2.append(std::string(kEmbeddedRaw));
  REQUIRE(shipped2_entry.has_value());
  CHECK(shipped2_entry.value().payload.find("12345678") != std::string::npos);

  Ledger hoisted2(clock, dir.path / "hoisted2.jsonl");
  const auto hoisted2_entry = hoisted2.append(std::string(kEmbeddedCanonical));
  REQUIRE(hoisted2_entry.has_value());
  CHECK(hoisted2_entry.value().payload.find("12345678") == std::string::npos);
  CHECK(hoisted2_entry.value().payload.find(broker_exec::domain::kRedactionMarker) !=
        std::string::npos);
  CHECK(shipped2_entry.value().payload != hoisted2_entry.value().payload);

  // Both chains still verify — the ordering question is about REDACTION, never
  // about integrity, and the IMP-17 invariant holds under either order.
  CHECK(shipped.verify_chain().has_value());
  CHECK(hoisted.verify_chain().has_value());
  CHECK(shipped2.verify_chain().has_value());
  CHECK(hoisted2.verify_chain().has_value());
}

TEST_CASE("IMP-17/C1: a RAW ill-formed byte in a stored LINE is REJECTED by load()",
          "[ledger][utf8][load]") {
  // THE GAP THIS CLOSES. The canonical_text choke point covers text this process
  // AUTHORS. Text PARSED BACK out of the file relies on a DIFFERENT and
  // THIRD-PARTY invariant: nlohmann's parser rejects a JSON string containing a
  // raw ill-formed UTF-8 byte. Nothing in this repo enforces that, so a dependency
  // bump could silently relax it and let a hand-planted raw byte into an in-memory
  // payload — where it would be hashed raw, re-stored replaced, and reintroduce
  // the exact IMP-17 divergence from the read side. Pin it.
  const TempDir dir("utf8_load_reject");
  const fs::path file = dir.path / "ledger.jsonl";
  const TestClock clock;
  {
    Ledger ledger(clock, file);
    REQUIRE(ledger.append("payload-alpha").has_value());
    REQUIRE(ledger.append("payload-bravo").has_value());
  }

  // Plant a raw 0x80 INSIDE the first line's payload string. It is NOT the last
  // content line, so the torn-write leniency does not apply and this must be FATAL.
  std::string content = read_file(file);
  const auto pos = content.find("alpha");
  REQUIRE(pos != std::string::npos);
  content.replace(pos, 5, "alph\x80");
  write_file(file, content);

  Ledger fresh(clock, file);
  const auto loaded = fresh.load();
  REQUIRE_FALSE(loaded.has_value());  // NOT ok(), and NOT a silently-skipped line
  CHECK(loaded.error().category == ErrorCategory::Validation);
  CHECK(loaded.error().message.find("malformed entry on line 1") != std::string::npos);

  // Belt and braces: assert the third-party behaviour we are depending on,
  // directly, so a dependency bump that changed it fails HERE with an obvious
  // message rather than in the ledger's error text.
  const nlohmann::json direct = nlohmann::json::parse(R"({"payload":"alph)" "\x80" R"("})",
                                                      nullptr, false);
  CHECK(direct.is_discarded());
}

TEST_CASE("IMP-17/B2: the checkpoint's head_hash IS the bytes sign_head signed",
          "[ledger][utf8][checkpoint]") {
  // ── THE ASYMMETRY THIS PINS SHUT ──────────────────────────────────────────
  //
  // sign_head() signs the RAW head_hash() string, eod_report() stores that same
  // raw value, and verify_against_checkpoint() compares the PERSISTED head against
  // a RAW in-memory entry hash. EodReport::to_json() briefly ran head_hash through
  // canonical_text on the way out — a transform on ONE side of a signature, in the
  // one place that is the anti-tamper ANCHOR. That is the IMP-17 defect itself
  // (hash/sign one string, store another), even though it was unreachable while
  // head_hash is ASCII hex. The transform is gone; this test is what keeps it gone.
  const TempDir dir("checkpoint_head_identity");
  const fs::path file = dir.path / "ledger.jsonl";
  const TestClock clock;
  Ledger ledger(clock, file);

  const auto kp = Ledger::generate_keypair();
  REQUIRE(kp.has_value());
  append_all(ledger, {"p0", "p1", "p2"});

  // ONE value, produced once, flowing into the signature, the struct and the JSON
  // with NO transform anywhere along the way.
  const std::string head = ledger.head_hash();
  REQUIRE(head.size() == 64);
  const auto report = ledger.eod_report(kp.value().private_key, kp.value().public_key);
  REQUIRE(report.has_value());
  CHECK(report.value().head_hash == head);

  const nlohmann::json bundle = nlohmann::json::parse(report.value().to_json(), nullptr, false);
  REQUIRE_FALSE(bundle.is_discarded());
  // THE ASSERTION: the persisted bytes are byte-for-byte the signed bytes.
  CHECK(bundle.at("head_hash").get<std::string>() == head);
  // ...and that is exactly what the signature was taken over.
  CHECK(Ledger::verify_head(bundle.at("head_hash").get<std::string>(), report.value().signature,
                            kp.value().public_key)
            .has_value());

  // The same identity through the file the checkpoint actually publishes, and the
  // full round-trip still verifies.
  REQUIRE(ledger.write_checkpoint(kp.value().private_key, kp.value().public_key).has_value());
  const nlohmann::json on_disk =
      nlohmann::json::parse(read_file(file.string() + ".checkpoint"), nullptr, false);
  REQUIRE_FALSE(on_disk.is_discarded());
  CHECK(on_disk.at("head_hash").get<std::string>() == head);
  CHECK(ledger.verify_against_checkpoint(kp.value().public_key).has_value());
}
