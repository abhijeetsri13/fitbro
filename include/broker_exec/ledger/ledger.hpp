#pragma once

// broker_exec::ledger — tamper-EVIDENT hash-chained ledger + Ed25519-signed EOD
// report + position heartbeat (Story 4.4, FR-29, architecture SE-3/SE-5/SEC-2).
//
// The ledger is an append-only SHA-256 hash chain: each entry's hash is
// SHA256_hex(prev_hash + scrubbed_payload), the genesis prev_hash is "". A
// non-key-holder edit/corruption of any payload, hash, or link is DETECTED by
// verify_chain(), which names the first bad seq. Entries are persisted one JSON
// line at a time and fsync'd through the platform durability seam.
//
// HONESTY (SEC-2): this is tamper-EVIDENT, NOT legal proof. The chain detects a
// non-key-holder edit; a holder of the Ed25519 private key can re-sign a forged
// chain, so it must never be presented as legal/cryptographic non-repudiation.
// A signing-key mismatch is a fail-closed safe-start blocker (AC-3).
//
// SCRUB BEFORE PERSIST (the 4.2 lesson, SEC-3): append() and make_heartbeat()
// run their text through domain::scrub() BEFORE hashing/persisting, so no
// token-shaped secret ever lands in the chain, the file, or the hash preimage.
//
// Crypto is OpenSSL only (Story 2.2 dependency): SHA-256 and Ed25519 both via
// the EVP interface, with RAII on every EVP context/PKEY so nothing leaks across
// the no-throw boundary. No libsodium, no new Conan dependency.
//
// Cross-platform: C++20 standard library only. OS divergence (fsync) lives
// behind broker_exec::platform; NO `#ifdef`/OS APIs here. No throw across the
// boundary (Result<T>), no float.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "broker_exec/ports/clock_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::ledger {

// One link in the hash chain. `hash == sha256_hex(prev_hash + payload)`; the
// genesis entry has `prev_hash == ""`. `payload` is always the SCRUBBED payload
// (the hash is computed over the scrubbed text, so the chain and the file agree).
struct LedgerEntry {
  std::int64_t seq = 0;
  std::string prev_hash;
  std::string payload;
  std::string hash;
};

// A raw Ed25519 key pair: 32-byte public key, 32-byte private (seed) key.
struct Ed25519KeyPair {
  std::vector<unsigned char> public_key;
  std::vector<unsigned char> private_key;
};

// The end-of-day signed report. Carries the entry count, the chain head hash,
// the Ed25519 signature over that head, and the public key (so a verifier needs
// nothing else). NOT legal proof — see the SEC-2 note at the top of this file.
struct EodReport {
  std::int64_t entry_count = 0;
  std::string head_hash;
  std::vector<unsigned char> signature;
  std::vector<unsigned char> public_key;

  // Compact, non-throwing JSON: {entry_count, head_hash, signature(hex),
  // public_key(hex)}. Binary fields are rendered as lowercase hex.
  [[nodiscard]] std::string to_json() const;
};

// A periodic position/exposure "still safe" heartbeat (AC-2). The exposure
// summary is scrubbed at construction so no secret reaches the operator sink.
struct PositionHeartbeat {
  std::string ts;        // ISO-8601 UTC wall time
  std::string exposure;  // scrubbed exposure/position summary

  // Compact, non-throwing JSON: {ts, exposure}.
  [[nodiscard]] std::string to_json() const;
};

class Ledger {
 public:
  // `clock` is the injected wall-clock source (FR-23); `path` is the on-disk
  // ledger file. The Ledger borrows the clock — it must outlive the Ledger.
  Ledger(const ports::ClockPort& clock, std::filesystem::path path) noexcept;

  // Scrub `payload`, link it onto the chain (seq, prev_hash, hash), append one
  // JSON line to the file, and fsync via platform::durable_sync. Returns the new
  // entry (carrying the SCRUBBED payload). The hash is over the scrubbed payload.
  [[nodiscard]] Result<LedgerEntry> append(std::string payload);

  // Walk head->tail: recompute each hash from prev_hash+payload, confirm each
  // entry links to the prior entry's hash, and confirm seq is contiguous from 0.
  // On the FIRST violation, fail (Validation) naming the bad seq. ok() if intact.
  [[nodiscard]] Result<ports::Ok> verify_chain() const;

  // Clear in-memory state and rebuild it from the file (one JSON line per
  // entry). A blank line is skipped; a parse failure is an Error. The caller is
  // expected to verify_chain() after a load (load rebuilds, it does not trust).
  [[nodiscard]] Result<ports::Ok> load();

  // The chain head hash (last entry's hash), or "" when the ledger is empty.
  [[nodiscard]] std::string head_hash() const;

  // Number of entries currently in memory.
  [[nodiscard]] std::size_t size() const noexcept;

  // ── Ed25519 (OpenSSL EVP, all RAII) ──────────────────────────────────────

  // Generate a fresh Ed25519 key pair (raw 32-byte public + private).
  [[nodiscard]] static Result<Ed25519KeyPair> generate_keypair();

  // Ed25519-sign the chain head hash with `private_key` (raw 32 bytes). An empty
  // ledger has no head -> a defined Error ("ledger empty, nothing to sign").
  [[nodiscard]] Result<std::vector<unsigned char>> sign_head(
      const std::vector<unsigned char>& private_key) const;

  // Verify an Ed25519 `signature` over `head_hash` under `public_key`. Returns
  // ok() iff OpenSSL verifies; a wrong key or a tampered head fails CLOSED.
  [[nodiscard]] static Result<ports::Ok> verify_head(
      std::string_view head_hash, const std::vector<unsigned char>& signature,
      const std::vector<unsigned char>& public_key);

  // Fail-closed key-identity guard (AC-3): ok() iff `expected == actual`, else a
  // safe-start Error. These are PUBLIC keys (not secrets), so a plain compare.
  [[nodiscard]] static Result<ports::Ok> require_key_match(
      const std::vector<unsigned char>& expected, const std::vector<unsigned char>& actual);

  // Persist the public key (lowercase hex) to `<dir>/ledger_public_key.hex` so a
  // verifier can recover it from the data dir as well as from the EOD report.
  [[nodiscard]] Result<ports::Ok> write_public_key(const std::vector<unsigned char>& public_key,
                                                   const std::filesystem::path& dir) const;

  // ── EOD report + heartbeat ───────────────────────────────────────────────

  // Sign the head and bundle {size, head_hash, signature, public_key}. NOT legal
  // proof (SEC-2).
  [[nodiscard]] Result<EodReport> eod_report(const std::vector<unsigned char>& private_key,
                                             const std::vector<unsigned char>& public_key) const;

  // Build a heartbeat: scrub the exposure summary, stamp `ts` as ISO-8601 UTC.
  [[nodiscard]] static PositionHeartbeat make_heartbeat(std::string_view exposure_summary,
                                                        std::chrono::system_clock::time_point ts);

 private:
  const ports::ClockPort* clock_;
  std::filesystem::path path_;
  std::vector<LedgerEntry> entries_;
};

}  // namespace broker_exec::ledger
