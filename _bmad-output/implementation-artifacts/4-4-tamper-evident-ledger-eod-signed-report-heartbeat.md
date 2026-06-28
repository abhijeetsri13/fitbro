# Story 4.4: Tamper-evident ledger, EOD signed report & heartbeat

Status: ready-for-dev

## Story

As an operator,
I want a hash-chained signed ledger and a position heartbeat,
so that I have a system-of-record foundation and a "still safe" signal. (FR-29)

## Acceptance Criteria

1. **Given** the ledger (SHA-256 chain + Ed25519 per-account signature) **When** an entry is modified by a non-key-holder
   or corrupted **Then** the chain breaks and is detectable; the public key is written to the data dir + EOD report.
2. **And** a periodic position/exposure heartbeat reaches the operator.
3. **And** key-mismatch is a fail-closed safe-start condition; the ledger is never presented as legal proof.

## Tasks / Subtasks

- [ ] Task 1: `ledger` module (AC: all)
  - [ ] `include/broker_exec/ledger/` + `src/ledger/`; target `broker_exec_ledger` (+ alias). Reuses the ALREADY-BUILT
        `OpenSSL::Crypto` (Story 2.2) for BOTH the SHA-256 chain AND Ed25519 — NO libsodium, NO new Conan dep. Depends inward on
        `domain` (scrub), `ports` (ClockPort), `platform` (durable_sync), `errors`.
- [ ] Task 2: Hash-chained ledger (AC: 1)
  - [ ] `struct LedgerEntry { std::int64_t seq; std::string prev_hash; std::string payload; std::string hash; };`
        where `hash = SHA256_hex(prev_hash + payload)` (genesis prev_hash = "").
  - [ ] `class Ledger` (ctor `(const ports::ClockPort& clock, std::filesystem::path path)`):
    - `Result<LedgerEntry> append(std::string payload)`: SCRUB the payload via `domain::scrub` FIRST (the 4.2 lesson — the
      persisted payload must carry no token); seq = last+1; prev_hash = last entry's hash (or ""); hash = SHA256_hex(prev_hash +
      scrubbed_payload); append the entry to memory AND to the on-disk file (one JSON line), then `platform::durable_sync` the fd.
    - `Result<ports::Ok> verify_chain() const`: walk entries head->tail; recompute each `hash` from its prev_hash+payload and
      check it equals the stored hash AND that each entry's prev_hash == the previous entry's hash AND seq is contiguous; on the
      FIRST mismatch return an Error NAMING the bad seq (tamper/corruption detected). A non-key-holder edit to any payload/hash
      breaks the chain (AC-1).
    - `Result<ports::Ok> load()`: read the file, rebuild the in-memory entries; (the caller verify_chain()s after load).
- [ ] Task 3: Ed25519 per-account signature (AC: 1, 3) — OpenSSL EVP
  - [ ] `struct Ed25519KeyPair { std::vector<unsigned char> public_key; std::vector<unsigned char> private_key; };` (raw 32-byte each).
  - [ ] `[[nodiscard]] static Result<Ed25519KeyPair> generate_keypair()` (EVP_PKEY_keygen with EVP_PKEY_ED25519; export raw via
        EVP_PKEY_get_raw_public_key / get_raw_private_key). RAII on the EVP contexts (mirror src/secrets/token_store.cpp).
  - [ ] `[[nodiscard]] Result<std::vector<unsigned char>> sign_head(const std::vector<unsigned char>& private_key) const`:
        Ed25519-sign the chain HEAD hash (the last entry's hash) — EVP_DigestSign one-shot. Empty ledger -> a defined Error.
  - [ ] `[[nodiscard]] static Result<ports::Ok> verify_head(std::string_view head_hash, const std::vector<unsigned char>& signature,
        const std::vector<unsigned char>& public_key)`: EVP_DigestVerify; a wrong key or a tampered head -> fail-closed Error.
- [ ] Task 4: Public-key persistence + key-mismatch fail-closed (AC: 1, 3)
  - [ ] `Result<ports::Ok> write_public_key(const std::vector<unsigned char>& public_key, const std::filesystem::path& dir) const`
        (write the hex/raw public key to a file in the data dir — also returned in the EOD report).
  - [ ] `[[nodiscard]] static Result<ports::Ok> require_key_match(const std::vector<unsigned char>& expected_public_key,
        const std::vector<unsigned char>& actual_public_key)`: ok() iff equal; else a fail-closed Error (a key-mismatch is a
        safe-start blocker — the account's signing key changed/was tampered). (AC-3)
- [ ] Task 5: EOD signed report + position heartbeat (AC: 1, 2)
  - [ ] `struct EodReport { std::int64_t entry_count; std::string head_hash; std::vector<unsigned char> signature; std::vector<unsigned char> public_key; std::string to_json() const; };`
        `[[nodiscard]] Result<EodReport> eod_report(const std::vector<unsigned char>& private_key, const std::vector<unsigned char>& public_key) const`
        — sign the head, bundle count+head+signature(hex)+public_key(hex). The report carries the public key (AC-1). Document: the
        ledger is tamper-EVIDENT, NOT legal proof (a key-holder can re-sign a forged chain — wording per architecture SEC-2).
  - [ ] `struct PositionHeartbeat { std::string ts; std::string exposure_json; std::string to_json() const; };` +
        `[[nodiscard]] static PositionHeartbeat make_heartbeat(std::string_view exposure_summary, std::chrono::system_clock::time_point ts)`
        — the periodic position/exposure signal the alerter (Story 4.3) delivers to the operator (AC-2); scrub the exposure
        summary (no secret). (The actual send is the AlertSink; here produce the heartbeat content.)
- [ ] Task 6: CMake (orchestrator pre-wires root add_subdirectory(src/ledger); OpenSSL already find_package'd — NO new dep)
  - [ ] `src/ledger/CMakeLists.txt`: links PUBLIC `broker_exec::errors`; PRIVATE `OpenSSL::Crypto` `broker_exec::domain`
        `broker_exec::ports` `broker_exec::platform` `nlohmann_json` warnings+sanitizers. Test exe `broker_exec_ledger_tests`
        ALSO links `broker_exec::clock` + OpenSSL + domain.
- [ ] Task 7: Tests (AC: 1, 2, 3) — `src/ledger/ledger_test.cpp` (temp dir; TestClock)
  - [ ] append a few entries -> verify_chain() ok; the chain links (each prev_hash == prior hash).
  - [ ] TAMPER (AC-1): mutate a stored entry's payload (or hash) -> verify_chain() returns an Error NAMING the first bad seq.
  - [ ] persist + reload: a fresh Ledger.load() over the same file -> verify_chain() ok (survives restart); a byte-edit to the
        FILE then load+verify -> detected.
  - [ ] Ed25519 (AC-1): generate_keypair(); append entries; sign_head(priv) -> verify_head(head, sig, pub) ok; verify with a
        DIFFERENT keypair's public key -> fail-closed Error; a tampered head -> verify fails.
  - [ ] key-mismatch (AC-3): require_key_match(expected, expected) ok; require_key_match(expected, other) -> fail-closed Error.
  - [ ] EOD report: eod_report() carries entry_count, head_hash, a signature that verify_head() accepts, and the public key;
        write_public_key writes a file in the temp dir.
  - [ ] heartbeat (AC-2): make_heartbeat("net=+50 ...", ts) -> to_json carries ts + exposure; a token in the exposure summary is scrubbed.
  - [ ] redaction (4.2 lesson): a token in an appended payload is scrubbed BEFORE hashing/persist (the stored payload + hash carry no token).

## Dev Notes

- **OpenSSL for both** (no libsodium): SHA-256 via EVP/`SHA256`, Ed25519 via `EVP_PKEY_ED25519` + `EVP_DigestSign/Verify`. RAII on
  every EVP ctx (mirror `src/secrets/token_store.cpp`). [architecture.md#SE-3, #Tech revision OpenSSL+libsodium -> OpenSSL only here]
- **Tamper-EVIDENT, not proof** (AC-3) — the chain detects a NON-key-holder edit/corruption; a key-holder can re-sign a forged
  chain, so it is NOT legal proof (document it). Key-mismatch is a fail-closed safe-start condition. [architecture.md#SEC-2, SE-5]
- **Scrub before persist** (the 4.2 lesson) — append() scrubs the payload before hashing/writing; no token in the ledger. [Story 4.2, SEC-3]
- **Durable** — append fsyncs via `platform::durable_sync`. [docs/conventions.md, NFR-1]
- **Reuse:** OpenSSL (2.2), `domain::scrub`, `ports::ClockPort`, `platform::durable_sync`/`portable_fileno`, `errors`, nlohmann, `clock::TestClock` (tests).

### References
- [Source: epics.md#Story 4.4] [architecture.md#SE-3 ledger crypto, #SEC-2 tamper-evidence honesty, #SE-5 key lifecycle, #FR-29] [Source: src/secrets/token_store.cpp (OpenSSL EVP RAII), include/broker_exec/platform/durable.hpp]
- [Source: docs/conventions.md] [Source: include/broker_exec/domain/redaction.hpp]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
