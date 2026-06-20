---
stepsCompleted: [1, 2, 3]
inputDocuments:
  - prds/prd-fitbro-2026-06-17/prd.md
  - architecture.md
  - ../specs/spec-broker-neutral-execution/SPEC.md
  - ../specs/spec-broker-neutral-execution/order-lifecycle.md
  - ../specs/spec-broker-neutral-execution/risk-controls.md
  - ../specs/spec-broker-neutral-execution/resilience-and-reconciliation.md
  - ../specs/spec-broker-neutral-execution/broker-capabilities.md
  - ../specs/spec-broker-neutral-execution/error-taxonomy.md
  - ../specs/spec-broker-neutral-execution/operational-modes.md
  - ../specs/spec-broker-neutral-execution/scope-and-phasing.md
  - ../specs/spec-broker-neutral-execution/technical-architecture.md
---

# Broker-Neutral Trading Execution Library - Epic Breakdown

## Overview

This document decomposes the PRD (FR-1…FR-37), the resolved Architecture (decisions D1–D8 + IA/DA/SE/ID + the adversarial-review resolutions), and the SPEC (CAP-1…CAP-34) into implementable stories. There is **no UX document** — this is a headless library (operator surface = thin CLI + localhost health endpoint), so the UX-DR section is intentionally empty.

## Requirements Inventory

### Functional Requirements

FR-1: Broker-neutral order & data API — place/modify/cancel/square-off/read through one interface; zero broker-specific identifiers in strategy code.
FR-2: Capability model & early rejection — per-broker capability set; MVP posture = reject-only (per-capability substitution deferred).
FR-3: Instrument-master lifecycle — daily refresh, cache, symbol↔token resolution, derived lot/tick/freeze/expiry, staleness gate.
FR-4: Trading-calendar source & staleness gate — sourced/refreshed/cached holiday & special-session calendar; stale ⇒ block safe-start.
FR-5: Corporate-action awareness — recognize split/bonus/symbol-ISIN change/F&O adjustment vs a mismatch; token re-resolution.
FR-6: Pre-submission validation gate — lot/freeze (over-freeze ⇒ slice), tick, product, exchange, funds (fail-closed-on-stale), risk, time, duplicate, hedge, kill-switch, UNKNOWN-pause.
FR-7: Unique client-side reference & idempotency — `<strategy>-<sig8>-<uuid>`; deterministic child refs `<parent>#<k>` for slices.
FR-8: Write-ahead intent log — fsync before the broker socket write; replay on boot (one fsync on the hot path).
FR-9: Order lifecycle state machine incl. UNKNOWN — transition table, terminal-absorbing/forward-progressing apply-ordering, parent↔child slicing, `MANUAL_INTERVENTION_REQUIRED`.
FR-10: No blind retry of dangerous operations — place/modify/cancel/square-off never auto-repeat; reconcile-then-decide.
FR-11: Continuous reconciliation — orders/trades/positions/holdings/funds on all triggers; fetch-off-loop/apply-on-loop; adaptive cadence.
FR-12: Manual-intervention detection.
FR-13: Funds/margin refresh cadence + fail-closed gate — owner `risk/funds_view.py`; never validate margin against a view older than cadence.
FR-14: Crash recovery / reconcile-before-resume — load state, check session, fetch broker, resolve unknowns, resume only when safe.
FR-15: Four-level risk rules — account/strategy/instrument/order.
FR-16: Option-selling safety — hedge-first, naked-sell prevention, emergency behavior.
FR-17: Basket / multi-leg execution — leg dependency, partial-fill, rollback.
FR-18: Margin/SPAN shock simulation — capability-gated (offered only where a basket/SPAN margin source exists).
FR-19: Market-data states & tradable-price validation — Live/Stale/Disconnected/Delayed/Unknown, reconnect, dedup, REST fallback.
FR-20: Liveness & process self-health watchdogs — connected-but-mute feed + disk/mem/handle health.
FR-21: Session management + daily establishment — establishment distinct from expiry-detection (Kite daily login/TOTP, no headless refresh).
FR-22: Safe-start cold-boot gate — session, reconciliation, clock, config, fresh instrument master + calendar, egress-IP, crypto keys.
FR-23: Clock abstraction & skew/stall detection — injectable clock; degrade to exit-only on drift/stall.
FR-24: Rate-limit protection with exit priority — token bucket, reserved exit lane, per-send timeout.
FR-25: Normalized error taxonomy & suggested action — stable categories; no broker error-string parsing in strategy code.
FR-26: Safe degradation postures — coordinator `modes/posture.py` mapping each detector signal to a defined posture.
FR-27: Per-order provenance & reports — full audit record; structured logs, trade journal, daily/error/reconciliation reports.
FR-28: Alerting with dead-man's-switch — Telegram + webhook via one `AlertSink`; absence alarm; `send_test_alert()`.
FR-29: Tamper-evident ledger, EOD report & position heartbeat — hash-chained + per-account-signed; key-scoped tamper-evidence.
FR-30: Trading modes — live/paper/dry-run/replay (clock bound to recorded timeline)/monitor-only/exit-only/emergency.
FR-31: Kill switches — soft/strategy/broker/account/panic + operator→engine control-plane transport.
FR-32: Multi-strategy isolation — per-strategy limits/tags/P&L/virtual positions; global account risk still enforced.
FR-33: Multi-account / process-per-account — one OS process per account + thin supervisor; shared per-broker refdata cache.
FR-34: Config-driven operation — same code across dev/paper/live-small/live-prod by config only.
FR-35: Secret hygiene — env/secret-manager, encrypted at rest, never in logs/intent-log/ledger; per-account keys; layered redaction.
FR-36: Library + thin CLI + localhost health endpoint — read-only except `kill`; CLI verbs double as the executable checklist.
FR-37: Adversarial fake broker & adapter conformance kit — fault injection + per-adapter certification (**Day-One** verification substrate).

### NonFunctional Requirements

NFR-1: Durability — an order's intent is fsync'd before its socket write; one fsync on the critical path; idempotency pre-check is an in-memory index read.
NFR-2: Correctness over speed — synchronous, single-threaded decision core; async confined to a single adapter if ever needed.
NFR-3: Zero duplicate orders — a hard invariant, proven across the full fake-broker fault matrix (SM-1).
NFR-4: Schema versioning — persisted-state + event schema version-stamped; fail visibly on unknown/newer; an older readable intent log replays in degraded reconcile/exit-only (no mid-day brick).
NFR-5: Performance budgets — one fsync + in-memory gate < ~50 ms/order; full reconcile < ~5 s; tick→tradable-state < ~100 ms — confirmed on the real (often network-attached) VPS storage class under IO load.
NFR-6: Security — secrets from env/secrets-manager, encrypted at rest, never in any log/error/intent-log/ledger output; per-account Fernet keys; health endpoint localhost/UDS only.
NFR-7: fsync honesty — durability is conditional on operator storage honoring fsync; startup/operator probe + production-checklist item.
NFR-8: Observability schema is a versioned public contract — field renames are breaking changes.
NFR-9: Footprint — runs on a single modest VPS; one process per account; no external DB daemon at MVP.
NFR-10: Determinism / testability — verifiable by SIGKILL + an injected Clock; replay binds the Clock to the recorded timeline.

### Additional Requirements

(From Architecture — `architecture.md`, incl. the Adversarial Review Findings & Resolutions.)

- **Greenfield starter (Epic 1, Story 1) — C++:** CMake (≥3.28) + **Conan v2** scaffold; `include/broker_exec/` + `src/` layout; C++20; clang-format + clang-tidy + sanitizers (ASan/UBSan/TSan); per-layer CMake targets enforce the hexagonal boundary (`domain` links no adapter/transport target — the import-linter analog). CI = GitHub Actions matrix (gcc+clang, Debug+Release+sanitizers): CMake build + ctest + conformance + SIGKILL durability. **Libraries:** cpr/libcurl (REST), IXWebSocket (WS), nlohmann/json, toml++, spdlog/fmt, OpenSSL + libsodium, SQLite C API, CLI11, cpp-httplib, Catch2.
- **Hexagonal architecture:** domain + ports pure; adapters the only place broker SDKs are imported; strategy-facing facade `api.py`.
- **`dispatch()` send chokepoint:** the single broker-mutation path (record→fsync→send→record); no `store`/`ledger` write outside `runtime/mainloop.py` (import-linter + review-blocking anti-pattern).
- **New modules surfaced by the adversarial review:** `runtime/control.py` (kill control-plane: authenticated POST → main-loop queue), `session/establish.py` (daily session establishment), `modes/posture.py` (FR-26 degradation coordinator), `risk/funds_view.py` (FR-13 funds cadence + fail-closed), `risk/slicing.py` (freeze-quantity slicer), `domain/redaction.py` (shared secret-shape scrubber for logs **and** persistence), shared `refdata/` cache + cross-process file lock.
- **Persistence:** fsync'd append-only intent log + SQLite (WAL, `synchronous=FULL`) projection rebuildable from the log; numbered transactional migrations + `schema_version`; corrupt/half-migrated SQLite ⇒ rebuild-from-intent-log.
- **Deployment:** systemd template unit `broker-exec@<account>.service` (process-per-account) + supervisor exit-code contract (CRASH ⇒ restart-with-backoff; `FAIL_CLOSED_NEEDS_HUMAN` ⇒ do-not-restart + dead-man's-switch alarm).
- **Per-epic verification spikes (tracked):** live-SDK resolution of capability/error `unknown`s — esp. **Kotak Neo margin/funds API + multi-step auth flow** (Day-One-critical-path, spike before the Kotak adapter epic); perf benchmark harness on the real VPS storage class; fsync-honesty probe on target storage; VCR-style recorded broker-error fixtures for CAP-13 cross-broker equivalence.

### UX Design Requirements

_None — headless library; no UI. Operator surface is the thin CLI + localhost health endpoint (covered by FR-36)._

### FR Coverage Map

- FR-1 (broker-neutral API): Epic 1 (interface + fake adapter), Epic 6 (Kotak realization)
- FR-2 (capability model): Epic 2 (Kite), Epic 6 (Kotak)
- FR-3 (instrument master): Epic 2
- FR-4 (trading calendar): Epic 2
- FR-5 (corporate actions): Epic 3
- FR-6 (validation gate): Epic 2
- FR-7 (idempotency/client-ref + child refs): Epic 1
- FR-8 (write-ahead intent log): Epic 1
- FR-9 (lifecycle FSM + apply-ordering + parent/child): Epic 1
- FR-10 (no blind retry): Epic 1
- FR-11 (continuous reconciliation): Epic 3
- FR-12 (manual-intervention detection): Epic 3
- FR-13 (funds/margin cadence + fail-closed): Epic 2
- FR-14 (crash recovery): Epic 3
- FR-15 (four-level risk): Epic 2
- FR-16 (option-selling safety): Epic 5
- FR-17 (basket execution): Epic 5
- FR-18 (margin/SPAN shock sim): Epic 5
- FR-19 (market-data states): Epic 3
- FR-20 (liveness/self-health watchdogs): Epic 3
- FR-21 (session mgmt + daily establishment): Epic 2 (Kite), Epic 6 (Kotak multi-step)
- FR-22 (safe-start gate): Epic 2
- FR-23 (clock + skew/stall): Epic 1
- FR-24 (rate-limit + exit lane): Epic 2
- FR-25 (error taxonomy): Epic 1 (model), Epic 6 (Kotak mapping)
- FR-26 (safe-degradation coordinator): Epic 3
- FR-27 (provenance & reports): Epic 4
- FR-28 (alerting + dead-man's-switch): Epic 4
- FR-29 (tamper-evident ledger + EOD + heartbeat): Epic 4
- FR-30 (trading modes): Epic 4
- FR-31 (kill switches + control plane): Epic 3
- FR-32 (multi-strategy isolation): Epic 6
- FR-33 (multi-account / process-per-account): Epic 6
- FR-34 (config-driven): Epic 2
- FR-35 (secret hygiene + redaction): Epic 2
- FR-36 (library + CLI + health endpoint): Epic 4
- FR-37 (fake broker + conformance kit): Epic 1

> **Cross-cut:** freeze-quantity slicing — the parent→child *model* (FSM, deterministic child client-ref, `UNIQUE` dedupe) lands in **Epic 1** (architecture-freezing per the review); the slicer + over-freeze gate behavior surfaces in **Epic 2** (gate) and is most exercised in **Epic 5** (option size). The secret-shape scrubber (`domain/redaction.py`) is built in **Epic 2** (FR-35) but its persistence-path binding is verified again in **Epic 4** (ledger). Per-epic verification spikes (Kotak margin/funds + auth flow, perf benchmarks, fsync-honesty) attach to the epic that first needs them.

## Epic List

### Epic 1: Order-Safety Substrate (provable zero-duplicate against a fake broker)
Stand up the project and the safety core: the broker-neutral interface + the durable Store (fsync'd write-ahead intent log + SQLite projection + migrations) + the order-lifecycle state machine (UNKNOWN, apply-ordering, parent/child slicing model) + idempotency + the `dispatch()` chokepoint + the injected Clock + the typed error taxonomy + the **adversarial fake broker and adapter conformance kit**. **Demonstrable value:** a strategy places orders through the neutral interface against the fake broker and the headline invariant — *zero duplicate orders across the full fault matrix, incl. SIGKILL-between-fsync-and-send* (SM-1) — is provable. Fully standalone (no real broker).
**FRs covered:** FR-1, FR-7, FR-8, FR-9, FR-10, FR-23, FR-25, FR-37. (Starter `uv init` = Story 1.)

### Epic 2: Live Kite Trading — Validation Gate, Risk, Reference Data & Session
Make it trade safely on a real Zerodha Kite account: the Kite adapter + capability model, the instrument-master and trading-calendar lifecycles (staleness-gated), the pre-submission validation gate (incl. freeze-slicing + UNKNOWN-pause), the four-level risk engine, funds/margin cadence with fail-closed, rate-limit protection with a reserved exit lane, config-driven operation, secret hygiene, daily **session establishment**, and the **safe-start cold-boot gate**. **Demonstrable value:** a strategy runs validated, risk-checked orders on Kite in paper/dry-run/live-min-qty, blocked at safe-start until session + reference data are fresh. Builds on Epic 1.
**FRs covered:** FR-2, FR-3, FR-4, FR-6, FR-13, FR-15, FR-21, FR-22, FR-24, FR-34, FR-35.

### Epic 3: Resilience — Reconciliation, Recovery, Safe Degradation & Kill Switches
Survive failure: continuous reconciliation (fetch-off-loop/apply-on-loop, adaptive cadence), manual-intervention and corporate-action detection, crash recovery (reconcile-before-resume), market-data tradability states, liveness/self-health watchdogs, the degradation-posture coordinator, and the five kill switches with the operator→engine control-plane transport. **Demonstrable value:** the bot reconciles against the broker, recovers cleanly after a crash, degrades to a defined safe posture under each failure, and the operator can kill it (soft→panic). Builds on Epics 1–2.
**FRs covered:** FR-5, FR-11, FR-12, FR-14, FR-19, FR-20, FR-26, FR-31.

### Epic 4: Operator Visibility — Observability, Alerts, Ledger, Modes & Surface
Let the operator stop watching: full per-order provenance + structured logs/audit/reports, alerting (Telegram + webhook) with the dead-man's-switch heartbeat, the tamper-evident hash-chained ledger + EOD signed reconciliation report + push position heartbeat, the trading modes (incl. replay with Clock bound to the recorded timeline), and the thin CLI + localhost health endpoint. **Demonstrable value:** a silent breach pushes to the operator's phone; an EOD report closes the day; `kill`/`status`/`reconcile` run from the CLI. Builds on Epics 1–3.
**FRs covered:** FR-27, FR-28, FR-29, FR-30, FR-36.

### Epic 5: Option-Selling Safety — Hedge-First, Baskets, Slicing & Margin Shock
The primary JTBD: hedge-first execution with naked-sell prevention, multi-leg basket execution (leg dependency, partial-fill, rollback), freeze-quantity slicing exercised at option size, and the capability-gated margin/SPAN shock simulation. **Demonstrable value:** a hedged NIFTY/BANKNIFTY option-selling basket executes as one logical trade — hedge before short, sliced over freeze, blocked if the shock projection crosses the broker's auto-square-off threshold. Builds on Epics 1–3 (gate, risk, reconcile). *(PRD [NOTE FOR PM]: candidate to pull earlier if the option-seller milestone should precede operator-visibility polish.)*
**FRs covered:** FR-16, FR-17, FR-18 (+ freeze-slicing realized at option size on the Epic-1 model).

### Epic 6: Breadth — Kotak Neo Adapter & Multi-Account
Earn breadth only after Kite is bulletproof: the Kotak Neo adapter (multi-step auth, capability/error mapping — gated on the Day-One-critical-path spike), broker-portability proof (SM-3: same strategy, config-only switch), multi-strategy isolation (virtual positions, per-strategy P&L), and multi-account process-per-account with the thin supervisor + shared per-broker refdata cache. **Demonstrable value:** the same unchanged strategy runs on Kotak Neo; two strategies and two accounts run isolated with bounded kill-switch blast radius. Builds on all prior epics.
**FRs covered:** FR-32, FR-33 (+ Kotak realization of FR-1, FR-2, FR-21, FR-25).

---

## Epic 1: Order-Safety Substrate

Stand up the C++ project and the safety core so a strategy can place orders through the broker-neutral interface against an adversarial fake broker, with zero duplicate orders provable across the fault matrix.

### Story 1.1: C++ project scaffold with hexagonal target boundary

As a platform maintainer,
I want a CMake + Conan C++20 project with the layered target structure and CI,
So that every later story builds on an enforced architecture and green pipeline.

**Acceptance Criteria:**

**Given** a clean checkout
**When** I run the documented CMake+Conan configure/build
**Then** the project builds on gcc and clang (Debug+Release) with `-Wall -Wextra -Werror`
**And** `include/broker_exec/` + `src/{domain,ports,adapters,...}` exist as separate CMake targets where the `domain` target links no adapter/transport target (verified by a build-time check)
**And** CI runs clang-format, clang-tidy, ASan/UBSan/TSan builds, and `ctest`, all green on an empty placeholder test.

### Story 1.2: Domain value types, OrderState enum, and the Money fixed-point type

As a strategy author,
I want immutable domain types and an exact Money type,
So that prices and quantities are never represented as floating point.

**Acceptance Criteria:**

**Given** the domain library
**When** I construct `Money`, `Price`, `Quantity`, `OrderIntent`, `Order`, `Trade`, `Position`, `Instrument`
**Then** `Money`/`Price` store integer paise (`int64`) and expose `round_to_tick()`; no `double`/`float` appears in any money path (lint-enforced)
**And** `OrderState` enumerates CREATED…UNKNOWN, RECONCILED, PARTIALLY_PLACED, MANUAL_INTERVENTION_REQUIRED
**And** value types are immutable and unit-tested for equality/serialization.

### Story 1.3: Ports and the injected Clock with skew/stall detection

As a platform maintainer,
I want the core to depend only on abstract ports including an injected Clock,
So that time-dependent behavior is deterministic and adapters are swappable. (FR-23)

**Acceptance Criteria:**

**Given** the ports library
**When** the core needs time, broker access, storage, alerts, secrets, or reference data
**Then** it calls a `ClockPort`/`BrokerPort`/`StorePort`/`AlertSink`/`SecretProvider`/`RefDataPort` abstraction, never a concrete library
**And** a system `Clock` impl and a test `Clock` impl exist; direct `*_clock::now()` outside the system impl is lint-banned
**And** an induced clock skew or main-loop stall beyond threshold is detected and surfaced (drives exit-only later).

### Story 1.4: Typed error taxonomy with suggested actions

As a strategy author,
I want broker/library errors normalized to typed categories with a suggested action,
So that strategy code never parses raw broker text. (FR-25)

**Acceptance Criteria:**

**Given** the error model
**When** any failure arises
**Then** it maps to a stable category carrying a `SuggestedAction` enum (retry-safe / do-not-retry / reconcile-first / block-strategy / re-establish-session / cancel / square-off / raise-alert)
**And** errors are returned as `std::expected<T,Error>` internally and never thrown across the strategy boundary
**And** a unit test asserts representative raw errors map to the expected category+action.

### Story 1.5: Write-ahead intent log (fsync, hash-chained, replayable)

As an operator,
I want every order intent durably recorded before any send,
So that a crash never produces a duplicate or an un-enumerable order. (FR-8, NFR-1)

**Acceptance Criteria:**

**Given** an order about to be sent
**When** `dispatch()` records the intent
**Then** the record (monotonic seq + client-ref + prev-record SHA-256) is appended as one JSON line and `fsync`'d **before** the socket write
**And** on boot the log replays head-to-tail, rebuilding the in-memory client-ref index and enumerating every order that might have been sent
**And** tampering with any past record is detectable via the broken hash chain.

### Story 1.6: SQLite projection and numbered migrations

As a platform maintainer,
I want a queryable SQLite read-model rebuildable from the intent log,
So that state queries are cheap and crash-safe. (FR-7 backstop)

**Acceptance Criteria:**

**Given** the SQLite C API store (WAL, `synchronous=FULL`)
**When** the app starts
**Then** numbered migrations apply transactionally with a `schema_version` row; an unknown/newer version refuses to start, an older readable one is supported
**And** a corrupt/half-migrated projection triggers rebuild-from-intent-log rather than refuse-to-start
**And** `orders/trades/positions/funds/risk_events/audit` tables exist with `UNIQUE(client_ref)`.

### Story 1.7: Client-ref generation and idempotency

As a strategy author,
I want each order intent uniquely referenced with duplicate detection,
So that a repeated request never creates a second order. (FR-7)

**Acceptance Criteria:**

**Given** an order request
**When** a client-ref is generated
**Then** it is `<strategy>-<sig8>-<uuid>`, with deterministic child form `<parent>#<k>` reserved for slicing
**And** submitting the same intent twice (including after restart) returns the existing order via the in-memory index + `UNIQUE(client_ref)` — exactly one order
**And** a deterministic signal hash catches a duplicate strategy signal.

### Story 1.8: Order lifecycle state machine with apply-ordering and parent/child model

As an operator,
I want a single state machine owning each order including parent/child slices,
So that concurrent broker views never corrupt order state. (FR-9)

**Acceptance Criteria:**

**Given** the lifecycle FSM applied only by the main loop
**When** broker views arrive (push or reconciler snapshot)
**Then** terminal states (FILLED/REJECTED/CANCELLED) are absorbing and a stale/older-keyed view is dropped+logged
**And** a parent intent folds over child states; any child UNKNOWN ⇒ parent PARTIALLY_PLACED-with-UNKNOWN
**And** transitions are forward-progressing (applied only if the broker ordering key ≥ last applied).

### Story 1.9: dispatch() chokepoint with no-blind-retry

As an operator,
I want every broker mutation to flow through one synchronous chokepoint,
So that fsync-before-send and no-blind-retry are guaranteed structurally. (FR-8, FR-10)

**Acceptance Criteria:**

**Given** `dispatch()` on the main loop
**When** an order is sent
**Then** it performs record-intent → `fsync` → send → record result|UNKNOWN with no thread hand-off between fsync and send
**And** a timeout/failure on place/modify/cancel/square-off marks the order UNKNOWN and reconciles — never an immediate repeat
**And** no code path sends to a broker except through `dispatch()` (enforced).

### Story 1.10: UNKNOWN handling and match-key precedence

As an operator,
I want uncertain orders resolved against broker truth by a defined precedence,
So that ambiguity never resolves into a duplicate or wrong order. (FR-9, FR-10)

**Acceptance Criteria:**

**Given** an order in UNKNOWN
**When** the library resolves it
**Then** new risky entries are paused process-wide (risk-reducing exits exempt) until resolved
**And** matching uses precedence: broker order_id > short correlation token > attribute corroboration > fail-closed
**And** when no authoritative match exists, the order stays UNKNOWN with an alert — never a second fire.

### Story 1.11: Adversarial fake broker for fault injection

As a test engineer,
I want an in-test fake broker that injects the dangerous faults,
So that the safety invariants can be exercised deterministically. (FR-37)

**Acceptance Criteria:**

**Given** the fake broker fixture
**When** a test configures a fault
**Then** it can inject delayed/dropped acks, duplicate fills, out-of-order events, 429s, and ack-lost-but-placed
**And** faults are deterministic under the injected Clock
**And** it implements the same `BrokerPort` as real adapters.

### Story 1.12: Conformance kit + SIGKILL durability harness (SM-1)

As a platform maintainer,
I want a reusable conformance kit and a SIGKILL durability test,
So that zero-duplicate-orders is a proven, regression-guarded invariant. (FR-37, NFR-3)

**Acceptance Criteria:**

**Given** the conformance kit run against the fake broker
**When** the full fault matrix executes
**Then** UNKNOWN-handling, no-blind-retry, and reconciliation pass, with duplicate-order count = 0
**And** a separate harness SIGKILLs the process between fsync and send (via an injectable pre-send barrier) and asserts zero duplicates on replay+reconcile
**And** the kit runs in CI and gates every adapter.

---

## Epic 2: Live Kite Trading — Validation Gate, Risk, Reference Data & Session

Make the library trade safely on a real Zerodha Kite account: validated, risk-checked orders, staleness-gated reference data, daily session establishment, and a safe-start gate.

### Story 2.1: Typed configuration system

As an operator,
I want all behavior driven by typed config (file + env),
So that the same binary runs across dev/paper/live by configuration only. (FR-34)

**Acceptance Criteria:**

**Given** a TOML config (toml++) + env overrides
**When** the app loads
**Then** config parses into a validated typed struct; invalid config fails fast with a clear message
**And** secrets are read only from env/secret-provider, never from the TOML
**And** the same source runs dev/paper/live-small/live-prod with only config changed.

### Story 2.2: Secret provider, token encryption, and the redaction scrubber

As an operator,
I want secrets sourced safely, encrypted at rest, and scrubbed from all output,
So that no credential ever leaks to a log, error, or the ledger. (FR-35)

**Acceptance Criteria:**

**Given** a `SecretProvider` (env default) and OpenSSL AES-256-GCM token store
**When** tokens are persisted
**Then** they are encrypted at rest with a per-account key; the store/key files are mode 0600 in a 0700 dir
**And** a shared secret-shape scrubber (`domain/redaction`) redacts token-shaped strings from spdlog output, exception text, AND the intent-log/ledger persistence path
**And** a test feeding a synthetic Kite access_token through a log, an exception, and a persisted record finds zero token-shaped strings in any sink.

### Story 2.3: Kite Connect REST client (the transport)

As a platform maintainer,
I want a native C++ Kite Connect REST client behind the adapter port,
So that orders/portfolio/instruments are reachable without an official SDK.

**Acceptance Criteria:**

**Given** the Kite REST client (cpr/libcurl) implementing the transport
**When** it calls order/portfolio/instrument/margin endpoints
**Then** it handles auth headers, request signing, pagination, and rate-limit headers
**And** it maps raw HTTP/transport errors to the typed taxonomy (FR-25), scrubbed of secrets
**And** it is exercised by recorded-fixture tests (no live credentials in CI).

### Story 2.4: Kite daily session establishment and expiry detection

As an operator,
I want the daily Kite login modeled as first-class establishment,
So that the bot trades after the day's token is supplied and blocks otherwise. (FR-21)

**Acceptance Criteria:**

**Given** Kite has no headless refresh (token dies ~6am)
**When** the bot cold-starts
**Then** `establish_session` exchanges an operator-supplied request_token for the access_token (stored encrypted) before the safe-start gate
**And** a dead daily token blocks trading (never a silent "refresh") with a typed alert
**And** the `headless session refresh` capability for Kite reads unsupported.

### Story 2.5: Capability model and early rejection

As a strategy author,
I want unsupported capabilities rejected at load/request time,
So that I never fail mid-trade on a missing broker feature. (FR-2)

**Acceptance Criteria:**

**Given** the per-broker capability set
**When** a strategy requires a capability the broker lacks
**Then** it is rejected early with a typed error (MVP posture = reject-only; substitution deferred)
**And** an unverified `unknown` capability is treated as unsupported
**And** the capability set is queryable by strategy code.

### Story 2.6: Instrument-master lifecycle

As an operator,
I want the instrument master refreshed and staleness-gated daily,
So that orders never use stale lot/tick/freeze/expiry or a wrong token. (FR-3)

**Acceptance Criteria:**

**Given** the per-broker instrument master
**When** a new trading day begins
**Then** it is downloaded, cached (date-versioned), and a symbol resolves to current token + lot/tick/freeze/expiry
**And** a stale or failed-to-download master blocks trading at safe-start with an alert
**And** an expired/unknown instrument is rejected at the gate; a new strike is tradable only after refresh.

### Story 2.7: Trading-calendar lifecycle

As an operator,
I want the holiday/special-session calendar sourced and staleness-gated,
So that the bot never trades on a holiday or misses a special session. (FR-4)

**Acceptance Criteria:**

**Given** a configured calendar source
**When** the bot starts
**Then** the calendar is refreshed/cached; a stale/missing calendar blocks trading at safe-start
**And** entry cut-off and square-off windows are enforced
**And** new holidays take effect only after a refresh.

### Story 2.8: Pre-submission validation gate

As an operator,
I want a non-bypassable validation gate before any broker call,
So that no invalid or unsafe order reaches the broker. (FR-6)

**Acceptance Criteria:**

**Given** the gate
**When** an order is submitted
**Then** it checks lot/freeze (over-freeze ⇒ slice), tick, product, exchange, funds (fail-closed-on-stale), risk, time-window, duplicate, hedge, kill-switch, and UNKNOWN-pause — naming the failed check
**And** strategy code has no path to bypass it
**And** an entry under an active UNKNOWN-pause is blocked while a risk-reducing exit passes.

### Story 2.9: Freeze-quantity slicer

As a strategy author,
I want over-freeze orders sliced into child orders automatically,
So that large orders execute safely without duplicates. (FR-6)

**Acceptance Criteria:**

**Given** an order above the instrument's freeze limit
**When** it is dispatched in slice-mode (default)
**Then** it fans out to children (qty ≤ freeze, ≥ lot, lot-aligned remainder) each with deterministic `<parent>#<k>` client-ref
**And** re-slicing on replay is bit-identical and `UNIQUE(client_ref)` dedupes a placed child
**And** any child UNKNOWN engages the parent UNKNOWN-pause.

### Story 2.10: Four-level risk engine

As an operator,
I want account/strategy/instrument/order risk enforced on every order,
So that configured limits demonstrably block offending orders. (FR-15)

**Acceptance Criteria:**

**Given** configured limits
**When** an order would violate one (daily loss, max lots, max margin, max open positions, market-order block, slippage, illiquid/stale-data, time windows, etc.)
**Then** it is blocked with the violated rule named
**And** each level (account/strategy/instrument/order) is independently testable
**And** a strategy can be stopped without stopping others.

### Story 2.11: Funds/margin view with cadence and fail-closed gate

As an operator,
I want margin checked against a fresh funds view,
So that an over-leveraged order is never let through on stale data. (FR-13)

**Acceptance Criteria:**

**Given** a funds view with a `fetched_at` stamp
**When** a margin-sensitive order is gated
**Then** the view is refreshed if older than the configured cadence; on unrefreshable-stale the check fails closed (block + alert)
**And** the scheduler refreshes funds on cadence and after fills
**And** the check never runs against a view older than the cadence.

### Story 2.12: Rate limiter with reserved exit lane

As an operator,
I want broker requests throttled with guaranteed exit capability,
So that throttling never blocks a square-off. (FR-24)

**Acceptance Criteria:**

**Given** the token bucket + admission queue
**When** the bucket is drained by entries
**Then** square-off/cancel still dispatch from a reserved allocation entries cannot consume
**And** a per-send timeout bounds head-of-line blocking (timeout ⇒ UNKNOWN, free the slot)
**And** under induced pressure the library slows+alerts instead of letting the broker reject.

### Story 2.13: Safe-start cold-boot gate

As an operator,
I want trading blocked until the world is verified fresh,
So that a restart never trades into a stale or unauthenticated state. (FR-22)

**Acceptance Criteria:**

**Given** a fresh boot
**When** the bot starts
**Then** it verifies session establishment, reconciliation completeness, clock sanity, config integrity, fresh instrument master + calendar, egress-IP allowlist, and crypto-key presence — trading only after all pass
**And** any failed check blocks trading loudly (fail closed)
**And** a forced egress-IP mismatch or stale master blocks the start.

### Story 2.14: Kite adapter conformance and live-min-qty smoke

As a platform maintainer,
I want the Kite adapter certified and smoke-tested live with minimum quantity,
So that real-money Kite trading is proven before scale.

**Acceptance Criteria:**

**Given** the Kite adapter
**When** the conformance kit runs against it (recorded fixtures)
**Then** it passes green and every Kite `unknown` capability/error is resolved with a committed fixture
**And** a documented live-min-qty smoke places, reconciles, and squares off one lot with zero duplicates
**And** paper and dry-run modes run the same path without live execution.

---

## Epic 3: Resilience — Reconciliation, Recovery, Safe Degradation & Kill Switches

Make the bot survive failure: reconcile against broker truth, recover after a crash, degrade to a defined safe posture, and let the operator kill it.

### Story 3.1: Continuous reconciliation (fetch-off-loop, apply-on-loop)

As an operator,
I want state continuously reconciled against the broker on every trigger,
So that local state always converges to broker truth. (FR-11)

**Acceptance Criteria:**

**Given** the reconciler on the scheduler thread
**When** a trigger fires (startup/login/reconnect/post-unknown/around square-off/periodic/WS-disconnect/manual-intervention-suspected)
**Then** it performs broker reads only and enqueues an immutable result; the main loop applies all diffs (sole writer)
**And** cadence is adaptive — tight (~1–2s) while SENT/UNKNOWN or a position is open, loose (~15–30s) when flat
**And** a mismatch raises an alert and optionally blocks new orders.

### Story 3.2: Manual-intervention detection

As an operator,
I want manual broker-app changes detected,
So that the bot never sends a duplicate exit. (FR-12)

**Acceptance Criteria:**

**Given** a position the bot believes open
**When** the user closes it manually in the broker app
**Then** reconciliation detects the change and updates internal state
**And** no duplicate exit order is sent
**And** the event is alerted and audited.

### Story 3.3: Corporate-action awareness

As an operator,
I want splits/bonus/symbol changes recognized as corporate actions,
So that they are not misread as a mismatch. (FR-5)

**Acceptance Criteria:**

**Given** a configured corporate-action source
**When** a held position's qty/price/symbol changes via a corporate action
**Then** it is classified as such (position re-based, token re-resolved) without a false mismatch alert or a corrective order
**And** absence of a configured source is surfaced, not silently assumed.

### Story 3.4: Crash recovery (reconcile-before-resume)

As an operator,
I want safe recovery after a crash,
So that the bot never trades immediately on restart. (FR-14)

**Acceptance Criteria:**

**Given** an in-flight order and a process kill
**When** the bot restarts
**Then** it loads state, checks session, fetches broker order book/trades/positions, resolves unknowns, and resumes only when safe
**And** zero duplicate orders result
**And** a double-fault (UNKNOWN + broker unreachable) lands in MANUAL_INTERVENTION_REQUIRED with escalation, no auto-square-off.

### Story 3.5: Market-data tick stream with tradability states

As a strategy author,
I want market data exposed with explicit tradability,
So that I can ask "is this price tradable?" not just "what is the LTP?". (FR-19)

**Acceptance Criteria:**

**Given** the IXWebSocket tick stream
**When** ticks flow or stop
**Then** data is exposed as Live/Stale/Disconnected/Delayed/Unknown with auto-reconnect (auth-aware), de-dup, and configured REST fallback
**And** price-sensitive entries are blocked while data is not Live
**And** a reconnect that fails on auth routes to session validation, not an endless retry loop.

### Story 3.6: Liveness and process self-health watchdogs

As an operator,
I want connected-but-mute feeds and resource exhaustion detected,
So that the bot degrades before it silently fails. (FR-20)

**Acceptance Criteria:**

**Given** a still-connected WebSocket delivering no ticks past threshold
**When** the watchdog runs
**Then** it forces reconnect-or-degrade
**And** disk (WAL/audit growth)/memory/handle limits trigger safe degradation + alert before corrupting logging
**And** clock-skew/stall feeds the same degrade-to-exit-only path.

### Story 3.7: Degradation-posture coordinator

As an operator,
I want one authority mapping each failure to a defined posture,
So that degradation is coherent, never contradictory. (FR-26)

**Acceptance Criteria:**

**Given** detector signals (broker-down, stale-data, UNKNOWN, mismatch, risk-breach, session-expiry, clock-stall, mute-feed)
**When** any fires
**Then** the coordinator selects a posture from the existing vocabulary (block-entries/exit-only/soft-kill/panic) and the gate enforces it as the single chokepoint
**And** detectors feed the coordinator; they do not each decide a posture
**And** each failure scenario produces its specified behavior without duplicate orders or silent failure.

### Story 3.8: Kill switches with operator control-plane

As an operator,
I want to trip any kill switch on a running process,
So that I have a reliable last line of defense. (FR-31)

**Acceptance Criteria:**

**Given** a running per-account engine
**When** I issue a kill via the CLI
**Then** an authenticated control command enqueues onto the main-loop queue and flips the in-process flag (soft/strategy/broker/account keep the process alive: block entries, allow exits; panic cancels+squares-off+blocks)
**And** a kill arriving mid-dispatch takes effect within one bounded broker-call timeout, and panic exits proceed even while an order is UNKNOWN
**And** an accepted kill is persisted so a crash replays as still-killed.

---

## Epic 4: Operator Visibility — Observability, Alerts, Ledger, Modes & Surface

Let the operator stop watching: full provenance, push alerts, a tamper-evident ledger + EOD report, modes, and the CLI/health surface.

### Story 4.1: Structured logging, provenance & audit

As an operator,
I want full per-order provenance in structured logs and an audit trail,
So that I can reconstruct any trade's decision path. (FR-27)

**Acceptance Criteria:**

**Given** spdlog JSON logging with the redaction formatter
**When** an order flows through the system
**Then** strategy/broker/account/all-timestamps/response/status-changes/risk-results/reconcile-result/P&L/error/override are recorded
**And** a complete audit record reconstructs any executed order's decision path
**And** the emitted event schema is versioned (field renames are breaking changes).

### Story 4.2: Daily/error/reconciliation reports

As an operator,
I want end-of-session reports,
So that I get a verdict, not just a stream of events. (FR-27)

**Acceptance Criteria:**

**Given** a trading session
**When** it ends
**Then** daily, error, and reconciliation reports are generated
**And** the reconciliation report states intended/sent/confirmed/reconciled vs broker
**And** reports are reproducible from the audit data.

### Story 4.3: Alerting with dead-man's-switch

As an operator,
I want push alerts that themselves cannot fail silently,
So that a silent breach reaches my phone. (FR-28)

**Acceptance Criteria:**

**Given** one `AlertSink` with Telegram + generic webhook
**When** any named alert condition fires
**Then** it is delivered, and a `send_test_alert()` exists per channel
**And** the alerter emits a periodic heartbeat whose absence an external watcher alarms on
**And** killing the alerter triggers the absence alarm in test.

### Story 4.4: Tamper-evident ledger, EOD signed report & heartbeat

As an operator,
I want a hash-chained signed ledger and a position heartbeat,
So that I have a system-of-record foundation and a "still safe" signal. (FR-29)

**Acceptance Criteria:**

**Given** the ledger built on the intent log (SHA-256 chain + Ed25519 per-account signature via libsodium)
**When** an entry is modified by a non-key-holder or corrupted
**Then** the chain breaks and is detectable; the public key is written to the data dir + EOD report
**And** a periodic position/exposure heartbeat reaches the operator
**And** key-mismatch is a fail-closed safe-start condition; the ledger is never presented as legal proof.

### Story 4.5: Trading modes

As an operator,
I want selectable run modes,
So that I can develop and operate safely. (FR-30)

**Acceptance Criteria:**

**Given** a configured mode
**When** the bot runs
**Then** live/paper/dry-run/replay/monitor-only/exit-only/emergency each exhibit their defined behavior
**And** replay binds the Clock to the recorded timeline (deterministic decision-path)
**And** dry-run validates but never executes; emergency allows only cancel/square-off.

### Story 4.6: CLI and localhost health endpoint

As an operator,
I want a thin CLI and a health endpoint,
So that I can operate and monitor the engine out-of-band. (FR-36)

**Acceptance Criteria:**

**Given** the CLI11 CLI and the cpp-httplib endpoint (localhost/UDS, 0600)
**When** I run `status/reconcile/replay-intent-log/safe-start-check/send-test-alert/verify-ip/kill` or GET `/healthz`,`/ready`
**Then** they are thin shells over the public API (read-only except `kill`)
**And** the endpoint serves an immutable `HealthSnapshot` (session state, heartbeat/tick age, in-flight count, clock sanity, replay-clean) published by the main loop
**And** the supervisor uses snapshot age to detect a wedged process.

---

## Epic 5: Option-Selling Safety — Hedge-First, Baskets, Slicing & Margin Shock

The primary JTBD: run a hedged option-selling basket safely as one logical trade.

### Story 5.1: Hedge-first execution with naked-sell prevention

As an option seller,
I want the hedge bought before the short is sold,
So that I am never momentarily naked. (FR-16)

**Acceptance Criteria:**

**Given** a hedged short configured hedge-first
**When** the basket executes
**Then** the hedge is placed first and the short is sent only on hedge confirmation
**And** if the hedge fails, the short is never sent
**And** if the short succeeds but the hedge later fails, an immediate alert fires and the configured emergency action runs.

### Story 5.2: Basket / multi-leg execution

As an option seller,
I want multi-leg trades executed as one logical unit,
So that legs don't get orphaned. (FR-17)

**Acceptance Criteria:**

**Given** a multi-leg basket
**When** it executes
**Then** leg dependency is honored (dependent legs not sent if prerequisites fail) and partial execution is detected
**And** a forced one-leg-fail exits/cancels executed legs per the configured policy
**And** the basket is tracked as one logical trade unless explicitly configured otherwise.

### Story 5.3: Freeze-slicing at option size

As an option seller,
I want large option orders sliced safely within a basket,
So that big positions execute without duplicates. (FR-6/FR-17 composition)

**Acceptance Criteria:**

**Given** a basket leg above the freeze limit
**When** it executes
**Then** the leg slices into deterministic-ref children that compose with leg dependency and rate-limit/exit-priority
**And** a mid-slice SIGKILL leaves no orphan/duplicate child on recovery
**And** any child UNKNOWN engages the parent/basket UNKNOWN-pause.

### Story 5.4: Margin/SPAN shock simulation (capability-gated)

As an option seller,
I want pre-trade margin modeled under a volatility shock,
So that I don't open a basket that a vol spike would force-liquidate. (FR-18)

**Acceptance Criteria:**

**Given** a broker with a basket/SPAN margin source
**When** a basket is submitted
**Then** margin now and under the configured shock is modeled against the broker's RMS auto-square-off threshold and a crossing basket is flagged/blocked pre-submission
**And** where no basket/SPAN source exists, the shock sim reports "unavailable on this broker" and net-short basket margin fails closed (never summed-legs)
**And** the result is audited.

---

## Epic 6: Breadth — Kotak Neo Adapter & Multi-Account

Earn breadth after Kite is bulletproof: the Kotak adapter, broker portability, and isolated multi-strategy/multi-account.

### Story 6.1: Kotak Neo REST+WebSocket client and multi-step auth

As a platform maintainer,
I want a native Kotak Neo transport with its multi-step auth,
So that Kotak is reachable without an official C++ SDK.

**Acceptance Criteria:**

**Given** the Kotak Neo REST+WS client (cpr/IXWebSocket)
**When** it logs in
**Then** it performs the multi-step flow (consumer-key → session/view-token → MPIN+TOTP), returning normalized HEALTHY/NEEDS_REAUTH/FAILED
**And** the day-one-critical-path `unknown`s (margin/funds API, auth flow) are resolved by an upfront spike with committed fixtures
**And** raw errors map to the typed taxonomy, scrubbed.

### Story 6.2: Kotak Neo adapter certification

As a platform maintainer,
I want the Kotak adapter certified,
So that it is held to the same safety bar as Kite.

**Acceptance Criteria:**

**Given** the Kotak adapter
**When** the conformance kit runs against it
**Then** it passes green and every Kotak `unknown` capability/error is resolved with a committed fixture
**And** capability differences (e.g., basket margin, order-update WS) are reflected in the capability model and degrade safely
**And** a live-min-qty smoke passes.

### Story 6.3: Broker-portability proof

As a strategy author,
I want to switch brokers by config,
So that my strategy is not locked to one broker. (SM-3)

**Acceptance Criteria:**

**Given** an unchanged option-selling strategy on Kite
**When** I flip the broker config to Kotak Neo
**Then** it runs without code changes
**And** a required-but-unsupported capability is rejected at load, not mid-trade
**And** a code scan finds zero broker-specific identifiers in the strategy.

### Story 6.4: Multi-strategy isolation

As an operator,
I want strategies isolated within an account,
So that they don't interfere or close each other's positions. (FR-32)

**Acceptance Criteria:**

**Given** two strategies trading the same instrument
**When** both run
**Then** each keeps separate virtual positions, tags, and P&L; neither squares off the other's position unless netting is enabled
**And** a global account limit still blocks the combined exposure
**And** one strategy can be stopped without stopping the other.

### Story 6.5: Multi-account process-per-account with supervisor

As an operator,
I want each account in its own process under a supervisor,
So that a failure in one account cannot touch another. (FR-33)

**Acceptance Criteria:**

**Given** N accounts
**When** they run under the systemd template + supervisor
**Then** each owns its intent log/store/token store/kill switch; instrument master + calendar are downloaded once per (broker, date) into a shared cache under a cross-process lock
**And** SIGKILL of one account's process leaves others trading and the supervisor restarts the dead one from its intent log
**And** the supervisor honors the exit-code contract (CRASH ⇒ restart-with-backoff; FAIL_CLOSED_NEEDS_HUMAN ⇒ no restart + absence alarm).
