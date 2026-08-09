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
// TYPED PROVENANCE (IMP-16): because that scrub also destroyed a client_ref (one
// long token-shaped run), the ids no longer travel inside the free-form payload —
// they are passed as a ProvenanceContext and rendered through the whole-column
// allowlist, so the chain can be joined to the log without weakening the payload's
// redaction by one byte. See ProvenanceContext and append(payload, provenance).
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

// TYPED PROVENANCE for a ledger record (IMP-16, FR-27/FR-29).
//
// THE DEFECT THIS EXISTS FOR: append() scrubs its FREE-FORM payload, and a minted
// client_ref is one long token-shaped run — so a payload that interpolated
// `client_ref=<ref>` was persisted (and HASHED) as `client_ref=***REDACTED***`.
// The tamper-evident ledger could not be joined back to the log, the store or the
// intent log, which is most of the reason a ledger entry exists. The IMP-15
// exemption is a WHOLE-TYPED-COLUMN allowlist and must never reach into a
// free-form body, so the ids travel BESIDE the payload instead, in typed columns.
// PUT IDS HERE, NEVER IN THE PAYLOAD STRING.
//
// Deliberately a LEDGER type rather than a reuse of ports::AlertContext: the
// ledger must not take an alerting dependency to record who an entry belongs to,
// and the two surfaces are free to grow different columns. Only the RENDERING is
// shared (domain::render_provenance_block), so the two can never drift in how a
// column is redacted.
struct ProvenanceContext {
  std::string client_ref;       // our idempotency key (the store/intent-log join key)
  std::string broker_order_id;  // the broker-minted order id
  std::string strategy;         // the owning strategy name
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

  // Same as append(payload), plus TYPED PROVENANCE (IMP-16). `payload` is scrubbed
  // by the IDENTICAL domain::scrub call — free-form redaction is untouched — and
  // `provenance` is rendered SEPARATELY through the whole-column allowlist and
  // APPENDED to the stored payload as ` [client_ref=... broker_order_id=...
  // strategy=...]`, omitting empty fields. A column that is not id-shaped is
  // redacted.
  //
  // HASH IMPACT — none, for anything already written. The stored payload IS the
  // hash preimage (hash = sha256(prev_hash + stored_payload)) and verify_chain()
  // recomputes from the STORED payload, so a chain written before this overload
  // existed reloads and verifies bit-for-bit unchanged. Only NEW entries created
  // through THIS overload with a NON-EMPTY context differ, and they differ
  // consistently in both the stored bytes and the preimage. An EMPTY context
  // renders "" and is therefore hash-identical to the 1-argument append — which is
  // exactly how the 1-argument overload is implemented.
  [[nodiscard]] Result<LedgerEntry> append(std::string payload,
                                           const ProvenanceContext& provenance);

  // Walk head->tail: recompute each hash from prev_hash+payload, confirm each
  // entry links to the prior entry's hash, and confirm seq is contiguous from 0.
  // On the FIRST violation, fail (Validation) naming the bad seq. ok() if intact.
  [[nodiscard]] Result<ports::Ok> verify_chain() const;

  // Clear in-memory state and rebuild it from the file (one JSON line per
  // entry). A blank line is skipped; a parse failure is an Error. load() REBUILDS
  // ONLY — it does not trust and never auto-validates. The safe-start / recovery
  // sequence is the caller's: load() -> verify_chain() (internal consistency) ->
  // verify_against_checkpoint() (truncation/rollback vs the retained signed
  // high-water mark). load() deliberately does NOT call either, per this contract.
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

  // Same, plus TYPED PROVENANCE (IMP-16), so a per-strategy/per-order heartbeat
  // can NAME what it is reporting on instead of a caller re-interpolating an id
  // into the free-form summary (which the scrub would destroy — the exact defect
  // IMP-16 fixes). The summary is scrubbed identically and the rendered block is
  // appended to `exposure`; an all-empty context leaves `exposure` — and therefore
  // to_json() — byte-identical to the 2-argument form.
  [[nodiscard]] static PositionHeartbeat make_heartbeat(std::string_view exposure_summary,
                                                        std::chrono::system_clock::time_point ts,
                                                        const ProvenanceContext& provenance);

  // ── Tamper-evident truncation / rollback detection ───────────────────────
  //
  // verify_chain() only proves the entries PRESENT are internally consistent;
  // it cannot see entries that were removed. An attacker who CHOPS THE TAIL
  // (drops the last N entries) or ROLLS BACK to an earlier state leaves a chain
  // that is still self-consistent, so verify_chain() passes and the loss goes
  // undetected. The retained signed checkpoint closes that gap: it persists a
  // signed high-water mark {size, head_hash, signature, public_key} to a SEPARATE
  // sibling file, and a later load can prove the live chain still REACHES that
  // signed point.
  //
  // TRUST MODEL (read this): detection holds ONLY against a PINNED public key
  // supplied out-of-band to verify_against_checkpoint() — the checkpoint file is
  // attacker-writable, so its embedded public_key cannot anchor trust by itself
  // (a non-key-holder could re-sign a truncated chain under a fresh key). The
  // pinned key is the EOD-published key / ledger_public_key.hex provisioned under
  // restricted perms. With that anchor, a non-key-holder rewrite is DETECTED; the
  // residual honesty caveat (SEC-2) is only that the genuine PRIVATE-key holder
  // can re-sign — so this is tamper-EVIDENT against everyone else, not legal proof.

  // Sign the current head and persist the high-water-mark bundle {size,
  // head_hash, Ed25519 signature over head_hash, public_key} as compact JSON to
  // the sibling path checkpoint_path() (= <ledger path> + ".checkpoint"). Written
  // ATOMICALLY (temp file + rename) and fsync'd via platform::durable_sync, the
  // same durability discipline as every other persisted record. An empty ledger
  // has no head -> a defined Error ("ledger empty, nothing to checkpoint").
  [[nodiscard]] Result<ports::Ok> write_checkpoint(
      const std::vector<unsigned char>& private_key,
      const std::vector<unsigned char>& public_key) const;

  // Verify the live chain still reaches the retained signed checkpoint, anchored
  // to `pinned_public_key` (supplied out-of-band — the EOD-published key /
  // ledger_public_key.hex; NEVER the key embedded in the checkpoint, which is
  // attacker-writable). The embedded key must MATCH the pinned key (else Error:
  // substitution) before the signature is trusted. Call this AFTER
  // load()+verify_chain() in the safe-start / recovery sequence (load() stays
  // rebuild-only by contract and never auto-calls this). Outcomes:
  //   • embedded key != pinned key -> Error (FAIL CLOSED): key substitution.
  //   • no checkpoint file        -> ok(). No prior signed state exists, so a
  //                                  truncation simply has no baseline to be
  //                                  measured against yet — this is NOT fail-open,
  //                                  there is genuinely nothing to compare. The
  //                                  very first run, before any checkpoint, cannot
  //                                  detect truncation; write_checkpoint() arms it.
  //   • checkpoint present but unparseable -> Error (FAIL CLOSED): a checkpoint
  //                                  that exists but is garbage is itself tamper.
  //   • signature does not verify  -> Error "checkpoint signature invalid"
  //                                  (forged / edited checkpoint).
  //   • size() <  checkpoint.size  -> Error: ledger TRUNCATED below the signed
  //                                  high-water mark (the tail was chopped).
  //   • entry[checkpoint.size-1].hash != checkpoint.head_hash -> Error: ledger
  //                                  ROLLED BACK / SUBSTITUTED (a divergent chain).
  //   • otherwise                  -> ok(). Growth is fine: appending MORE entries
  //                                  after a checkpoint still verifies, because the
  //                                  historical entry at checkpoint.size-1 is
  //                                  unchanged and size only grew.
  [[nodiscard]] Result<ports::Ok> verify_against_checkpoint(
      const std::vector<unsigned char>& pinned_public_key) const;

 private:
  // The retained-checkpoint sibling path: the ledger path with a ".checkpoint"
  // suffix appended to its filename (e.g. <dir>/ledger.jsonl.checkpoint).
  [[nodiscard]] std::filesystem::path checkpoint_path() const;

  const ports::ClockPort* clock_;
  std::filesystem::path path_;
  std::vector<LedgerEntry> entries_;
};

}  // namespace broker_exec::ledger
