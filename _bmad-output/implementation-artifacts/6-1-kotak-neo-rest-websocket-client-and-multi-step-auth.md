# Story 6.1: Kotak Neo REST+WebSocket client and multi-step auth

Status: ready-for-dev
Epic: 6 (Breadth — Kotak Neo Adapter & Multi-Account)
FRs: FR-1, FR-25 · Architecture: IBR-6 (Kotak multi-step auth), IA-2, SEC-3

## Story

As a platform maintainer,
I want a native Kotak Neo transport with its multi-step auth,
So that Kotak is reachable without an official C++ SDK.

## Acceptance Criteria

1. **Multi-step auth**: the Kotak Neo client performs the documented multi-step flow
   (consumer-key Basic auth → session/view token → MPIN/TOTP 2FA → final session bundle),
   returning normalized `SessionState` HEALTHY / NEEDS_REAUTH / FAILED. The session
   "token" is an **opaque per-broker bundle** (auth token + sid + server-id/hsServerId +
   any 2FA artifacts), not one string (arch IBR-6).
2. **Fixtures resolve unknowns**: the day-one-critical-path `unknown`s (auth-flow shape,
   margin/funds API envelope) are covered by **committed recorded-response fixtures** in
   the tests (VCR substrate, TO-6 tier-1). Live tier-2 verification stays a tracked
   operator step (documented).
3. **Typed, scrubbed errors**: raw Kotak errors (`stat:"Not_Ok"`, `errMsg`, fault codes,
   HTTP 401/403/429/5xx) map to the typed taxonomy with correct `SuggestedAction`
   (session-death → SessionExpired/ReEstablishSession; 429 → RateLimited/RetrySafe;
   5xx/transport on a mutation → ReconcileFirst — NEVER DoNotRetry for a possibly-live
   order); every message `domain::scrub`-ed; **no credential (consumer secret, MPIN,
   TOTP, tokens, sid) ever stored on an Error, logged, or persisted**.

## Planning decisions (binding for dev)

- **Layout**: `include/broker_exec/adapters/kotak/*.hpp` + `src/adapters/kotak/*.cpp`,
  namespace `broker_exec::adapters::kotak`, target `broker_exec_adapters_kotak`
  (mirror the kite adapter CMake exactly; tests `broker_exec_adapters_kotak_tests`).
- **Transport seam reuse**: reuse the existing broker-agnostic
  `adapters::kite::HttpClient/HttpRequest/HttpResponse` seam via namespace aliases in
  the kotak namespace (`using HttpClient = kite::HttpClient;` …). Do NOT move the
  header (hoist-to-`adapters/http/` is a tracked tier-2 refactor; no ripple now).
  No new Conan dep: the concrete transport is the existing cpr client; tests use a
  scripted fake `HttpClient` with recorded fixtures.
- **WebSocket**: NO IXWebSocket dep this story. Provide the pure protocol layer only:
  subscribe/unsubscribe frame builders + message classifier (order-update vs tick vs
  heartbeat) as pure functions over an injected send/receive seam, feeding the existing
  `marketdata`/`feedsub` seams. Real socket transport = tier-2 follow-up (documented in
  the header). This mirrors how 3-5/IMP-10 landed.
- **Pieces**:
  1. `kotak_session.hpp/.cpp` — `KotakSessionBundle` (opaque fields), multi-step
     `KotakSessionEstablisher` over the HttpClient seam: `establish(LoginInputs)` runs
     step1 (view token via consumer creds + mobile/password), step2 (2FA MPIN or TOTP →
     final token+sid). Persists the bundle **encrypted via the existing TokenStore**
     (serialize bundle → JSON → encrypt); `validate()` loads + probes a cheap read;
     session-death → `SessionState::NeedsReauth`. `kSupportsHeadlessSessionRefresh` for
     Kotak = **Unknown** (fail-closed; per capability model).
  2. `kotak_rest_client.hpp/.cpp` — mirrors `KiteRestClient` surface: place/modify/
     cancel order, orders/trades/positions/holdings/margins(limits), scrip-master
     (CSV/text). Kotak envelope parse (`stat`/`Ok`/`Not_Ok`, `data`, `errMsg`,
     `fault`), auth headers from the session bundle (Auth token + Sid + neo-fin-key
     etc.) built ONLY at call site.
  3. `kotak_errors.hpp/.cpp` — `map_kotak_error(response)` single mapping point
     (reuse `brokerreason` classifier where phrasing-based; fail-closed unknown →
     DoNotRetry for rejects, Indeterminate/ReconcileFirst for timeout/5xx on mutation).
  4. `kotak_ws_protocol.hpp/.cpp` — pure frame builders/classifier (no socket).
  5. `kotak_capabilities()` — tri-state CapabilitySet: verified-by-fixture entries
     Supported, everything unverified stays **Unknown** (zero-init fail-closed),
     HeadlessSessionRefresh Unknown, OrderUpdateWS Unknown until 6-2.
- **Tests (Catch2)**: fixture-driven — committed JSON fixture strings for: 2-step auth
  happy path; wrong-MPIN (auth step-2 fail → Failed, nothing persisted); expired sid on
  a read (→ NeedsReauth); 429 with Retry-After; 5xx on place_order (→ ReconcileFirst);
  margin/limits envelope parse; scrub assertions (no MPIN/TOTP/token/sid substring in
  any Error message or serialized anything except the encrypted store); WS frame
  build/classify round-trip. Assert bundle persists encrypted (not plaintext on disk).
- **Conventions**: docs/conventions.md binding — C++20, `Result<T>`/`std::expected`,
  no-throw across boundary, no float in money paths (paise integers), no `#ifdef _WIN32`
  outside platform/, warnings+sanitizers targets linked PRIVATE, domain/ports never
  link adapters.

## Dev notes

- Kotak Neo API (public docs / github kotak-neo-api): base `https://gw-napi.kotaksecurities.com/`;
  auth: (1) OAuth token via consumer key/secret Basic → `access_token`; (2) login
  `/login/1.0/login/v2/validate` with mobileNumber/password → `view` token + `sid`;
  (3) 2FA same endpoint with MPIN (or TOTP path) → final `token` + `sid` + `hsServerId`;
  subsequent calls: `Authorization: Bearer <access_token>`, `Auth: <token>`, `Sid: <sid>`.
  Order endpoints under `Orders/2.0/quick/order/rule/ms/place?sId=<hsServerId>` style;
  envelope `{"stat":"Ok", ...}` / `{"stat":"Not_Ok","errMsg":...,"stCode":...}` and
  sometimes `{"fault":{"code":...,"message":...}}`. Model these EXACT shapes in fixtures;
  where the real value is uncertain, the fixture is our recorded assumption and the
  capability stays Unknown (tier-2 resolves).
- Secrets via `ports::SecretProvider` names: `kotak_consumer_key`, `kotak_consumer_secret`,
  `kotak_mobile`, `kotak_password`, `kotak_mpin` (fetch lazily, never stored).
- Reuse `domain::scrub` on every outward string; extend nothing in domain.

## Tasks

- [ ] kotak_session (bundle + establisher + validate + TokenStore persistence)
- [ ] kotak_rest_client (surface parity with KiteRestClient)
- [ ] kotak_errors single mapping point
- [ ] kotak_ws_protocol pure layer
- [ ] kotak_capabilities tri-state
- [ ] CMake target + root wiring + tests, all green
