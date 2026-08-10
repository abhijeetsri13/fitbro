# Story 4.3: Alerting with dead-man's-switch

Status: ready-for-dev

## Story

As an operator,
I want push alerts that themselves cannot fail silently,
so that a silent breach reaches my phone. (FR-28)

## Acceptance Criteria

1. **Given** one `AlertSink` with Telegram + generic webhook **When** any named alert condition fires **Then** it is
   delivered, and a `send_test_alert()` exists per channel.
2. **And** the alerter emits a periodic heartbeat whose absence an external watcher alarms on.
3. **And** killing the alerter triggers the absence alarm in test.

## Tasks / Subtasks

- [ ] Task 1: `alerting` module (AC: all)
  - [ ] `include/broker_exec/alerting/` + `src/alerting/`; target `broker_exec_alerting` (+ alias). Depends inward on `ports`
        (AlertSink, ClockPort), `domain` (scrub), `errors`, + nlohmann (channel body JSON). NO new Conan dep — delivery goes
        through an INJECTED POST seam so CI needs no Telegram/webhook network; the real cpr HTTP wiring is a thin adapter behind
        the seam (follow-up, like the cpr transport behind the kite HttpClient seam).
- [ ] Task 2: Channels + the POST seam (AC: 1)
  - [ ] `struct AlertChannel { enum class Kind { Telegram, Webhook }; Kind kind; std::string url; std::string chat_id; };`
        (chat_id used for Telegram). `using PostFn = std::function<Result<ports::Ok>(std::string_view url, std::string_view body)>;`
        (tests inject a capturing fn; production wires cpr POST — out of scope here).
- [ ] Task 3: `MultiChannelAlertSink : ports::AlertSink` (AC: 1)
  - [ ] Ctor `(PostFn post, std::vector<AlertChannel> channels, const ports::ClockPort& clock)`.
  - [ ] `Result<ports::Ok> send(ports::AlertLevel level, const std::string& message) override`:
        SCRUB the message via `domain::scrub` FIRST (no token in any outbound body — the 4.2 lesson: every outbound message
        scrubs itself, the in-memory message is NOT pre-scrubbed); build a per-channel JSON body (Telegram: `{chat_id, text}`;
        Webhook: `{level, message, ts}`) with the scrubbed text; POST to each channel via the seam. Delivery is best-effort
        across channels: return `ok()` if AT LEAST ONE channel delivered; if ALL fail -> an Error (the alert could not reach
        the operator — itself a condition the dead-man's-switch backstops). Also `beat()` the heartbeat on a successful send
        (a delivered alert is itself a sign of life). No throw.
  - [ ] `Result<ports::Ok> send_test_alert() override`: send a fixed "test alert" message to EVERY channel (proves each channel
        is wired/reachable, AC-1). ok() iff all channels accepted (a test must verify EACH channel, so require all here).
- [ ] Task 4: Dead-man's-switch heartbeat (AC: 2, 3)
  - [ ] `class HeartbeatMonitor` (ctor `(const ports::ClockPort& clock)`): `void beat() noexcept` stamps
        `last_beat_ = clock.now_steady()` and sets `seen_ = true`; `[[nodiscard]] bool is_alive(std::chrono::milliseconds max_gap) const`
        returns `seen_ && (clock.now_steady() - last_beat_) <= max_gap`. (An EXTERNAL watcher polls is_alive on its own clock;
        the alerter beats on each heartbeat tick. Killing the alerter = no more beats = after max_gap, is_alive flips false — AC-3.)
  - [ ] The `MultiChannelAlertSink` can hold/own a `HeartbeatMonitor` (or accept one by reference) and `beat()` it on each
        successful delivery + on an explicit periodic heartbeat the alerter emits; expose `heartbeat()` to send a heartbeat alert
        AND beat. Document the watcher contract.
- [ ] Task 5: CMake (orchestrator pre-wires root add_subdirectory(src/alerting); NO new Conan dep)
  - [ ] `src/alerting/CMakeLists.txt`: links PUBLIC `broker_exec::ports` `broker_exec::errors`; PRIVATE `broker_exec::domain`
        `nlohmann_json::nlohmann_json` warnings+sanitizers. Test exe `broker_exec_alerting_tests` ALSO links `broker_exec::clock`
        (TestClock) + `broker_exec::domain`.
- [ ] Task 6: Tests (AC: 1, 2, 3) — `src/alerting/alerting_test.cpp` (a capturing PostFn recording (url, body) per call; TestClock)
  - [ ] AC-1 deliver: send(Warning, "msg") with a Telegram + a Webhook channel -> the capturing seam recorded 2 POSTs, each to
        the right url, body carrying the message; returns ok().
  - [ ] AC-1 test-alert: send_test_alert() -> a fixed test message posted to EVERY channel.
  - [ ] REDACTION (4.2 lesson): send a message containing a 32-char alnum synthetic token -> ZERO occurrences of the token in
        ANY posted body (domain::scrub applied to the outbound body).
  - [ ] best-effort: a seam that fails ONE channel but succeeds the other -> send returns ok(); a seam failing ALL channels ->
        send returns Error (the alert could not reach the operator).
  - [ ] AC-2/AC-3 dead-man's-switch: HeartbeatMonitor; before any beat -> is_alive(gap)==false; beat() then within gap -> true;
        advance the TestClock steady clock PAST max_gap with NO further beat (the alerter "killed") -> is_alive==false (the absence
        alarm condition the external watcher fires on). A subsequent beat -> alive again.
  - [ ] a successful send beats the heartbeat (is_alive true right after a delivered alert).

## Dev Notes

- **Cannot-fail-silently** — best-effort multi-channel delivery + a dead-man's-switch heartbeat: even if every channel is down,
  the absence of heartbeats trips the EXTERNAL watcher's absence alarm. [architecture.md#FR-28 alerting + dead-man's-switch, #CC-6]
- **Injected POST seam, no network** — delivery via `std::function<Result<Ok>(url, body)>`; the cpr HTTP channel is a thin
  adapter behind the seam (follow-up). [architecture.md#FR-28, ID-3]
- **Every outbound message scrubs itself** (the Story-4.2 lesson) — `domain::scrub` on the alert body; the in-memory message is
  NOT pre-scrubbed. [Story 4.2, architecture.md#SEC-3]
- **Heartbeat on the steady clock** (monotonic — a wall jump cannot fake liveness). [docs/conventions.md, FR-23]
- **Reuse:** `ports::AlertSink`/`ClockPort`/`AlertLevel`, `domain::scrub`, `errors`, `clock::TestClock` (tests), nlohmann.

### References
- [Source: epics.md#Story 4.3] [architecture.md#FR-28 alerting, #CC-6 dead-man's-switch, #ID-3] [Source: include/broker_exec/ports/alert_sink.hpp]
- [Source: docs/conventions.md] [Source: include/broker_exec/domain/redaction.hpp]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
