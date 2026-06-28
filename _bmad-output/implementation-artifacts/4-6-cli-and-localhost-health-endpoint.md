# Story 4.6: CLI and localhost health endpoint

Status: ready-for-dev

## Story

As an operator,
I want a thin CLI and a localhost health endpoint,
so that I can operate and monitor the engine out-of-band. (FR-36)

## Acceptance Criteria

1. **Given** the CLI verbs (`status`/`reconcile`/`replay-intent-log`/`safe-start-check`/`send-test-alert`/`verify-ip`/`kill`)
   **When** a verb is dispatched **Then** each is a thin shell over the public API (an injected operator-API seam) and is
   **read-only EXCEPT `kill`** — the read-only verbs must never reach a mutating path; only `kill` may mutate.
2. **And** the health endpoint serves an immutable `HealthSnapshot` (session state, heartbeat age, tick age, in-flight
   count, clock sanity, replay-clean) published by the main loop; `GET /healthz` is liveness, `GET /ready` is readiness
   (ready only when the snapshot is fully healthy + session_state == Healthy).
3. **And** the served snapshot JSON is redaction-safe (scrubbed — it is an outbound payload) and the endpoint is bound to
   localhost only, 0600-equivalent; an unknown route is a clean 404 (never a crash, never a leak).

## Tasks / Subtasks

- [ ] Task 1: New module `src/cli` + header dir `include/broker_exec/cli` (AC: all)
  - [ ] Target `broker_exec_cli`; depends inward on `domain` (scrub), `errors`, `ports`, `session` (SessionState).
        Add **CLI11** and **cpp-httplib** Conan deps (both header-only). Wire into root `CMakeLists.txt` + `conanfile.py`.
- [ ] Task 2: `HealthSnapshot` value + JSON (AC: 2, 3) — `include/broker_exec/cli/health_snapshot.hpp` / `health_snapshot.cpp`
  - [ ] Immutable struct: `session::SessionState session_state; std::int64_t heartbeat_age_ms; std::int64_t tick_age_ms;`
        `int in_flight_count; bool clock_sane; bool replay_clean;` (integers only, no float). All-arg construction; const fields
        or const accessors so a published snapshot can't be mutated by a reader.
  - [ ] `[[nodiscard]] std::string to_json(const HealthSnapshot&)` — nlohmann; **run the rendered string through
        `domain::scrub`** before returning (outbound payload; in-memory values are raw — every payload scrubs itself).
  - [ ] `[[nodiscard]] bool is_ready(const HealthSnapshot&)` — true ONLY when fully healthy: `clock_sane && replay_clean &&`
        `session_state == SessionState::Healthy && in_flight_count >= 0 && heartbeat_age_ms` within a passed-in liveness budget.
        (SessionState vocabulary is `{Healthy, NeedsReauth, Failed}` — there is NO "Active".)
  - [ ] `[[nodiscard]] bool is_live(const HealthSnapshot&)` — liveness (process responding): heartbeat age within budget +
        `clock_sane` (a stalled clock is NOT live). Liveness is weaker than readiness.
- [ ] Task 3: `HealthState` publisher (AC: 2) — `health_state.hpp/.cpp`
  - [ ] Single-producer/multi-reader latest-snapshot holder the main loop `publish()`es and the endpoint `latest()` reads.
        Guard with a `std::mutex` (return a value copy — immutable snapshot). Starts EMPTY -> `latest()` returns a
        not-ready/not-live default (fail-closed: before the loop publishes, /ready is 503).
- [ ] Task 4: Health endpoint handlers (AC: 2, 3) — `health_endpoint.hpp/.cpp`
  - [ ] Pure route handler: `HttpReply route(std::string_view method, std::string_view path, const HealthState&)` returning
        `{int status; std::string body; std::string content_type;}`. `GET /healthz` -> 200 if `is_live(latest)` else 503;
        `GET /ready` -> 200 if `is_ready(latest)` else 503; body = scrubbed snapshot JSON. Any other route/method -> 404
        `{"error":"not found"}`. **No throw.** This is the unit-tested seam — NO real socket in tests.
  - [ ] Thin `HealthHttpServer` registering the routes on a cpp-httplib `Server` bound to `127.0.0.1` (a `listen()` wrapper;
        NOT exercised by unit tests — the route() function is). Document the 0600/localhost-only binding contract.
- [ ] Task 5: CLI verb router (AC: 1) — `cli_app.hpp/.cpp`
  - [ ] `struct OperatorApi` = the injected public-API seam: `std::function`s for each verb
        (`status`, `reconcile`, `replay_intent_log`, `safe_start_check`, `send_test_alert`, `verify_ip`, `kill`).
        Each returns `Result<std::string>` (human/audit line, scrubbed). The CLI is a SHELL — it owns no module logic.
  - [ ] `enum class Verb { Status, Reconcile, ReplayIntentLog, SafeStartCheck, SendTestAlert, VerifyIp, Kill };`
        `[[nodiscard]] bool is_mutating(Verb)` — true ONLY for `Kill`. `[[nodiscard]] Result<std::string> dispatch(Verb,
        const OperatorApi&)` routes to the seam; **a null `kill` callback or a null read callback fails closed** (Error, no
        crash). Document: read-only verbs must be wired to read-only API methods — `dispatch` does not itself mutate.
  - [ ] `int run_cli(int argc, char** argv, const OperatorApi&)` builds the CLI11 app (one subcommand per verb), parses,
        dispatches, prints the scrubbed result line, returns 0/non-0. (CLI11 confined to this .cpp.)
- [ ] Task 6: CMake — `src/cli/CMakeLists.txt`
  - [ ] `broker_exec_cli` STATIC (health_snapshot/health_state/health_endpoint/cli_app .cpp); link CLI11::CLI11 +
        httplib::httplib + nlohmann_json + broker_exec_{domain,errors,ports,session}. `broker_exec_cli_tests` (Catch2). No
        socket / no argv parsing in tests — test `route()`, `dispatch()`, `to_json`, `is_ready/is_live`, `is_mutating`.
- [ ] Task 7: Tests — `src/cli/*_test.cpp`
  - [ ] AC-1: `dispatch(Status,...)` calls only the status seam; `is_mutating` true ONLY for Kill; a null kill/read callback
        -> Error (fail-closed, no crash); every read verb maps to its seam (spy callbacks record invocation).
  - [ ] AC-2: `route(GET,/healthz)` 200 when live / 503 when not; `/ready` 200 only when fully healthy + Healthy session, 503
        before any publish (empty HealthState) and when in-flight/clock/replay unhealthy; `/ready` weaker-vs-stronger than
        `/healthz` (a live-but-not-ready snapshot -> healthz 200, ready 503).
  - [ ] AC-3: an unknown route -> 404 (no throw); a snapshot whose session detail carries a token-shaped string is scrubbed
        in `to_json` (no secret in the served body); a `POST /healthz` (wrong method) -> 404.

## Dev Notes

- **Thin shells over the public API** (FR-36): the CLI/endpoint own NO business logic — inject seams (`OperatorApi`,
  `HealthState`). This mirrors the established pattern (alerting POST seam, marketdata tick seam, kite HttpClient seam):
  the real wiring of verbs to live modules + the actual socket bind is the composition root's job (tier-2), NOT this story.
- **Read-only except `kill`** (AC-1): `is_mutating` is the explicit guard; only `Kill` is true. Fail closed on a null seam.
- **Snapshot is an outbound payload** — scrub the rendered JSON (in-memory fields are RAW; lesson from 4-2). [redaction.hpp]
- **Fail-closed readiness:** an empty `HealthState` (loop hasn't published) -> `/ready` 503 and `/healthz` 503. Never default
  to healthy. No throw across the route boundary (404 on anything unexpected). No float; integer ages/counts (ms).
- **Reuse:** `domain::scrub`, `errors` (Error/Result), `session::SessionState`, nlohmann_json. New header-only deps: CLI11,
  cpp-httplib. Cross-platform: no `#ifdef` outside `src/platform/` (cpp-httplib handles the socket portability internally).

### References
- [Source: epics.md#Story 4.6, FR-36] [architecture.md#FR-36 CLI/health surface]
- [Source: include/broker_exec/session/session_state.hpp] [include/broker_exec/domain/redaction.hpp]
- [Source: docs/conventions.md] [Source: src/alerting/* (injected-seam sibling pattern)]

## Dev Agent Record
### Agent Model Used
claude-opus-4-8[1m] (Opus 4.8, 1M context)

### Completion Notes List
- New module `broker_exec_cli` (alias `broker_exec::cli`) under `src/cli` + `include/broker_exec/cli`.
  Depends inward only: domain (scrub), errors (Result/Error), ports, session (SessionState),
  nlohmann_json. CLI11 + cpp-httplib are confined to `cli_app.cpp` / `health_endpoint.cpp` and link
  PRIVATE (their headers never enter a public header).
- `HealthSnapshot` is fully immutable (all `const` members + all-arg constructor); integer/enum-only,
  NO float. `to_json` renders compact JSON and runs it through `domain::scrub` before returning
  (outbound payload). `is_live`/`is_ready` take the liveness budget as a parameter; both fail closed
  on a negative budget/age. `is_ready` is strictly stronger than `is_live`.
- `HealthState` is a `std::mutex`-guarded `std::optional<HealthSnapshot>` holder; `publish()` emplaces
  (snapshot is non-assignable due to const members), `latest()` returns a value copy or the
  `fail_closed_default()` (Failed session, max-int heartbeat age, clock NOT sane) so an unpublished
  state answers 503 on both endpoints. HealthState is intentionally non-movable (owns a mutex).
- `route()` is the pure, no-throw, no-socket seam (the unit-tested one). GET /healthz -> 200/503 on
  liveness, GET /ready -> 200/503 on readiness, anything else (unknown path OR wrong method) -> 404
  `{"error":"not found"}`. `HealthHttpServer` (pImpl over httplib::Server) binds 127.0.0.1; NOT
  unit-tested. Localhost-only/0600 contract documented in the header.
- `dispatch(Verb, OperatorApi)` routes to the injected seam; a null callback fails closed with an
  Internal/BlockStrategy Error (no crash, no UB). `is_mutating` is true for `Kill` ONLY. `run_cli`
  builds the CLI11 app (one subcommand per verb) and scrubs every printed line.
- Tests cover every Task-7 AC bullet via the pure seams only (no socket bind, no argv parsing):
  AC-1 dispatch routing + is_mutating + null-callback fail-closed + each read verb -> its seam (spies);
  AC-2 healthz/ready 200/503 incl. empty-state 503 and live-but-not-ready; AC-3 unknown route 404,
  wrong method 404, scrubbed/redaction-clean to_json body.
- ASSUMPTIONS the build should verify: Conan deps `cli11/2.4.2` and `cpp-httplib/0.15.3` (both
  header-only, default options — cpp-httplib's SSL/zlib/brotli stay OFF, so no OpenSSL coupling).
  CMake targets `CLI11::CLI11` and `httplib::httplib` via `find_package(CLI11 REQUIRED)` /
  `find_package(httplib REQUIRED)` (cpp-httplib's CMakeDeps file/target name is `httplib`). If the
  pinned versions are unavailable on the configured remote, bump to the nearest conancenter revision;
  target names are stable.

### File List
- include/broker_exec/cli/health_snapshot.hpp (new)
- include/broker_exec/cli/health_state.hpp (new)
- include/broker_exec/cli/health_endpoint.hpp (new)
- include/broker_exec/cli/cli_app.hpp (new)
- src/cli/health_snapshot.cpp (new)
- src/cli/health_state.cpp (new)
- src/cli/health_endpoint.cpp (new)
- src/cli/cli_app.cpp (new)
- src/cli/CMakeLists.txt (new)
- src/cli/health_snapshot_test.cpp (new)
- src/cli/health_endpoint_test.cpp (new)
- src/cli/cli_app_test.cpp (new)
- CMakeLists.txt (edited — find_package CLI11/httplib + add_subdirectory(src/cli)) [orchestrator-owned]
- conanfile.py (edited — cli11/2.4.2 + cpp-httplib/0.15.3) [orchestrator-owned]
