# Story 2.4: Kite daily session establishment and expiry detection

Status: ready-for-dev

## Story

As an operator,
I want the daily Kite login modeled as first-class establishment,
so that the bot trades after the day's token is supplied and blocks otherwise. (FR-21)

## Acceptance Criteria

1. **Given** Kite has no headless refresh (token dies ~6am) **When** the bot cold-starts **Then**
   `establish_session` exchanges an operator-supplied request_token for the access_token (stored encrypted)
   before the safe-start gate.
2. **And** a dead daily token blocks trading (never a silent "refresh") with a typed alert/error
   (SuggestedAction::ReEstablishSession), surfaced as a normalized session state.
3. **And** the `headless session refresh` capability for Kite reads unsupported.

## Tasks / Subtasks

- [ ] Task 1: `session` module (AC: all)
  - [ ] `include/broker_exec/session/` + `src/session/`; target `broker_exec_session` (+ alias). Upper-layer
        orchestration: may depend on `adapters/kite` (HttpClient seam), `secrets` (TokenStore), `ports`, `errors`.
  - [ ] Normalized `enum class SessionState { Healthy, NeedsReauth, Failed };` (IBR-6 normalized login result).
- [ ] Task 2: `KiteSessionEstablisher` — daily login (AC: 1)
  - [ ] Ctor: `(const adapters::kite::HttpClient& http, const ports::SecretProvider& secrets, secrets::TokenStore& store,
        std::string account_id, secret-name config)` — secret names for api_key + api_secret.
  - [ ] `Result<SessionState> establish(std::string request_token)`:
        compute `checksum = SHA256_hex(api_key + request_token + api_secret)` (OpenSSL SHA256), POST `/session/token`
        form-encoded `{api_key, request_token, checksum}`, parse `data.access_token` from the Kite `{status,data}`
        envelope, and persist it ENCRYPTED via `TokenStore::save(account, "kite.access_token", access_token)`.
        On a Kite error envelope / non-2xx, map via `kite::map_http_error` -> typed Error (scrubbed). Never log the
        request_token/access_token/api_secret/checksum. Return `Healthy` on success.
- [ ] Task 3: Expiry detection (AC: 2)
  - [ ] `Result<SessionState> validate()` (or `check_session()`): issue a lightweight authenticated read
        (e.g. `GET /user/margins/equity` or `/portfolio/positions`) using the stored access_token via a `KiteRestClient`
        (or the HttpClient directly with auth headers); a 401/`TokenException` -> `NeedsReauth` (NOT an error to retry
        silently); a transport/5xx -> a reconcile-first Error; success -> `Healthy`.
  - [ ] A dead token path returns `NeedsReauth` + the caller (safe-start, Story 2.13) blocks trading. Provide a typed
        Error helper (SuggestedAction::ReEstablishSession) so the alerting path (Epic 4) can raise the operator alert.
        NEVER auto-refresh.
- [ ] Task 4: Capability bit (AC: 3)
  - [ ] Expose `static constexpr bool kSupportsHeadlessSessionRefresh = false;` (or a small accessor) documenting Kite's
        no-headless-refresh reality. Story 2.5 folds this into the full capability model; here it must read unsupported.
- [ ] Task 5: CMake (orchestrator pre-wires root add_subdirectory(src/session); no new Conan dep — OpenSSL already present)
  - [ ] `src/session/CMakeLists.txt`: target `broker_exec_session` links `broker_exec::kite`, `broker_exec::secrets`,
        `broker_exec::ports`, `broker_exec::errors`, `OpenSSL::Crypto` (SHA256), `nlohmann_json` (envelope parse if needed),
        warnings+sanitizers PRIVATE; test exe `broker_exec_session_tests`.
- [ ] Task 6: Tests (AC: 1, 2, 3) — `src/session/session_test.cpp`
  - [ ] A `RecordedHttpClient`-style double returns a canned `/session/token` success -> establish() returns Healthy and
        the access_token is persisted (verify by loading it back from a real TokenStore in a temp dir, or by asserting
        save() was called with the decrypted value).
  - [ ] The `/session/token` request carries the correct `checksum` for known (api_key, request_token, api_secret)
        (assert the SHA256 hex against an independently-computed expected value).
  - [ ] establish() never leaks the request_token/access_token/api_secret/checksum into any Error message.
  - [ ] validate() on a 401 TokenException -> `NeedsReauth`; on success -> `Healthy`.
  - [ ] `kSupportsHeadlessSessionRefresh == false`.

## Dev Notes

- **Kite session exchange:** `POST https://api.kite.trade/session/token` with `api_key`, `request_token`,
  `checksum = SHA-256(api_key + request_token + api_secret)`; response `{status:"success", data:{access_token, ...}}`.
  access_token dies daily (~6am) with NO headless refresh. [architecture.md#E. Sessions, IBR-1]
- **Reuse:** `adapters::kite::HttpClient`/`HttpResponse`/`map_http_error` (Story 2.3); `secrets::TokenStore` +
  `ports::SecretProvider` (Story 2.2); `errors::Error`/`SuggestedAction::ReEstablishSession`.
- **Secrets:** api_secret + tokens via SecretProvider/TokenStore; NEVER logged or placed in an Error; the checksum is
  derived from the api_secret so it is also sensitive — keep it out of logs/Errors. [docs/conventions.md, architecture.md#SEC-3]
- **No headless refresh:** capability reads unsupported; the daily token is operator-supplied (request_token at boot).
- **Boundary:** session is an upper module (not domain/ports), so depending on adapters/kite is allowed.

### References
- [Source: epics.md#Story 2.4] [architecture.md#E. Sessions & Indian-broker realism — IBR-1, IBR-6]
- [Source: docs/conventions.md] [Source: include/broker_exec/adapters/kite/*, include/broker_exec/secrets/*]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
