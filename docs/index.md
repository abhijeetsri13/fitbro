# Documentation

Documentation for the **Broker-Neutral Trading Execution Library** (C++20). The library is in the **design/planning** phase — these documents are the contract implementation derives from.

## Start here
- [Project README](../README.md) — what it is, the core promise, the stack.
- [Architecture Overview](architecture-overview.md) — the design, the safety guarantees, and the C++ technology stack.
- [Multi-account operation](multi-account.md) — process-per-account layout, the shared refdata cache and its cross-process lock, the exit-code contract, and the systemd template unit.
- [Roadmap](roadmap.md) — the 6 epics and what ships in the MVP.

## Operations
- [Upgrade: IMP-11 stop orders](upgrade-imp-11-stops.md) — **flatten stops before deploying (either direction).** Why a stop's signal signature changes across this release, the duplicate-order hazard that follows, and the cold-boot gate that enforces the procedure.

## The full design contract
- **Specification** — the canonical machine contract (34 capabilities CAP-1…CAP-34):
  - [SPEC.md](../_bmad-output/specs/spec-broker-neutral-execution/SPEC.md) and its companions in the same folder:
    `glossary` · `broker-capabilities` · `order-lifecycle` · `risk-controls` · `error-taxonomy` ·
    `resilience-and-reconciliation` · `operational-modes` · `scope-and-phasing` · `architecture-diagrams` ·
    `technical-architecture`.
- **PRD** — [prd.md](../_bmad-output/planning-artifacts/prds/prd-fitbro-2026-06-17/prd.md) — 14 features, FR-1…FR-37, success metrics.
- **Architecture** — [architecture.md](../_bmad-output/planning-artifacts/architecture.md) — decisions (D1–D8), implementation patterns, source tree, the **Adversarial Review Findings & Resolutions**, and the **C++ technology revision**.
- **Epics & Stories** — [epics.md](../_bmad-output/planning-artifacts/epics.md) — 6 epics, 49 stories with Given/When/Then acceptance criteria.

## Conventions
- **Money** is fixed-point `int64` paise — never floating point.
- **Time** is always accessed through the injected `Clock` (deterministic tests + faithful replay).
- **Every broker send** flows through the single `dispatch()` chokepoint (fsync-before-send, no blind retry).
- **The domain core links no broker transport** — broker specifics live only in adapters (hexagonal boundary, CMake-enforced).
