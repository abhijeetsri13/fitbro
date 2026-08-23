# Roadmap

Six epics decomposed into 49 implementable stories. The full breakdown with acceptance criteria is in [epics.md](../_bmad-output/planning-artifacts/epics.md). Discipline: **Kite-bulletproof first, earn breadth later.**

## Epics

| # | Epic | Status | Demonstrable value |
|---|---|---|---|
| 1 | **Order-Safety Substrate** | ✅ implemented | A strategy places orders vs an adversarial fake broker; **zero duplicate orders provable** across the fault matrix (incl. SIGKILL-between-fsync-and-send). |
| 2 | **Live Kite Trading** | ✅ implemented | Validated, risk-checked orders on real Kite (paper / dry-run / live-min-qty); safe-start blocks until session + reference data are fresh. |
| 3 | **Resilience** | ✅ implemented | Reconciles, recovers after a crash, degrades safely under each failure; the operator can kill it (soft → panic). |
| 4 | **Operator Visibility** | ✅ implemented | A silent breach pushes to the operator's phone; EOD signed report; `kill` / `status` / `reconcile` from the CLI; tamper-evident ledger. |
| 5 | **Option-Selling Safety** | ✅ implemented | A hedged NIFTY/BANKNIFTY basket as one logical trade — hedge-first, sliced over freeze, blocked if a vol-shock projection crosses the auto-square-off threshold. |
| 6 | **Breadth — Kotak + Multi-Account** | ✅ implemented | The same unchanged strategy runs on Kotak Neo; isolated multi-strategy / multi-account with bounded kill blast radius. |

> **“Implemented” means the epic's modules are built and their tests pass — not that the epic runs.**
> The synchronous trading main loop is not written: `broker-exec run` completes the whole cold-boot
> sequence and then exits 70 via `boot::unimplemented_run_phase()` rather than pretend to trade, and
> composition is `EngineMode::ExitOnly` because an entry-capable assembly needs inputs only that loop
> produces. So several modules below — hedge-first execution, basket unwind, the UNKNOWN-first pause —
> are exercised only by their tests and have no production call site yet. Nothing here has traded real
> money. Defects found since are tracked as [open issues](https://github.com/abhijeetsri13/fitbro/issues).

## MVP (Day One)

Kite + Kotak Neo adapters · unified order/status/positions/trades/margins · capability model · validation gate · four-level risk · idempotency + write-ahead intent log · no blind retry · order/position reconciliation · instrument-master + trading-calendar refresh with staleness gating · clock abstraction + stall detection · safe-start gate · rate-limit protection · error taxonomy · audit logs · Telegram + webhook alerts with a dead-man's-switch · kill switches · paper + dry-run modes · config-driven broker selection · thin CLI + localhost health endpoint · **adversarial fake broker + conformance kit** (the SM-1 verification substrate).

## Should have soon

WebSocket market data + order updates · connected-but-mute watchdog · basket execution · hedge-first · margin/SPAN shock simulation · corporate-action awareness · crash-recovery polish · multi-account supervisor · strategy-wise P&L/positions · expiry-day rules · auto square-off · tamper-evident ledger + EOD report + heartbeat.

## Later

Broker fallback / smart order routing · historical-data & option-chain abstraction · Greeks · replay polish · dashboard · Prometheus/Grafana · cloud templates · third-party "proof" tooling (evidence-gated).

## Open product questions (do not block implementation)

1. **Positioning & distribution** — open-core vs private/self-infrastructure.
2. **Target segment** — is the prop-desk / PMS "never lose the broker argument" wedge worth pursuing?
3. **Independent anchoring** — if the system-of-record product is pursued, what third-party timestamping gives the ledger standing the operator doesn't control?
