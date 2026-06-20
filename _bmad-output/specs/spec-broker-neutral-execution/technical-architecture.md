# Technical Architecture — Resolved Decisions

Backs **CAP-26…CAP-32** and resolves the SPEC's original seven open questions. These were settled in a BMAD party-mode debate (Winston/Architect, Amelia/Engineer, Murat/Test Architect — near-unanimous) on 2026-06-17. Each is a decision with a default and the condition that would reverse it. Downstream architecture work starts from here, not from a blank page.

## D1 — Persistence: fsync'd write-ahead intent log (durable truth) + SQLite projection

- **Durable truth** = a standalone, append-only, **fsync'd write-ahead intent log**. Serialize the intent (monotonic seq + unique client-ref) → write → `fsync` **before** the broker socket write → then send. Replay head-to-tail on boot.
- **Queryable state** = **SQLite** in WAL mode, `PRAGMA synchronous=FULL`, holding order lifecycle, audit, and the idempotency index (`UNIQUE` on client-ref enforced on the hot path). Rebuildable from the intent log.
- **Not Postgres** at baseline: a second daemon adds a network-shaped failure class to a single-VPS system for zero concurrency gain. **Reverses if:** target becomes multi-VPS/multi-node.
- Durability comes from fsync discipline, not the DB brand. Testable by SIGKILL between fsync and socket write → restart → assert no duplicate order.

## D2 — Concurrency: synchronous safety core + threads at I/O edges

- The decision path (write intent log → check idempotency → decide send/don't-send) is **synchronous, linear, single-threaded** — readable under stress, deterministic with an injected clock.
- **Threads only at I/O boundaries**: WebSocket tick ingest (SDK-owned callback threads → `queue.Queue`), the rate-limit egress queue (one token bucket under a `Lock`), alert sending.
- **No asyncio in the core.** `pykiteconnect` and the Kotak Neo SDK are sync request/response with callback tickers; an event loop sharing threads with SDK callbacks yields invisible stalls and vanishing stack traces. **Reverses only for** a broker SDK that is async-only and unwrappable → isolate *that adapter* in its own loop on its own thread; never let async reach the core.

## D3 — Multi-account isolation: one OS process per account + thin supervisor

- **One process per account.** Each owns its **own** intent log + SQLite file + kill switch. A kill switch becomes physical and absolute: kill the process, the OS reclaims the socket, the file is fsync-consistent, recovery replays cleanly. Blast radius = exactly one account.
- A **thin supervisor** (systemd template unit `trader@<account>.service`, or a small launcher) starts/stops/restarts processes. No shared address space, no cross-account GIL coupling.
- **MVP simplification:** single-account ships single-process behind a clean account-boundary interface; go multi-process when account #2 is real (aligns with the "earn breadth" scope discipline).
- Testable: spin two processes, SIGKILL one, assert the other keeps trading and the supervisor restarts the dead one from its own intent log.

## D4 — Runtime & packaging — SUPERSEDED → C++ (was: Python 3.11+, PEP 621)

> **Superseded 2026-06-21:** implementation is **C++20** — CMake ≥ 3.28 + Conan v2 (prebuilt binary cache = the provable "prod runs what CI tested" lockfile equivalent), `include/` + `src/` layout, broker adapters as separate CMake targets enforcing the hexagonal boundary, CI matrix gcc+clang with ASan/UBSan/TSan. See architecture.md "Technology Stack Revision — Full Native C++". The Python text below is retained as history.

- **Python 3.11 floor** (`tomllib` in stdlib, exception groups, better tracebacks — material for a forensics library; old enough that every VPS has it). CI matrix 3.11 + 3.12.
- `pyproject.toml` / PEP 621, `src/` layout, broker adapters as **optional extras** (`pip install <lib>[kite,neo]`). Hashed lockfile so "prod runs what CI tested" is provable. **No namespace packages.**
- **Distribution stance (public PyPI open-core vs private/licensed) is deferred** — see SPEC `## Open Questions`; it depends on the product-positioning decision, and the *Direct-adapters / no-AGPL* choice (D7) keeps the open-core option open.

## D5 — Alerting: Telegram + generic webhook, behind a dead-man's-switch

- One internal `AlertSink` interface, two MVP implementations: **Telegram** (de-facto channel for Indian retail/prop; phone push, free) and a **generic webhook** (universal escape hatch — Slack/Discord/PagerDuty/dashboard all speak "POST JSON").
- **Email deferred** (SMTP deliverability/spam fails silently during incidents — the worst time).
- **Mandatory dead-man's-switch heartbeat:** the alerter emits a periodic "alive" and an *external* watcher alarms on its **absence** — because silent alert failure is invisible until the incident you needed it for. Every channel ships behind `send_test_alert()` invoked from the production checklist.

## D6 — Static-IP / regulatory: enforce a safe-start egress-IP gate; document the obligation

- At the **safe-start gate** (CAP-28): resolve the egress IP, compare to an operator-declared allowlist, and **refuse to start trading — loudly — on mismatch or non-verification** (fail closed).
- The library **enforces the operator's stated intent**, it does **not** claim to enforce the regulation — static-IP provisioning, broker allowlisting, and SEBI/exchange registration live outside the process and remain the operator's legal obligation. Documented, surfaced, verified — never falsely promised.

## D7 — OpenAlgo stance: direct adapters only (no runtime dependency)

- Broker adapters implement the **Kite Connect / Kotak Neo REST + WebSocket protocol directly** (no official C++ SDK exists — see the C++ revision; this replaces the earlier "talk to pykiteconnect / Kotak Neo SDK" wording). **OpenAlgo is prior-art to study, not a dependency or backend.**
- Rationale: OpenAlgo is a self-hosted **AGPL-v3** full-stack *platform* (Flask+React server, REST/WebSocket), solving broker *connectivity* — not the execution-*safety* core (idempotency, UNKNOWN handling, reconciliation-first, no-blind-retry, kill switches) this library exists to provide. Depending on it would add a daemon to run/secure, route "broker = source of truth" through a middleman, break the embedded-library/process-per-account model, and couple the project to AGPL.
- The broker-neutral interface (CAP-1) and capability model (CAP-2) leave the door open to add an OpenAlgo adapter later for broad broker reach, **without** the safety core depending on it. (Decision 2026-06-17.)

## D8 — Operator surface: library + thin CLI + localhost health endpoint

- **Library** is the product. Plus a **thin CLI** (`status`, `reconcile`, `replay-intent-log`, `safe-start-check`, `send-test-alert`, `verify-ip`, and `kill`) and a **localhost-only** health endpoint (stdlib `http.server`; `/healthz` + `/ready` exposing session state, last-heartbeat/tick age, in-flight count, clock sanity, replay-clean flag).
- CLI and endpoint are **thin shells over the library's public API** — no logic of their own. **Read-only except `kill`** (the one mutation that *reduces* risk). The health endpoint is also how the D3 supervisor detects a wedged process; the CLI verbs double as the executable production checklist.

## The through-line

D1 + D2 + D3 are one coherent stance: **single-writer storage → single-threaded decisions → single-account processes.** Together they make "no duplicate orders, clean recovery, bounded blast radius" an *architectural* guarantee rather than something the code must remember to do. Every choice stays verifiable by SIGKILL + an injected clock.
