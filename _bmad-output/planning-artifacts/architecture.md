---
stepsCompleted: [1, 2, 3, 4, 5, 6, 7, 8]
lastStep: 8
status: 'complete'
completedAt: '2026-06-21'
inputDocuments:
  - ../specs/spec-broker-neutral-execution/SPEC.md
  - ../specs/spec-broker-neutral-execution/technical-architecture.md
  - ../specs/spec-broker-neutral-execution/order-lifecycle.md
  - ../specs/spec-broker-neutral-execution/risk-controls.md
  - ../specs/spec-broker-neutral-execution/resilience-and-reconciliation.md
  - ../specs/spec-broker-neutral-execution/broker-capabilities.md
  - ../specs/spec-broker-neutral-execution/error-taxonomy.md
  - ../specs/spec-broker-neutral-execution/operational-modes.md
  - ../specs/spec-broker-neutral-execution/scope-and-phasing.md
  - ../specs/spec-broker-neutral-execution/architecture-diagrams.md
  - ../specs/spec-broker-neutral-execution/glossary.md
  - prds/prd-fitbro-2026-06-17/prd.md
workflowType: 'architecture'
project_name: 'fitbro (Broker-Neutral Trading Execution Library)'
user_name: 'Abhijeet'
date: '2026-06-17'
---

# Architecture Decision Document

_This document builds collaboratively through step-by-step discovery. Sections are appended as we work through each architectural decision together._

_Strong head start: the spec's `technical-architecture.md` already resolves 8 foundational decisions (D1–D8) via a prior architecture debate. This workflow confirms, deepens, and fills the gaps around them rather than starting from zero._

## Project Context Analysis

### Requirements Overview

**Functional Requirements:** 37 FRs across 14 features (FR-1…FR-37). Architecturally they cluster into:
- **Broker abstraction** (FR-1, FR-2, FR-25) — a normalized interface + capability model + error taxonomy over per-broker adapters.
- **Order safety pipeline** (FR-6…FR-10) — validation gate → idempotency/client-ref → write-ahead intent log → lifecycle state machine (incl. UNKNOWN) → no-blind-retry.
- **State integrity** (FR-11…FR-14, FR-5) — reconciliation-first, manual-intervention + corporate-action detection, funds/margin cadence, crash recovery.
- **Risk** (FR-15…FR-18) — four-level engine + option-selling + basket + margin-shock simulation.
- **Reference data** (FR-3, FR-4) — instrument master + trading calendar lifecycle with staleness gating.
- **Reliability substrate** (FR-19…FR-24, FR-26) — market-data states, watchdogs, session/safe-start, clock, rate limits, safe degradation.
- **Operability** (FR-27…FR-31, FR-36) — observability/ledger/alerting, modes, kill switches, CLI + health endpoint.
- **Isolation & config** (FR-32…FR-35) — multi-strategy/account, config-driven, secrets.
- **Test seam** (FR-37) — adversarial fake broker + adapter conformance kit.

**Non-Functional Requirements (architecture drivers):**
- Durability: intent fsync'd before socket write; no order dispatched whose intent is not first recorded.
- Correctness over speed: synchronous single-threaded decision core; async confined to a single adapter if ever needed.
- Zero-duplicate invariant; uncertain state always resolves toward broker truth.
- Persisted-state + event-schema versioning (fail visibly on unknown version).
- Perf budgets (starting targets): gate+fsync < ~50 ms/order; full reconciliation < ~5 s; tick→tradable-state < ~100 ms.
- Security: secrets from env/secrets-manager, encrypted at rest, never logged; health endpoint localhost-only; operator surface read-only except `kill`.
- Footprint: single modest VPS; one process per account; no external DB daemon at MVP.

### Scale & Complexity
- Primary domain: backend Python library / execution engine (no UI).
- Complexity level: High (real-money, safety-critical, stateful, concurrent I/O, multi-broker).
- Estimated architectural components: ~12–15 modules (see Step 6 source tree).

### Technical Constraints & Dependencies (pre-resolved — `technical-architecture.md` D1–D8)
- Python 3.11+; PEP 621 wheel; broker SDKs as optional extras; no namespace packages.
- Persistence: fsync'd append-only write-ahead intent log (durable truth) + SQLite (WAL, `synchronous=FULL`) projection. No Postgres at single-VPS baseline.
- Concurrency: synchronous safety core + threads only at I/O edges; no asyncio in core; injected clock.
- Multi-account: one OS process per account + thin supervisor.
- Brokers: direct `pykiteconnect` / Kotak Neo SDK adapters. No OpenAlgo / no AGPL dependency.
- Static-IP: enforce operator-declared egress IP at safe-start; document the regulatory obligation.
- Operator surface: library + thin CLI + localhost health endpoint.

### Cross-Cutting Concerns Identified
Reconciliation · observability/audit/tamper-evident ledger · kill switches · clock/time abstraction · reference-data freshness · error normalization · rate limiting · secret management · multi-account isolation · safe degradation.

## Starter Template Evaluation

### Primary Technology Domain
Backend Python library / execution engine (no UI). There is no web-style "starter"; the foundation decision is the build/tooling stack that realizes D4.

### Starter Options Considered
- **uv + Hatchling (selected)** — uv for env/deps/lockfile, Hatchling build backend. Fast, PEP 621-native, mature, handles optional-dependency extras and custom file selection (bundling reference-data schema files) cleanly.
- **uv + uv_build** — leaner, zero-config, tightly integrated; viable alternative. Chosen-against only because Hatchling's file-selection maturity is marginally safer ("boring beats clever").
- **Poetry / poetry-core** — fine but a heavier, less standard lockfile/resolver story than uv in 2026; not chosen.
- **setuptools** — works, but no first-class lockfile/env management; not chosen.

### Selected Foundation: uv + Hatchling
**Rationale:** matches D4 (PEP 621, src-layout, hashed lockfile, extras), gives a provable "prod runs what CI tested" lockfile, no C-extension needs, and the lowest-ceremony modern toolchain.

**Initialization Command (first implementation story):**
```bash
uv init --package --lib --python 3.11 broker-exec   # name TBD; creates pyproject.toml + src/ layout
# build-system: hatchling ; manage deps + lockfile with uv
```

**Architectural Decisions Provided by this Foundation:**
- **Language & Runtime:** Python 3.11 floor (CI matrix 3.11 + 3.12); PEP 621 `pyproject.toml`.
- **Build Tooling:** Hatchling backend; `uv` for env/deps + hashed cross-platform lockfile; broker SDKs as optional extras (`broker-exec[kite,neo]`); no namespace packages.
- **Testing:** pytest (+ pytest-cov); the adversarial fake broker (FR-37) as a fixture library; deterministic time via the injected `Clock` (D2) with `time-machine` as a fallback guard; SIGKILL-based durability tests for the intent log (FR-8).
- **Lint / Format / Types:** Ruff (lint + format); mypy strict (typed public surface is part of the contract).
- **Code Organization:** src/ layout; domain core has zero broker-SDK imports (SDKs live only inside adapters).
- **Development Experience:** `uv run` task entry; localhost health endpoint + thin CLI; CI = GitHub Actions matrix (3.11/3.12) running Ruff + mypy + pytest with the conformance kit.

**Note:** Project initialization with this toolchain should be the first implementation story.

## Core Architectural Decisions

### Decision Priority Analysis
- **Critical (block implementation):** persistence format & schema versioning (DA-1…DA-3), hexagonal ports/adapters (IA-1, IA-2), concurrency model (IA-3), order-lifecycle/idempotency mechanism (IA-4), secrets & ledger crypto (SE-1…SE-3).
- **Important (shape architecture):** error model (IA-5), retry policy (IA-6), reference-data cache (DA-4), config (DA-5), logging/alert transport (ID-2, ID-3), supervisor (ID-1).
- **Deferred (post-MVP):** secrets-manager adapter beyond env; Parquet vs SQLite for reference-data history; broker fallback routing.

### Data Architecture
- **DA-1 — Durable truth = write-ahead intent log.** Append-only newline-delimited JSON, one record/line (monotonic seq + client-ref + prev-record SHA-256 → hash chain); `fsync` per record **before** the broker socket write. JSON over msgpack for 3am inspectability. *(D1, CAP-26, CAP-30)*
- **DA-2 — Read model = SQLite (stdlib `sqlite3`), WAL + `synchronous=FULL`.** Tables: orders/lifecycle, trades, positions, holdings, funds, risk_events, audit. Idempotency = `UNIQUE(client_ref)`. Rebuildable from the intent log. No ORM. *(D1)*
- **DA-3 — Schema versioning + migrations.** `schema_version` table; numbered idempotent SQL migrations applied on startup; **refuse to start on an unknown/newer version** (fail visibly). Event schema version-stamped too.
- **DA-4 — Reference-data cache.** Instrument master & calendar cached as date-versioned local data (SQLite table for query + raw broker dump retained for audit); staleness = cache-date vs trading-date check at safe-start. *(CAP-33, CAP-21)*
- **DA-5 — Config = pydantic-settings v2 (2.14.2).** Layered defaults → TOML file → env; typed/validated; **secrets only via env/secret-provider, never in the TOML**. *(CAP-22)*

### Authentication & Security
- **SE-1 — Secrets via a `SecretProvider` port** (env default; optional secrets-manager adapter later); structlog redaction processor guarantees no secret in logs. *(CAP-23)*
- **SE-2 — Token encryption at rest = `cryptography` 49.0.0 Fernet (AES)**, key from the secret provider; per-account encrypted token store.
- **SE-3 — Ledger integrity = SHA-256 hash chain (stdlib `hashlib`) + periodic Ed25519 chain-head signature (`cryptography`).** Never represented as legal proof. *(CAP-30)*
- **SE-4 — Health endpoint bound to 127.0.0.1 only; operator surface read-only except `kill`.** *(D8)*

### Internal Architecture & Communication (the library's "API")
- **IA-1 — Hexagonal (ports & adapters).** Domain core defines ports — `BrokerPort`, `MarketDataPort`, `ClockPort`, `AlertSink`, `SecretProvider`, `Store` — implemented by adapters. **Domain core imports zero broker SDKs.** *(CAP-1, D7)*
- **IA-2 — Broker adapter contract.** Typed `BrokerAdapter` (place/modify/cancel/squareoff/orderbook/tradebook/positions/holdings/funds/instruments) returning normalized domain objects; declares a `Capabilities` set; maps raw errors to the typed taxonomy. Certified by the conformance kit. *(CAP-2, CAP-13, FR-37)*
- **IA-3 — Concurrency: one synchronous main loop + threaded I/O edges.** Main loop owns the decision path + SQLite writer + intent log; threads handle market-data ingest (SDK callbacks → `queue.Queue`), an egress/rate-limit worker (token bucket under a `Lock`, **exit-priority** queue), alert sender, and a scheduler (reconcile/heartbeat). Cross-thread handoff via thread-safe queues; the core shares no mutable state across threads. *(D2, CAP-12)*
- **IA-4 — Order lifecycle = explicit `OrderState` enum + transition table.** Write-ahead protocol: `record intent (fsync) → send → record result | UNKNOWN → reconcile`. Idempotency key = client-ref (UUID + deterministic strategy/signal hash to catch duplicate signals). *(CAP-4, CAP-5, CAP-26)*
- **IA-5 — Error model = typed exception hierarchy** mapped to taxonomy categories, each carrying a `SuggestedAction` enum; strategy sees typed errors only. *(CAP-13)*
- **IA-6 — Retry policy = `tenacity` for SAFE reads only** (reference-data download, quote/orderbook fetch) with bounded backoff. **Dangerous ops are never retry-wrapped** — they take the UNKNOWN→reconcile path; an architecture/lint rule enforces this. *(CAP-14)*

### Frontend Architecture
N/A — headless library. Operator surface = thin CLI (stdlib `argparse`) + localhost health endpoint (stdlib `http.server`). *(D8)*

### Infrastructure & Deployment
- **ID-1 — Process-per-account + systemd template units** (`broker-exec@<account>.service`); each process owns its data dir (intent log + SQLite + token store). *(D3)*
- **ID-2 — Logging = structlog 26.1.0 → JSON** to stdout/file; event schema is a versioned contract; secret-redaction processor. *(CAP-15)*
- **ID-3 — Outbound HTTP (Telegram/webhook) = `requests`, not httpx.** `requests` is already a transitive dep of the Kite SDK (zero marginal dep) and httpx's stable line is in maintenance flux (issues/discussions closed Feb 2026, nothing stable past 0.28.1). Brokers' own SDKs handle their REST/WS. *(D5)*
- **ID-4 — CI = GitHub Actions matrix 3.11/3.12** → Ruff + mypy strict + pytest (incl. fake-broker conformance kit + SIGKILL durability test). *(D4)*
- **ID-5 — Clock = `Clock` port** (system impl + manual test impl); `time-machine` only as a fallback test guard. *(D2, CAP-27)*

### Decision Impact Analysis
**Implementation sequence:** toolchain init → domain models + ports → Store (intent log + SQLite + migrations) → order FSM + idempotency → one BrokerAdapter (Kite) + conformance kit → risk gate → reconciler → market data → safe-start/session → observability/alerts → CLI/health → Kotak adapter.
**Cross-component dependencies:** every adapter depends on the ports + taxonomy; the FSM depends on the Store; safe-start depends on reference data + session + reconciler; the conformance kit gates every adapter.

## Implementation Patterns & Consistency Rules

### Critical Conflict Points Identified
~10 areas where independent agents could diverge: identifier/enum naming, money/time representation, domain-object style, SQLite schema conventions, intent-log/event JSON shape, cross-thread communication, clock/uuid access, error/retry handling, logging schema, and the domain↔adapter import boundary.

### Naming Patterns
- **Python code:** modules/packages `snake_case`; classes `PascalCase`; functions/vars `snake_case`; constants `UPPER_SNAKE`. Ports suffixed `…Port`; broker adapters `<Broker>Adapter` (e.g., `KiteAdapter`); enum values `UPPER_SNAKE` (`OrderState.PARTIALLY_FILLED`).
- **SQLite:** tables `snake_case` plural (`orders`, `trades`, `positions`, `risk_events`); columns `snake_case`; client-reference column always `client_ref`; names `pk_<table>` / `idx_<table>_<cols>`.
- **Events / log keys:** dotted lowercase (`order.placed`, `order.unknown`, `reconcile.mismatch`, `killswitch.activated`); payload keys `snake_case`.

### Structure Patterns
- **src/ layout** package `broker_exec` (Step-6 tree). **Domain core imports nothing from `adapters/` or any broker SDK** (enforced by an import-linter contract).
- **Tests** under `tests/` mirroring the package; files `test_*.py`. The adapter **conformance kit ships as an importable module** (`broker_exec.testing`) so adapter authors run the same suite.

### Format Patterns
- **Money & prices = `Decimal` only — never `float`.** Quantities are `int` (whole lots × lot size). A single `round_to_tick()` does all price rounding.
- **Time = UTC, always via the injected `Clock`.** Direct `datetime.now()`/`time.time()` is banned (lint). SQLite stores ISO-8601 UTC; the intent log also stores epoch-nanoseconds for ordering.
- **Domain objects = frozen dataclasses.** `pydantic` only at boundaries (config + broker-payload parsing), never in the hot path.
- **JSON (intent log + events) = `snake_case` keys; `Decimal` serialized as string; `None` for null;** every record carries `schema_version`.
- **Client-ref format:** `<strategy_id>-<signal_hash8>-<uuid4hex>`.

### Communication / Concurrency Patterns
- **All cross-thread handoff via `queue.Queue`; no shared mutable state.** The synchronous main loop is the **only** writer to the Store and the intent log.
- **One `dispatch()` chokepoint** for every order send: `record intent → fsync → send → record result|UNKNOWN`. No code path sends to a broker except through it.
- **UUID/ID generation via an injected `IdGenerator` port** for deterministic tests.
- Every order-state transition **emits one structured event** with a versioned schema.

### Process Patterns
- **Error handling:** domain raises typed taxonomy exceptions; adapters translate raw SDK errors; **no bare `except`, never swallow.** A failed dangerous op → `UNKNOWN` + reconcile, **never retry**.
- **Retry:** `tenacity` applied **only** to functions marked `@safe_read`; wrapping a dangerous op is a review-blocking violation.
- **Logging levels:** INFO=lifecycle; WARNING=degradation/staleness; ERROR=failure; CRITICAL=kill-switch/safety. Secret-redaction processor on every logger.

### Enforcement Guidelines
- **All agents MUST:** use `Decimal` for money, the `Clock` for time, `dispatch()` for sends, typed taxonomy errors, and keep `domain/` free of adapter/SDK imports.
- **Enforced by:** Ruff (banned-API for `datetime.now`/`time.time`/`float`-on-money), mypy strict, an **import-linter** contract (`domain` ⇏ `adapters`/SDKs), and the conformance kit in CI.
- **Anti-patterns (review-blocking):** retrying a dangerous op; `float` for price; `datetime.now()`; a broker SDK import inside `domain/`; sending an order outside `dispatch()`; a bare `except`.

## Project Structure & Boundaries

### Complete Project Directory Structure

```
broker-exec/                          # repo root (package name TBD)
├── pyproject.toml                    # PEP 621, hatchling, deps + extras [kite,neo]
├── uv.lock                           # hashed lockfile
├── .python-version                  # 3.11
├── README.md  LICENSE  .gitignore
├── ruff.toml                         # lint+format (banned-API rules)
├── .importlinter                     # domain ⇏ adapters/SDK contract
├── mypy.ini                          # strict
├── .github/workflows/ci.yml          # matrix 3.11/3.12: ruff+mypy+pytest+conformance
├── deploy/systemd/broker-exec@.service   # process-per-account template unit (D3)
├── config/example.toml               # non-secret config sample (secrets via env)
├── src/broker_exec/
│   ├── domain/                       # PURE core — no SDK, no adapter imports
│   │   ├── models.py                 # frozen dataclasses: OrderIntent, Order, Trade, Position, Instrument, Money
│   │   ├── enums.py                  # OrderState, SuggestedAction, MarketDataState, KillSwitchType, TradingMode
│   │   ├── lifecycle.py              # OrderState transition table + FSM        (FR-9)
│   │   ├── errors.py                 # typed exception hierarchy + taxonomy      (FR-25)
│   │   ├── ids.py                    # client-ref format, IdGenerator protocol   (FR-7)
│   │   └── money.py                  # Decimal helpers, round_to_tick
│   ├── ports/                        # Protocols (interfaces) the core depends on
│   │   ├── broker.py                 # BrokerPort, Capabilities                  (FR-1, FR-2)
│   │   ├── marketdata.py  clock.py  store.py  alert.py  secrets.py  refdata.py
│   ├── adapters/                     # the ONLY place SDKs are imported
│   │   ├── brokers/kite/             # KiteAdapter (pykiteconnect)               (FR-1, FR-25)
│   │   ├── brokers/kotak/            # KotakNeoAdapter
│   │   ├── alerts/telegram.py  alerts/webhook.py                                (FR-28)
│   │   ├── secrets/env.py            # SecretProvider (env)                      (FR-35)
│   │   └── clock/system.py
│   ├── store/
│   │   ├── intent_log.py             # append-only, fsync-before-send, hash chain (FR-8)
│   │   ├── sqlite_store.py           # WAL projection, UNIQUE(client_ref)         (FR-7)
│   │   ├── migrations/               # numbered .sql + schema_version            (NFR DA-3)
│   │   └── ledger.py                 # hash-chain + Ed25519 signing              (FR-29)
│   ├── runtime/
│   │   ├── mainloop.py               # the synchronous engine; sole Store writer  (IA-3)
│   │   ├── dispatcher.py             # dispatch() chokepoint: record→fsync→send   (FR-8, FR-10)
│   │   ├── queues.py                 # thread-safe handoff
│   │   └── ratelimit.py              # token bucket + exit-priority queue        (FR-24)
│   ├── risk/
│   │   ├── gate.py                   # pre-submission validation gate           (FR-6)
│   │   ├── account.py strategy.py instrument.py order.py                        (FR-15)
│   │   ├── options.py                # hedge-first, naked prevention            (FR-16)
│   │   ├── basket.py                 # multi-leg, rollback                      (FR-17)
│   │   └── margin.py                 # SPAN shock simulation                    (FR-18)
│   ├── reconcile/
│   │   ├── reconciler.py             # continuous reconciliation                (FR-11, FR-12)
│   │   ├── corporate_actions.py      # split/bonus/symbol-change                (FR-5)
│   │   └── recovery.py               # crash recovery / intent-log replay       (FR-14)
│   ├── marketdata/
│   │   ├── states.py                 # Live/Stale/…, tradable-price             (FR-19)
│   │   ├── stream.py                 # WS ingest threads, dedup, REST fallback
│   │   └── watchdog.py               # connected-but-mute + self-health         (FR-20)
│   ├── refdata/
│   │   ├── instruments.py            # instrument-master lifecycle              (FR-3)
│   │   └── calendar.py               # trading-calendar lifecycle               (FR-4)
│   ├── session/
│   │   ├── manager.py                # login/token/health                       (FR-21)
│   │   └── safe_start.py             # cold-boot gate                           (FR-22)
│   ├── observability/
│   │   ├── logging.py                # structlog + secret redaction             (FR-27, FR-35)
│   │   ├── events.py                 # versioned event schema
│   │   ├── audit.py
│   │   └── reports.py                # EOD reconciliation report + heartbeat    (FR-29)
│   ├── modes/
│   │   ├── modes.py                  # live/paper/dry-run/replay/…              (FR-30)
│   │   └── killswitch.py             # 5 kill-switch types                      (FR-31)
│   ├── strategy/
│   │   ├── isolation.py              # virtual positions, per-strategy P&L      (FR-32)
│   │   └── accounts.py               # multi-account boundary                   (FR-33)
│   ├── config/settings.py            # pydantic-settings v2                     (FR-34)
│   ├── clock.py                      # Clock port re-export + stall detection   (FR-23)
│   ├── cli/main.py                   # argparse: status/reconcile/kill/…        (FR-36)
│   ├── health/server.py             # localhost /healthz /ready                (FR-36)
│   ├── api.py                        # the public strategy-facing facade
│   └── testing/                      # importable test seam
│       ├── fake_broker.py            # adversarial fault injection             (FR-37)
│       └── conformance.py            # adapter conformance kit
└── tests/
    ├── unit/…                        # mirrors src tree
    ├── conformance/                  # runs testing.conformance vs each adapter
    ├── durability/                   # SIGKILL intent-log tests                (FR-8)
    └── fixtures/
```

### Architectural Boundaries
- **Hexagonal boundary (load-bearing):** `domain/` + `ports/` are pure; `adapters/` depend inward on ports; **nothing in `domain/`/`ports/` imports `adapters/` or a broker SDK** (import-linter-enforced). The strategy-facing facade is `api.py`.
- **Thread boundary:** the synchronous `runtime/mainloop.py` is the only writer to `store/`. All I/O crosses into it via `runtime/queues.py`. No mutable state shared across threads.
- **Data boundary:** durable truth = `store/intent_log.py` (fsync); queryable projection = `store/sqlite_store.py`, rebuildable from the log; reference-data cache is date-versioned and staleness-gated.
- **Send boundary:** every broker mutation funnels through `runtime/dispatcher.dispatch()` — the single enforcement point for fsync-before-send and no-blind-retry.

### Requirements-to-Structure Mapping
F1→`adapters/brokers`+`ports/broker`; F2→`refdata/`; F3→`risk/gate`; F4→`store/intent_log`+`runtime/dispatcher`+`domain/lifecycle`; F5→`reconcile/`; F6→`risk/`; F7→`marketdata/`; F8→`session/`+`runtime/ratelimit`+`clock`; F9→`domain/errors`+`reconcile`; F10→`observability/`+`store/ledger`; F11→`modes/`; F12→`strategy/`; F13→`config/`+`adapters/secrets`; F14→`cli/`+`health/`+`testing/`.

### Data Flow (place-order path)
`strategy → api.place() → risk/gate → runtime/dispatcher.dispatch() → store/intent_log (fsync) → adapters/brokers/<broker> → result | UNKNOWN → store/sqlite + reconcile → observability/events + alert`.

### Cross-Cutting Concern Placement
Clock (`clock.py`, injected everywhere) · logging/redaction (`observability/logging`) · errors (`domain/errors`) · config (`config/settings`) · kill switch (`modes/killswitch`, checked in `risk/gate`) · reconciliation triggers (`reconcile/reconciler`, scheduled by `runtime/mainloop`).

## Architecture Validation Results

### Coherence Validation ✅
**Decision compatibility:** the stack is mutually reinforcing — SQLite single-writer (DA-2) ⇆ synchronous core (IA-3) ⇆ process-per-account (ID-1) ⇆ fsync intent log (DA-1) all assume and protect the same single-writer/single-account invariant. No contradictory decisions. Library versions are independent and current (Python 3.11+, pydantic-settings 2.14.2, structlog 26.1.0, cryptography 49.0.0, tenacity current, requests). No version conflicts (pure-Python, no native ABI coupling).
**Pattern consistency:** the consistency rules directly serve the decisions — `dispatch()` chokepoint enforces DA-1; Decimal/Clock/typed-error rules are mechanically enforced (Ruff banned-API, import-linter, mypy strict, conformance kit). Naming is uniform across code/SQL/events.
**Structure alignment:** the hexagonal tree realizes IA-1 (domain ⇏ adapters) physically; every boundary (hexagonal, thread, data, send) has a single owning module.

### Requirements Coverage Validation ✅
**Functional (FR-1…FR-37):** all 37 FRs map to a module (see Requirements-to-Structure Mapping); none orphaned. The 34 spec capabilities are transitively covered via the FR↔CAP links.
**Non-functional:** durability→DA-1; determinism→IA-3+Clock; zero-duplicate→`dispatch()`+intent log+conformance; schema versioning→DA-3; security→SE-1…SE-4; footprint→ID-1; observability-as-contract→ID-2/events. Perf budgets addressed as starting targets (see gaps).

### Implementation Readiness Validation ✅
**Decision completeness:** all critical decisions carry a choice + rationale + version where applicable. **Pattern completeness:** every identified conflict point has a rule + enforcement mechanism. **Structure completeness:** the tree is concrete (real files), with an explicit implementation sequence.

### Gap Analysis Results
**Critical gaps:** none — nothing blocks starting implementation.
**Important gaps (track, non-blocking):**
- **Perf budgets are `[ASSUMPTION]` targets** (gate+fsync <50ms, reconcile <5s, tick→state <100ms) — need a benchmark harness on the target VPS to confirm/adjust.
- **Per-broker `Capabilities` + error-taxonomy mapping has `unknown` entries** (`broker-capabilities.md`) — must be verified against the live Kite/Kotak SDKs during adapter implementation; the conformance kit is the gate.
- **Replay-mode determinism** depends on a frozen recorded-event format — spec the record schema alongside the intent log.
**Minor gaps (later):** secrets-manager adapter beyond env; Parquet-vs-SQLite for long-term reference-data history; WebSocket order-update stream interplay with reconciliation cadence.

### Validation Issues Addressed
No critical issues. Important gaps recorded for the architecture→epics handoff (each becomes an early story or spike); open business questions (distribution/positioning) are out of architectural scope.

### Architecture Completeness Checklist
**Requirements Analysis**
- [x] Project context thoroughly analyzed
- [x] Scale and complexity assessed
- [x] Technical constraints identified
- [x] Cross-cutting concerns mapped

**Architectural Decisions**
- [x] Critical decisions documented with versions
- [x] Technology stack fully specified
- [x] Integration patterns defined
- [x] Performance considerations addressed (budgets as confirm-later targets)

**Implementation Patterns**
- [x] Naming conventions established
- [x] Structure patterns defined
- [x] Communication patterns specified
- [x] Process patterns documented

**Project Structure**
- [x] Complete directory structure defined
- [x] Component boundaries established
- [x] Integration points mapped
- [x] Requirements to structure mapping complete

### Architecture Readiness Assessment
**Overall Status:** READY FOR IMPLEMENTATION (all 16 checklist items satisfied; no critical gaps)
**Confidence Level:** High
**Key Strengths:** safety invariants pushed into structure (fsync-before-send, single-writer, process-per-account); mechanically enforced consistency rules; a built-in adversarial test seam (fake broker + conformance kit); a clean hexagonal boundary that keeps broker churn out of the core.
**Areas for Future Enhancement:** confirm perf budgets via benchmarks; resolve `Capabilities`/error `unknown`s per broker; freeze the replay record schema; secrets-manager adapter; broker fallback routing.

### Implementation Handoff
**AI Agent Guidelines:** follow the decisions exactly; use the consistency rules everywhere; respect the four boundaries; route every send through `dispatch()`; never import a broker SDK in `domain/`.
**First Implementation Priority:** `uv init --package --lib --python 3.11` + the domain models/ports + the Store (intent log + SQLite + migrations) — i.e., the substrate before the first adapter.

> **Readiness correction (see Adversarial Review below):** the "no critical gaps / high confidence" verdict above was **overstated**. A 7-dimension adversarial review (45 findings, 43 verified: 6 critical, 23 high) ran before freeze. Three "critical" findings were ambiguities in an already-sound design (send topology, fsync→send window, client-ref anchor), now pinned; three were genuine additive gaps (kill control-plane, daily session establishment, freeze-quantity slicing), now designed. With the resolutions in the next section folded in, the architecture is **READY FOR IMPLEMENTATION** with per-epic verification items tracked.

## Adversarial Review Findings & Resolutions

A multi-agent adversarial review (Ultracode) ran across 7 dimensions before freeze: **45 findings raised, 43 survived refute-by-default verification (6 critical, 23 high, 12 medium, 2 low).** Most verdicts were "partial" — the design intent was sound but under-specified; the resolutions below **amend** the named decisions. The few genuine additive gaps are designed in full. Finding IDs are retained for traceability to the review.

### A. Concurrency & the single-writer model — clarifications
- **Send topology (amends IA-3, Send boundary) [CC-1]:** `send()` **and** the result-record execute on the synchronous main loop **inside `dispatch()`**. The "egress/rate-limit worker" is **admission control only** — an exit-priority queue + token bucket deciding *which* order enters `dispatch()` next and *when* a token is free; it never owns the broker socket or the Store. No result hand-back, no SENT-PENDING state. Exit-priority = dequeue order on the main loop.
- **fsync→send adjacency (amends DA-1, IA-4) [CC-2]:** admission completes *before* `dispatch()`; within `dispatch()`, record-intent+fsync and the socket write are adjacent on the main loop with **no thread hand-off** between them.
- **Reconcile = fetch-off-loop, apply-on-loop (amends IA-3, reconciler) [COH-1, CC-4]:** the scheduler/reconciler thread does broker **reads only** and enqueues an immutable `ReconcileResult`; the main loop applies **all** diffs (state, UNKNOWN→RECONCILED, corporate-action re-base, ledger writes). New review-blocking anti-pattern + import-linter contract: **no `store/`/`ledger` write outside `runtime/mainloop.py`.**
- **Reserved exit lane (amends `runtime/ratelimit`, CAP-12) [CC-5]:** square-off/cancel draw from a reserved token allocation entries can't consume; a per-send timeout bounds head-of-line blocking (timeout → UNKNOWN, free the slot). Conformance test: drain the bucket with entries, assert an exit still dispatches.
- **Health snapshot (amends D8/SE-4) [CC-6]:** the main loop publishes an immutable `HealthSnapshot` via a single atomic reference rebind; the endpoint thread reads only that reference — the **one sanctioned** read-only exception to "no shared mutable state". Wedge = snapshot age > N×publish-cadence.

### B. Order-safety mechanism — gaps now designed
- **Client-ref ↔ broker anchor [NFR-1, critical]:** UNKNOWN match-key **precedence** = captured broker `order_id` > short correlation token (`signal_hash8`, fits Kite ~20-char tag / Kotak remark) carried on the order where supported > attribute (symbol/side/qty/price/time) corroboration only > **fail-closed** (stay UNKNOWN, paused, alert). The long client-ref stays local. Add "tag carry/echo" as a capability dimension (graceful degradation, not a cert gate). Fake-broker tests: truncated/dropped tag + a colliding manual order ⇒ "stay UNKNOWN", never a second fire.
- **Freeze-quantity slicing [IBR-2, critical — NEW `risk/slicing.py`]:** a parent `OrderIntent` over the freeze limit fans out to N child intents (qty ≤ freeze, ≥ lot, lot-aligned remainder), each with a **deterministic** child client-ref `<parent_ref>#<k>` so re-slice on replay is bit-identical and `UNIQUE(client_ref)` dedupes a placed child for free. Parent FSM = fold over child states; **any child UNKNOWN ⇒ parent PARTIALLY_PLACED-with-UNKNOWN ⇒ blocks new risky orders.** `dispatch()` stays single-send per child. Gate rule: over-freeze ⇒ **slice** (default) or reject (config) — not "Invalid quantity". Composes with basket legs and the rate-limit burst.
- **UNKNOWN-pause scope — PINNED [CC-3]:** default = the **whole account process**; **exempt risk-reducing ops** (square-off, cancel, hedge-completion, configured emergency hedge/exit) so a protective leg is never frozen. Add an "UNKNOWN-pause active?" row to the validation gate; the flag lives in the per-process lifecycle layer. Per-strategy narrowing only if strategies provably share no net broker position.
- **Double-fault (UNKNOWN + broker down) [NFR-2]:** bounded reconcile-retry budget (backoff) → terminal `MANUAL_INTERVENTION_REQUIRED` state (queryable on `/healthz` + CLI) + dead-man's-switch escalation; the UNKNOWN-pause **overrides** "allow safe exits" for that scope; never auto-square-off a phantom. Fake-broker test for the combined fault.
- **Lifecycle apply-ordering [CC-7]:** terminal states are **absorbing**; transitions **forward-progressing**; apply a broker view only if its broker-sourced ordering key ≥ last applied; a reconciler full-orderbook snapshot is authoritative over a single WS push. Single-writer-serialized (no lock).

### C. Persistence, durability & perf — clarifications
- **One fsync on the hot path (amends DA-2) [NFR-3]:** only the **intent-log fsync** is pre-send; the idempotency check is an **in-memory** `client_ref` index read (rebuilt from the intent log at boot); the SQLite `UNIQUE(client_ref)` row is written **after** send as a single-writer backstop — not a `synchronous=FULL` pre-send insert. Perf budget restated as "one fsync + in-memory gate"; benchmark on the real (often network-attached) VPS storage class under IO load.
- **fsync honesty [NFR-4]:** durability is **conditional** on the operator's storage honoring fsync to stable media (mirrors the egress-IP obligation). Add a production-checklist item + a startup/operator fsync-honesty probe; an optional power-fail harness, labeled distinct from the SIGKILL (process-death) test.
- **Migration & forward-compat [NFR-5]:** each migration's DDL + version bump in **one transaction**; non-transactional ops use a checkpoint marker; a corrupt/half-migrated SQLite projection ⇒ **rebuild-from-intent-log** (not refuse-to-start). **Intent-log forward-compat:** an older *readable* intent log MUST replay in degraded reconcile/exit-only on a newer binary (don't brick an account mid-day); refuse-to-start only for a *newer unknown* version. Supervisor backoff + a fatal "do-not-restart, page operator" exit class for version mismatch.
- **Shared per-broker reference data (amends DA-4/D3) [COH-2, IBR-4]:** the instrument master + calendar move **out** of the per-account dir into a shared per-`(broker, segment, trading-date)` path; the first process wins the download under a **cross-process file lock**, siblings read the cache; safe-start freshness reads the shared cache. One download per `(broker, date)` regardless of account count. (Multi-account fast-follow.)

### D. Security — honest threat-model + redaction
- **Token-key co-location [SEC-1 → medium]:** SE-2 scoped honestly — env-key Fernet defends **offline artifacts** (accidental commit, disk/backup theft) only, **not** same-host root/process compromise. Require key/ciphertext non-co-location + a safe-start assertion + `0600`/`0700` perms; offer keyring/KMS via `SecretProvider` as opt-in hardening.
- **Ledger key-holder [SEC-2]:** CAP-30 wording corrected — tamper-evidence holds against **non-key-holder** edits + accidental corruption; the key-holder (operator) can re-sign a forged chain ⇒ true tamper-evidence needs the independent-anchoring Open Question. (Spec wording fix.)
- **Redaction beyond logging [SEC-3, SEC-6 — NEW `domain/redaction.py`]:** layered — field redactor **+ a final rendered-string scrubber keyed on token *shapes*** (Kite access/enc/request/public tokens, Kotak feed token, MPIN/TOTP digit patterns, token-named URL params); a secret-scrubbing domain exception base (adapters MUST wrap raw SDK exceptions before propagation); **bind the scrubber to the persistence path** (intent log + ledger), allowlist persisted fields at the `dispatch()` chokepoint, and **exclude session/auth responses from the inspectable chain entirely**. Conformance tests feed auth-exceptions / positional logs / raw login responses and assert zero token-shaped strings in logs, audit, **and** ledger.
- **Health endpoint isolation [SEC-4 → low]:** prefer a **Unix domain socket** (`0600`, per-account dir) over TCP loopback for `/healthz`; document loopback ≠ user/container isolation; GET-only, 404/405 else. (`kill` is a CLI/OS-process control, not an HTTP route.)
- **Key lifecycle [SEC-5 — NEW decision]:** **per-account** Fernet keys (never shared — preserves the D3 blast-radius), MultiFernet rotation, key-mismatch ⇒ fail-closed safe-start "re-login required"; Ed25519 keypair generated once per account at provisioning, **public key written to the data dir + EOD report**, key-id stamped in each signed head; add "crypto keys present/valid" to the safe-start gate.

### E. Sessions & Indian-broker realism
- **Daily session ESTABLISHMENT [IBR-1, critical — NEW `session/establish.py`]:** split establishment from expiry-detection. Kite's `access_token` dies daily (~6am) with **no headless refresh** — add a first-class adapter `establish_session` run **before** the safe-start gate; default = operator-supplied `request_token`/token-of-day at boot (adapter exchanges → `access_token`, stores encrypted); an optional automated TOTP-login adapter is flagged best-effort/ToS-sensitive. New capability dimension "headless session refresh": Kite = **unsupported**, Kotak = unknown. Error-taxonomy action becomes "re-establish session (daily interactive login may be required)", not "Refresh session". UJ-3 stays fail-closed-correct: cold-start **blocks** until the day's token is supplied.
- **WS re-auth [IBR-3]:** reconnect is **auth-aware** — distinguish transport vs token/auth failure; after N bounded attempts call session validation, stop socket retries on an invalid session, route to the session-expired posture (don't hammer the broker / mis-report token death as a feed glitch). Split tick-stream and order-update-stream ports. Order-status reconciliation does **not** depend on the order-update WS (pull is authoritative; WS is a latency optimization).
- **Reconciliation cadence [IBR-5, CC-7 — promoted from "minor"]:** **capability-driven** — order-update push primary where supported, else poll-only; **adaptive cadence** (tight ~1–2 s while SENT/UNKNOWN or any position open; loose ~15–30 s when flat); reserve rate-limit budget for reconcile polling **above** entries, **below** exits. This bounds the UNKNOWN-unblock / fill-detection latency floor that CAP-5 implicitly promises.
- **Kotak multi-step auth [IBR-6]:** the BrokerPort login is adapter-owned (possibly multi-step), returning a normalized `HEALTHY / NEEDS_REAUTH / FAILED`; "token storage" = an **opaque per-broker session bundle**, not one token. Add a Kotak auth-flow capability `unknown` to verify in the adapter epic.
- **Basket-margin fallback [IBR-7]:** "sum per-leg margin" is **not** a generic safe fallback — it over-states net-short option baskets (false-rejects) and is no basis for CAP-32. Where no basket/SPAN margin API exists, net-short basket margin **fails closed** (or an explicit audited operator opt-in); CAP-32 shock-sim is offered **only** where a basket/SPAN source exists, else "unavailable on this broker". Restrict summed-legs to single-leg/long-only.
- **Corporate-action source [RCT-4]:** extend the `refdata` port with a corporate-action-record source (split/bonus ratios); "absence surfaced, not assumed" = a config field + fail-visible path. (Fast-follow.)

### F. Operator control-plane & supervisor
- **Kill delivery [TO-1, critical — NEW `runtime/control.py`]:** the CLI `kill` is a *separate process* and cannot touch the engine's in-memory flag. Resolution: promote the localhost health server to accept a **single authenticated POST `kill {type, scope}`**; the handler enqueues a `KillCommand` on `runtime/queues`; the synchronous main loop dequeues and flips the in-process flag (preserves single-writer). Soft/strategy/broker/account kills keep the process alive (block entries, allow exits); panic may escalate to supervisor termination **after** the in-process square-off/cancel runs. A manual kill is fsync'd so a crash between "accepted" and "acted" replays as still-killed.
- **Kill-vs-in-flight race [TO-2]:** drain the command queue at the top of each loop iteration **and** immediately before `dispatch()`; bound every dangerous broker op with a per-call timeout so kill latency ≤ one timeout; panic exits proceed **even while an order is UNKNOWN** (exits are exempt from the pause); a fresh reconcile runs before square-off to avoid squaring off a phantom.
- **Supervisor exit-code contract [TO-5]:** entrypoint exit codes — `0` clean; a CRASH class ⇒ auto-restart with bounded backoff; a dedicated `FAIL_CLOSED_NEEDS_HUMAN` class ⇒ **do not auto-restart**, fire the dead-man's-switch absence alarm **before** exiting (closes the "slept through a bot that never started" gap). systemd `RestartPreventExitStatus` + `StartLimitIntervalSec`; the cron path uses a marker to avoid a flap loop.

### G. Testability
- **Conformance vs durability [TO-3]:** the in-process conformance kit covers UNKNOWN/no-blind-retry/reconciliation against an adapter's *declared* behavior; the **fsync→send ordering** is covered by the separate SIGKILL durability harness (add an injectable pre-send barrier so the kill lands deterministically in the window; optionally a real-socket fake-broker server for kernel-TCP-flush ordering).
- **Replay determinism [TO-4]:** in replay mode the injected Clock is **driven from the recorded timeline** (so every time-dependent path replays against recorded time); stamp a monotonic ingest-sequence on every event the main loop dequeues for one replayable total order — or document that replay reproduces decision-path order, not live thread interleaving. Acceptance: replay reproduces identical lifecycle transitions + intent-log output for the decision path.
- **Live-SDK certification tier [TO-6]:** an adapter is "certified" only when **both** pass — (tier 1) fake-broker conformance in CI, and (tier 2) production-checklist live-SDK verification that resolves every capability `unknown` and captures real error payloads (commit them as VCR-style fixtures for CI regression of CAP-13 cross-broker equivalence).
- **tenacity clock [COH-3]:** wire `ClockPort` into tenacity (`Retrying(sleep=ClockPort.sleep, wait=…reads ClockPort.monotonic)`) or roll a small backoff over `ClockPort.sleep`; extend the banned-API lint to `time.monotonic`/`time.sleep` outside the sanctioned wiring.

### H. Traceability & phasing corrections (cross-document)
- **FR-37 promotion [COH-4, applied]:** the adversarial fake broker + conformance kit move from "Should have soon" to **Day-One MVP** in `scope-and-phasing.md` and PRD §6 — SM-1/SM-2 and the #1 risk mitigation depend on it and the architecture already builds it Day-One.
- **FR-13 / FR-26 owners [RCT-1, RCT-2, RCT-6]:** **FR-13** → new `risk/funds_view.py` (funds snapshot with `fetched_at`, cadence refresh by the main-loop scheduler, **fail-closed** pre-order staleness gate in `risk/gate`); **FR-26** → new `modes/posture.py` degradation coordinator mapping each detector signal to an existing exit-only/block-entries/kill posture (one authority, gate reads it). Relabel the F1–F14 map as feature-level; annotate both FRs.
- **CAP-2 fallback de-conflated [RCT-3]:** MVP posture = **reject-only** on unsupported capability; per-capability *substitution* deferred to "Should have soon"; distinct from Level-4 "broker fallback routing".
- **Phase tags [RCT-5]:** add Day-One / Should-have-soon / Later tags to the source-tree modules; flag the one intentional deviation — the conformance kit is pulled early (Day-One) for safety though the spec tiers it fast-follow.

### New modules added (source tree deltas)
`runtime/control.py` (kill control-plane, TO-1) · `session/establish.py` (daily login, IBR-1) · `modes/posture.py` (degradation coordinator, FR-26) · `risk/funds_view.py` (funds/margin cadence + fail-closed, FR-13) · `risk/slicing.py` (freeze-qty slicer, IBR-2) · `domain/redaction.py` (shared secret-shape scrubber, SEC-3/SEC-6) · shared `refdata/` cache + cross-process lock (COH-2).

### Per-epic verification items (tracked, non-blocking for substrate)
Live-SDK resolution of every capability/error `unknown` (esp. Kotak margin/funds, order-update WS, basket margin, auth flow) · perf benchmarks on the real VPS storage class · fsync-honesty probe on target storage · the broker-specific realities honored in the Kite/Kotak adapter epics (daily login, freeze slicing, push-vs-poll cadence).

## Technology Stack Revision — Full Native C++ (supersedes the Python tech layer)

**Decision (2026-06-21):** the implementation language is **C++**, not Python. The language-agnostic *design* above is unchanged — hexagonal ports/adapters, the fsync'd write-ahead intent log + SQLite projection, the synchronous safety core + threads at I/O edges, the `dispatch()` chokepoint, process-per-account, and **every order-safety semantic and adversarial-review resolution** stand verbatim. What changes is the toolchain, the library choices, and — critically — the broker-transport approach. Current libraries verified via web research (2026).

### Major consequence — no official C++ broker SDK
Zerodha Kite and Kotak Neo ship **Python-only** SDKs. In C++ the broker adapters **implement the Kite Connect and Kotak Neo REST + WebSocket clients directly** against the documented HTTP APIs (auth/session exchange, order/portfolio endpoints, the binary tick protocol, order postbacks). This is the largest new surface, owned by the Kite (Epic 2) and Kotak (Epic 6) adapter epics. It **inverts** the earlier D7 "official SDKs underneath" stance and the non-goal "not a wrapper around pykiteconnect" — the new equivalent is "reimplement the documented broker REST+WS protocol cleanly behind the adapter port." The conformance kit + recorded live-payload fixtures (TO-6) cover it.

### Toolchain (supersedes "Starter Template Evaluation")
- **Language:** **C++20** (concepts for ports, `std::expected`-style `Result`, `std::chrono`, `std::filesystem`).
- **Build:** **CMake ≥ 3.28**; **Conan v2** for dependencies (prebuilt binary cache → fast CI; vcpkg an acceptable alternative).
- **Layout:** `include/broker_exec/` (public headers) + `src/` (impl) + `tests/`; CMake targets per layer enforce the hexagonal boundary (the `domain` target links no adapter/transport target — the C++ analog of the import-linter contract).
- **Quality gates:** clang-format, clang-tidy, **sanitizers (ASan/UBSan/TSan)** in CI — **TSan is doubly important** given the threaded I/O edges; `-Wall -Wextra -Werror`.
- **CI:** GitHub Actions matrix (gcc + clang, Debug+Release, sanitizer builds) → CMake build + `ctest` + conformance kit + SIGKILL durability test.

### Library choices (supersedes Step-4 ID-/SE-/DA-5)
- **HTTP (REST):** **libcurl** via **cpr** — broker REST + alert POSTs (replaces `requests`).
- **WebSocket:** **IXWebSocket** (no-Boost, SSL, proven) for tick + order-update streams (replaces the SDK ticker).
- **JSON:** **nlohmann/json** (config/REST/payloads); **simdjson** optionally for the hot tick-parse path.
- **TOML config:** **toml++** + a typed config struct; env via `std::getenv` (replaces pydantic-settings, DA-5).
- **Logging:** **spdlog** (on **fmt**) with a JSON sink + a secret-shape scrubbing formatter (replaces structlog, ID-2).
- **Crypto:** **OpenSSL** (AES-256-GCM token encryption-at-rest; SHA-256 hash chain) + **libsodium** (Ed25519 ledger signing) (replaces cryptography/Fernet, SE-2/SE-3).
- **SQLite:** the **SQLite C API** (amalgamation) directly — WAL + `synchronous=FULL`, `UNIQUE(client_ref)`; no ORM (same as DA-2).
- **CLI:** **CLI11** (replaces argparse, D8).
- **Health endpoint:** **cpp-httplib** (header-only) bound to localhost, or a Unix domain socket (SEC-4 preference).
- **Test:** **Catch2** (or GoogleTest) + the in-house fake-broker fixtures; time mocked via the injected `ClockPort` (no external lib).
- **Retry/backoff:** in-house bounded backoff over `ClockPort` (no tenacity — already the COH-3 resolution).

### Patterns (supersedes Step-5 specifics)
- **Money = a fixed-point integer type** (`Money`/`Price` storing paise as `int64_t`) — **never `double`/`float`**; one `round_to_tick()`. Lint bans float in money paths.
- **Time = `std::chrono` via the injected `ClockPort`** (`system_clock` wall, `steady_clock` monotonic/stall); ban direct `*_clock::now()` outside the system Clock impl.
- **Domain objects = value types/structs** (immutable where practical); **ports = abstract base classes / C++20 concepts**; the domain library has **no link dependency** on broker transport libs (CMake-enforced).
- **Error model = typed hierarchy / `std::expected<T, Error>`** carrying a `SuggestedAction`; **no exceptions cross the strategy boundary** (RAII internally; convert at the edge). Adapters wrap raw transport/HTTP errors → typed taxonomy + scrubbed.
- **Concurrency = `std::thread` + a thread-safe queue (`std::mutex`/`std::condition_variable`)**; the synchronous main loop remains the sole Store writer; TSan guards the boundaries.
- **RAII for every resource** (fds, sockets, SQLite handles, OpenSSL/sodium contexts); `dispatch()` does an explicit `fsync(fd)`/`fdatasync(fd)` before the socket write.

### Source tree (supersedes Step-6, same module decomposition)
`include/broker_exec/…` headers + `src/{domain,ports,adapters,store,runtime,risk,reconcile,marketdata,refdata,session,observability,modes,strategy,config,cli,health,testing}` mirroring the Python tree namespace-for-namespace; the 7 review-added modules carry over. **Greenfield Story 1 = the CMake + Conan scaffold** (replaces `uv init`); broker adapters add their own REST+WS client submodule.

### Unchanged
Every order-safety semantic and adversarial-review resolution (fsync-before-send, one fsync on the hot path, single-writer, UNKNOWN scope + match-key precedence, freeze slicing, daily session establishment, kill control-plane, degradation coordinator, redaction-on-persistence, key lifecycle, supervisor exit codes, adaptive reconcile cadence) is **language-agnostic and stands verbatim** — realized in C++ rather than Python.
