# Story 2.2: Secret provider, token encryption, and the redaction scrubber

Status: ready-for-dev

## Story

As an operator,
I want secrets sourced safely, encrypted at rest, and scrubbed from all output,
so that no credential ever leaks to a log, error, or the ledger. (FR-35)

## Acceptance Criteria

1. **Given** a `SecretProvider` (env default) and an OpenSSL AES-256-GCM token store **When** tokens are persisted
   **Then** they are encrypted at rest with a per-account key; the store/key files are mode 0600 in a 0700 dir.
2. **And** a shared secret-shape scrubber (`domain/redaction`) redacts token-shaped strings from log output,
   exception text, AND the intent-log/ledger persistence path.
3. **And** a test feeding a synthetic Kite access_token through a log line, an exception message, and a persisted
   record finds zero token-shaped strings in any sink.

## Tasks / Subtasks

- [ ] Task 1: `domain/redaction` — pure secret-shape scrubber (AC: 2, 3)
  - [ ] `include/broker_exec/domain/redaction.hpp` + `src/domain/redaction.cpp`, added to the existing `broker_exec_domain`
        target (domain stays PURE — no crypto/OpenSSL here; pure string logic only).
  - [ ] `std::string scrub(std::string_view)` replacing token-shaped substrings with a fixed marker (e.g. `***REDACTED***`).
        Shapes: long base64/hex token runs (Kite access/enc/request/public tokens ~20–40 chars), `*_token`/`api_key`/
        `secret`-named `key=value`/`key: value`/JSON `"key":"value"` pairs, MPIN/TOTP digit runs (4–8 digits in an auth
        context), and token-named URL query params (`?...token=XXXX`). Conservative: never redact ordinary prose/ids.
  - [ ] Idempotent (scrub(scrub(x)) == scrub(x)); no throw; cross-platform std-lib only.
- [ ] Task 2: Platform file-permission seam (AC: 1)
  - [ ] Add to `src/platform/`: `restrict_to_owner_file(path)` (0600) and `restrict_to_owner_dir(path)` (0700) —
        POSIX `chmod`, Windows ACL/`_S_IREAD|_S_IWRITE` best-effort — behind the existing platform seam (the ONLY place
        OS `#ifdef` is allowed). Header `include/broker_exec/platform/permissions.hpp`. Return bool/Result.
- [ ] Task 3: `secrets` module — env SecretProvider + encrypted token store (AC: 1)
  - [ ] `src/secrets/` + `include/broker_exec/secrets/`. New CMake target `broker_exec_secrets`.
  - [ ] `EnvSecretProvider` implements `ports::SecretProvider::get()` via an injected env seam (reuse the
        `config` env-seam idea; do NOT call getenv directly outside a default seam). Missing key -> typed Error
        (never a token-shaped message).
  - [ ] `TokenStore`: AES-256-GCM (OpenSSL EVP) encrypt-at-rest. Per-account 256-bit key obtained from a
        `SecretProvider` (key is non-co-located with ciphertext — SEC-1). Random 96-bit IV per record; store
        IV‖ciphertext‖tag. `save(account, name, plaintext)->Result<>` writes the file then sets 0600 in a 0700 dir
        via the platform seam; `load(...)->Result<std::string>` decrypts and FAILS CLOSED on a bad GCM tag (tamper).
  - [ ] All fallible calls return `Result<T>` (`expected<T,Error>`); never throw across the boundary; RAII for all
        OpenSSL contexts (EVP_CIPHER_CTX_free).
- [ ] Task 4: CMake + Conan wiring (AC: all)
  - [ ] Add `openssl/3.x` to `conanfile.py`; `find_package(OpenSSL REQUIRED)` in root CMake; `add_subdirectory(src/secrets)`.
  - [ ] `broker_exec_secrets` links `OpenSSL::Crypto`, `broker_exec::ports`, `broker_exec::errors`, `broker_exec::platform`,
        `broker_exec::domain` (for redaction in error paths), warnings+sanitizers PRIVATE.
- [ ] Task 5: Tests (AC: 1, 2, 3)
  - [ ] `src/domain/redaction_test.cpp` (or extend a domain test): a synthetic Kite `access_token` embedded in
        (a) a log-shaped line, (b) an exception `what()` string, (c) a persisted JSON record string -> after `scrub`,
        zero occurrences of the token AND zero token-shaped runs remain. Idempotence. Non-secret text untouched.
  - [ ] `src/secrets/secrets_test.cpp`: env provider get/missing; token store encrypt->decrypt round-trip; a flipped
        ciphertext/tag byte -> decrypt fails closed; (where checkable) file perms restricted. Deterministic key via a
        fake SecretProvider; temp dir via std::filesystem.

## Dev Notes

- **Existing port:** implement `include/broker_exec/ports/secret_provider.hpp` (`Result<std::string> get(key)`).
- **Redaction placement:** `domain/redaction` — domain stays pure (NO OpenSSL link on domain). [architecture.md#SEC-3/SEC-6, New modules]
- **Crypto:** OpenSSL AES-256-GCM (EVP) — SE-2; per-account key, IV per record, auth tag verified (fail-closed on tamper).
  [architecture.md#SE-2, SE-5 key lifecycle]
- **Perms / cross-platform:** 0600/0700 only via `src/platform/` seam; no `#ifdef` elsewhere. [docs/conventions.md#Cross-platform]
- **No secret in any sink:** the scrubber binds to logs, exception text, and the persistence path; a secret-scrubbing
  domain-error base (adapters wrap raw errors) — at minimum ensure Error messages never carry a raw token. [architecture.md#SEC-3]
- **Errors:** `Result<T>`; reuse `errors::Error`/`make_error`/`ErrorCategory`; `SuggestedAction` ~ block/do-not-retry for
  crypto/secret failures, re-establish-session where a missing token implies re-login.

### References

- [Source: epics.md#Story 2.2] [architecture.md#SE-1..SE-5, #SEC-1/2/3/5, #New modules added]
- [Source: docs/conventions.md] [Source: include/broker_exec/ports/secret_provider.hpp, platform/*.hpp]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
