# Story 2.3: Kite Connect REST client (the transport)

Status: ready-for-dev

## Story

As a platform maintainer,
I want a native C++ Kite Connect REST client behind the adapter port,
so that orders/portfolio/instruments are reachable without an official SDK.

## Acceptance Criteria

1. **Given** the Kite REST client (cpr/libcurl) implementing the transport **When** it calls
   order/portfolio/instrument/margin endpoints **Then** it handles auth headers, request signing,
   pagination, and rate-limit headers.
2. **And** it maps raw HTTP/transport errors to the typed taxonomy (FR-25), scrubbed of secrets.
3. **And** it is exercised by recorded-fixture tests (no live credentials in CI).

## Tasks / Subtasks

- [ ] Task 1: HTTP transport seam (testability) (AC: 1, 3)
  - [ ] `include/broker_exec/adapters/kite/http_client.hpp`: an abstract `HttpClient` with
        `Result<HttpResponse> request(const HttpRequest&)` where `HttpRequest` = {method, url, headers, body,
        query params} and `HttpResponse` = {status_code, headers (incl. rate-limit), body}. Pure virtual; no cpr in the header.
  - [ ] `CprHttpClient` (in `src/adapters/kite/`, links cpr) implements it over cpr/libcurl with a configurable
        base URL + per-request timeout (from config). Transport failures (DNS/connect/timeout) -> typed Error, never throw.
- [ ] Task 2: `KiteRestClient` — the Kite Connect protocol (AC: 1, 2)
  - [ ] `include/broker_exec/adapters/kite/kite_rest_client.hpp` + `src/adapters/kite/kite_rest_client.cpp`.
  - [ ] Constructed with `HttpClient&` + credentials (api_key + access_token sourced via `ports::SecretProvider`,
        NEVER hard-coded/logged). Sets the `Authorization: token <api_key>:<access_token>` header and `X-Kite-Version: 3`.
  - [ ] Methods (typed request/response structs or nlohmann::json in, domain-ish out — keep minimal but real):
        place_order, modify_order, cancel_order, orders (orderbook), trades, positions, holdings, margins, instruments
        (CSV dump — handle as text). Implement enough to be real; parse Kite's `{status, data}` envelope.
  - [ ] Pagination + rate-limit headers: read `X-RateLimit-*`/equivalent response headers and surface remaining/limit
        on the response so the rate limiter (Story 2.12) can consume them. (Kite REST is mostly non-paginated; where a
        list endpoint paginates, follow it; otherwise document "single-page".)
- [ ] Task 3: Error mapping -> typed taxonomy, scrubbed (AC: 2)
  - [ ] Map HTTP status + Kite error `{status:"error", error_type, message}` to `errors::Error` with the right
        `SuggestedAction` (401/403 TokenException -> re-establish-session; 429 -> retry-safe/slow (rate limited);
        5xx -> reconcile-first/retry-safe; 400 input -> do-not-retry; network/timeout -> reconcile-first for dangerous,
        retry-safe for safe reads). Use `domain::scrub` on any message that could carry a token; never include the
        Authorization header or token in an Error/log.
- [ ] Task 4: CMake + Conan (orchestrator pre-wires cpr dep + find_package + add_subdirectory) (AC: all)
  - [ ] `src/adapters/kite/CMakeLists.txt`: target `broker_exec_kite` (+ alias `broker_exec::kite`), links
        `cpr::cpr`, `nlohmann_json::nlohmann_json`, `broker_exec::ports`, `broker_exec::errors`, `broker_exec::domain`
        (scrub), `broker_exec::secrets`/`broker_exec::ports` for SecretProvider; warnings+sanitizers PRIVATE; test exe.
        NOTE: this is in the adapters layer (the only place transports/SDKs live) — boundary rule satisfied.
- [ ] Task 5: Recorded-fixture tests (AC: 3)
  - [ ] A `RecordedHttpClient` test double implementing `HttpClient` that returns canned `HttpResponse`s keyed by
        (method,url) from in-test fixtures (committed JSON strings; NO live creds, NO network).
  - [ ] Tests: a successful place_order parses the order_id; an orderbook/positions/margins response parses; a 401
        TokenException maps to re-establish-session; a 429 maps to the rate-limited action and exposes rate-limit headers;
        a malformed body -> typed Error; assert NO token/Authorization value appears in any error string.

## Dev Notes

- **Layer:** `adapters/kite` — transports/SDK-equivalents live ONLY in adapters (boundary enforced). [architecture.md#Tech revision: no official C++ SDK -> implement Kite REST directly]
- **Libraries:** cpr (libcurl) for REST; nlohmann::json for envelopes. [architecture.md#Library choices]
- **Auth:** Kite Connect uses `Authorization: token api_key:access_token`, `X-Kite-Version: 3`. access_token is the
  daily token from Story 2.4 session establishment (here just consumed via SecretProvider).
- **Errors:** typed taxonomy + `SuggestedAction`; scrub secrets; never throw across boundary. [docs/conventions.md, architecture.md#IA-5/IA-2]
- **No live creds in CI:** the HttpClient seam + RecordedHttpClient is the testing substrate (also the basis for
  the TO-6 VCR-style fixtures and Story 2.14 conformance). [architecture.md#TO-6]
- **Reuse:** `ports::SecretProvider`, `errors::Error`/`make_error`/`SuggestedAction`, `domain::scrub`, `config` for base_url/timeout.

### References
- [Source: epics.md#Story 2.3] [architecture.md#Technology revision, #Library choices, #IA-2/IA-5, #TO-6]
- [Source: docs/conventions.md]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
