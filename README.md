# Broker-Neutral Trading Execution Library

A **broker-neutral execution and safety layer** that sits between strategy code and Indian broker APIs (Zerodha **Kite** and Kotak **Neo**, extensible to others). It is **not** a thin SDK wrapper and **not** a strategy engine — it is the execution-safety core those leave out.

> **The promise is not "nothing fails."** Broker APIs, sessions, WebSockets, exchanges, and VPS infrastructure all fail. The promise is:
>
> **When something fails, it fails _visibly_, _safely_, _without duplicate orders_, and _with a clear recovery path_.**

**Status:** 📐 **Design / planning complete — pre-implementation.** This repository currently holds the full design contract (spec → PRD → architecture → epics & stories). Implementation language is **C++ (C++20)**.

---

## What it does

```
Strategy
  ↓
Trading Library  ──  Risk checks · Order safety · Idempotency
  ↓
Broker Abstraction  ──  Kite / Kotak Neo / future brokers
  ↓
Reconciliation & Monitoring  (broker = source of truth)
```

A strategy says *"sell this NIFTY option, hedge-first."* The library validates it against four levels of risk, assigns a unique client reference, writes the intent to a durable log **before** the socket call, normalizes the broker's response, and owns the order's full lifecycle — treating any uncertain outcome as `UNKNOWN` until it reconciles against the broker.

## Core guarantees

- **No silent failure · no blind retry · no duplicate order · no unknown position ignored.**
- **Idempotent order placement** — every intent has a unique client-side reference; a repeat never creates a second order.
- **Write-ahead intent log** — fsync'd to disk *before* the broker socket write; replayed on boot.
- **`UNKNOWN`-first** — an uncertain response pauses new risky entries (exits exempt) until reconciled against order book → trade book → positions.
- **Reconciliation-first** — the broker is the source of truth; local state is a working copy.
- **Four-level risk engine** — account / strategy / instrument / order, plus option-selling safety (hedge-first, naked-sell prevention) and a capability-gated margin/SPAN shock check.
- **Safe degradation** — broker outage, stale data, unknown order, mismatch, risk breach, or session expiry each degrade to a defined posture.
- **Kill switches, modes, reconciliation, audit ledger, alerts** — built for unattended operation on a VPS.

## What it is *not*

- Not a wrapper around a broker SDK (in C++ there is no official SDK — adapters implement the documented REST + WebSocket protocol directly).
- Not a strategy engine, backtester, no-code platform, or black box.
- Not built on OpenAlgo or any broker-bridge platform.

## Technology

- **Language:** C++20 · **Build:** CMake ≥ 3.28 + Conan v2 · **CI:** gcc/clang + ASan/UBSan/TSan + `ctest`.
- **Libraries:** cpr/libcurl (REST), IXWebSocket (WS), nlohmann/json, toml++, spdlog/fmt, OpenSSL + libsodium, SQLite C API, CLI11, cpp-httplib, Catch2.
- **Design:** hexagonal ports & adapters (domain links no broker transport), synchronous safety core + threads at I/O edges, fsync'd intent log + SQLite projection, one OS process per account.
- **Money is exact** — fixed-point `int64` paise, never floating point.

## Documentation

| Doc | What it is |
|---|---|
| [docs/](docs/) | Documentation index |
| [docs/architecture-overview.md](docs/architecture-overview.md) | Design, safety guarantees, C++ stack |
| [docs/multi-account.md](docs/multi-account.md) | Running several accounts: process-per-account, shared refdata cache, exit-code contract, systemd |
| [docs/roadmap.md](docs/roadmap.md) | The 6 epics and MVP scope |
| [Specification (SPEC.md + companions)](_bmad-output/specs/spec-broker-neutral-execution/SPEC.md) | The canonical contract — 34 capabilities |
| [PRD](_bmad-output/planning-artifacts/prds/prd-fitbro-2026-06-17/prd.md) | 14 features, FR-1…FR-37 |
| [Architecture](_bmad-output/planning-artifacts/architecture.md) | Decisions, patterns, source tree, adversarial review |
| [Epics & Stories](_bmad-output/planning-artifacts/epics.md) | 6 epics, 49 implementable stories |

## Multi-account operation

Several broker accounts on one machine run as **one OS process per account** — an
account is a blast radius, and the OS is the only isolation boundary that holds
under a segfault, an OOM kill or a stuck socket. Each process owns a private
`0700` data directory (intent log, SQLite projection, token store, ledger, kill
journal) derived from a validated account id, so two accounts can never share a
file.

The one thing they *do* share is reference data: the instrument master and
trading calendar are identical per `(broker, segment, trading_date)`, so a
**shared cache behind a cross-process file lock** lets exactly one process
download while the rest read its atomically published artifact — one download for
N accounts, and a fail-closed `DataStale` error rather than a stampede if the
lock cannot be had.

Supervision is split on purpose: the library owns the **decision**
(`SupervisorPlan` — per-account restart backoff, crash-loop breaker and absence
alarm, with account A's crash loop provably unable to affect account B), and
**systemd owns the spawning** through the template unit in
[`deploy/systemd/broker-exec@.service`](deploy/systemd/broker-exec@.service). The
library never forks, execs or spawns.

See [docs/multi-account.md](docs/multi-account.md) for the layout, the exit-code
contract table and the systemd usage.

## Roadmap (epics)

1. **Order-Safety Substrate** — provable zero-duplicate against a fake broker.
2. **Live Kite Trading** — validation gate, risk, reference data, session.
3. **Resilience** — reconciliation, recovery, safe degradation, kill switches.
4. **Operator Visibility** — observability, alerts, ledger, modes, CLI.
5. **Option-Selling Safety** — hedge-first, baskets, slicing, margin shock.
6. **Breadth** — Kotak Neo adapter & multi-account.

## License

Licensing/distribution is an open product decision (open-core vs private) and is **not yet chosen** — until then, all rights reserved by the author. See the open questions in the [spec](_bmad-output/specs/spec-broker-neutral-execution/SPEC.md).

---

*Planned and documented with the BMAD method. ⚠️ Trading involves real financial risk; this library is unfinished and not yet suitable for real-money use.*
