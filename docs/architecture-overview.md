# Architecture Overview

A condensed view of the design. The authoritative source is [architecture.md](../_bmad-output/planning-artifacts/architecture.md) and the [spec](../_bmad-output/specs/spec-broker-neutral-execution/SPEC.md).

## Layering

```
Strategy code
   │  (broker-neutral API — zero broker-specific identifiers)
   ▼
api.py / facade
   ▼
Risk gate ──▶ dispatch() ──▶ Broker adapter ──▶ Kite / Kotak Neo
   │             │  (record intent → fsync → send → record result|UNKNOWN)
   │             ▼
   │          Store: write-ahead intent log (durable truth) + SQLite projection
   ▼
Reconciler / safe-degradation / observability   (broker = source of truth)
```

## The four boundaries

- **Hexagonal boundary** — `domain/` + `ports/` are pure; `adapters/` depend inward; the domain links **no** broker transport library (enforced by CMake target dependencies).
- **Thread boundary** — a single synchronous main loop is the **only** writer to the Store. All I/O (WebSocket ingest, the rate-limit egress queue, alerts, the scheduler/reconciler) crosses into it via thread-safe queues. No shared mutable state across threads.
- **Data boundary** — durable truth is the fsync'd append-only intent log; the SQLite projection (WAL, `synchronous=FULL`) is rebuildable from it.
- **Send boundary** — every broker mutation funnels through `dispatch()`, the single enforcement point for *fsync-before-send* and *no-blind-retry*.

## Safety mechanisms (why duplicates can't happen)

1. **Unique client-side reference** per intent; `UNIQUE(client_ref)` in SQLite + an in-memory index.
2. **Write-ahead intent log** — the intent is `fsync`'d to disk before the socket write; on boot the log replays so the system can enumerate every order it might have sent.
3. **`UNKNOWN`-first** — an uncertain response never assumes success or failure; it pauses new risky entries (process-wide; risk-reducing exits exempt) and reconciles via order book → trade book → positions. Match precedence: broker `order_id` > short correlation token > attribute corroboration > **fail-closed**.
4. **No blind retry** — `place/modify/cancel/square_off` never auto-repeat on timeout.
5. **Freeze-quantity slicing** — large orders fan out to children with deterministic `<parent>#<k>` refs, so re-slicing on replay is bit-identical and a placed child is deduped for free.
6. **Reconciliation-first + crash recovery** — never trade immediately on restart; reconcile, then resume.

## Risk & option-selling

- Four levels: account / strategy / instrument / order.
- Option-selling: **hedge-first** (the short is sent only after the hedge confirms), naked-sell prevention, emergency behavior, and a **capability-gated margin/SPAN shock simulation** (offered only where the broker exposes a basket/SPAN margin source).
- Baskets are one logical trade with leg dependency, partial-fill handling, and rollback.

## Operability

- **Modes:** live / paper / dry-run / replay / monitor-only / exit-only / emergency.
- **Kill switches:** soft / strategy / broker / account / panic, delivered to the running process via an authenticated control-plane command.
- **Observability:** structured logs, audit trail, a tamper-evident hash-chained + Ed25519-signed ledger, an EOD signed reconciliation report, and a push position heartbeat with a dead-man's-switch.
- **Safe-start gate:** trading is blocked on cold-boot until session, reconciliation, clock sanity, config, fresh instrument master + calendar, egress-IP, and crypto keys all verify.
- **One OS process per account** under a supervisor — the kill-switch blast radius is exactly one account.

## Technology stack (C++)

| Concern | Choice |
|---|---|
| Language / build | C++20 · CMake ≥ 3.28 · Conan v2 · gcc+clang + ASan/UBSan/TSan |
| REST / WebSocket | cpr (libcurl) · IXWebSocket — **the Kite/Kotak transport is implemented directly (no official C++ SDK)** |
| JSON / config | nlohmann/json · toml++ (+ env) |
| Logging | spdlog / fmt (+ secret-shape redaction) |
| Crypto | OpenSSL (AES-256-GCM, SHA-256) · libsodium (Ed25519) |
| Storage | SQLite C API (WAL) + the fsync'd intent log |
| CLI / health | CLI11 · cpp-httplib (localhost / Unix socket) |
| Tests | Catch2 + the in-house adversarial fake broker + conformance kit |
| Money / time | fixed-point `int64` paise · injected `Clock` (`std::chrono`) |

## Verification

The headline invariant — **zero duplicate orders across the full fault matrix** — is proven by an adversarial **fake broker** (delayed/dropped acks, duplicate fills, out-of-order events, 429s, ack-lost-but-placed) plus a **SIGKILL durability harness** that kills the process between fsync and send. The architecture was hardened by a multi-agent adversarial review (45 findings, 43 confirmed) whose resolutions are recorded in [architecture.md](../_bmad-output/planning-artifacts/architecture.md).
