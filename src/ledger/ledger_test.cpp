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
