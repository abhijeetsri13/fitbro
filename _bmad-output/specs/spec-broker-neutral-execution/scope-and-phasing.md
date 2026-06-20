# Scope, Phasing, Reliability Levels, Acceptance Criteria & Production Checklist

Backs **CAP-22** and gates the SPEC's **Success signal**. This is the phasing and done-definition contract downstream epic/story planning derives from. The library targets **Reliability Level 3 (Production Execution Engine)** as the goal, with Level 4 as long-term vision.

## MVP phasing

### Must have (Day One)

Kite support · Kotak Neo support · unified order placement · unified order status · unified positions · unified trades · unified margins/funds where possible · order validation · risk checks · idempotency · no blind retry · order reconciliation · position reconciliation · rate-limit protection · audit logs · alerts · kill switch · paper mode · dry-run mode · config-driven broker selection · **write-ahead intent log (CAP-26)** · **clock abstraction + stall detection (CAP-27)** · **safe-start cold-boot gate (CAP-28)** · **instrument-master daily refresh + symbol/token resolution + staleness gating (CAP-33)** · **trading-calendar source + refresh + staleness gating (CAP-21)** · **funds/margin intraday refresh cadence (CAP-6)** · **thin CLI + localhost health endpoint (D8)** · **Telegram + webhook alerts with dead-man's-switch heartbeat (CAP-30/D5)** · **adversarial fake broker + adapter conformance kit (CAP-31)**.

> The write-ahead intent log, clock abstraction, and safe-start gate are promoted into Day-One MVP because the party debate found they are the *substrate* that makes idempotency, crash recovery, and reconciliation actually compose — without them, those three are islands (`technical-architecture.md`). The **adversarial fake broker + conformance kit (CAP-31)** is likewise Day-One: it is the verification substrate the primary acceptance metric (SM-1, zero duplicate orders under fault injection) and the #1 risk mitigation depend on — deferring the only tool that injects the faults would make the MVP unverifiable against its own headline promise (adversarial-review finding COH-4, 2026-06-21).

### Should have soon

WebSocket market data · WebSocket order updates · stale-data detection · "connected-but-mute" feed watchdog + process self-health (CAP-29) · basket order support · hedge-first execution · margin/SPAN shock simulation (CAP-32) · strategy-wise P&L · strategy-wise positions · expiry-day rules · auto square-off · corporate-action awareness (CAP-34) · crash recovery · multiple accounts (process-per-account supervisor) · **tamper-evident hash-chained signed ledger + EOD reconciliation report + push position heartbeat (CAP-30)**.

### Later

Broker fallback · smart order routing · historical-data abstraction · option-chain abstraction · Greeks integration · backtesting/replay · dashboard · Prometheus/Grafana metrics · cloud deployment templates · **third-party proof tooling (dispute packet, external verifier) — evidence-gated, see Product positioning**.

## Product positioning (staged)

Resolved in the BMAD party-mode debate (Mary/Victor/John, 2026-06-17):

- **Now:** build the **reliability library** sold as "run a real-money algo and sleep at night." The 25→32-capability Level-3 spec *is* the product. The durable intent log and reconciliation ship because they make the engine *correct*, framed as "no duplicate orders, fails visibly, clean recovery" — not as "courtroom proof."
- **The tamper-evident ledger (CAP-30) ships as a *system-of-record foundation*, not a billed product.** Hash-chaining + signing the existing intent log is nearly free; throwing the evidence away would not be.
- **Deferred & evidence-gated:** any third-party-facing proof tooling (dispute packet, external verifier, pricing). Reopen only when (a) usage telemetry shows operators export the record after incidents, **and** (b) independent third-party anchoring is solved (a self-signed, self-hosted log proves consistency, not truth — see SPEC Non-goals).
- **Distribution:** open-core on **public PyPI** is the working assumption (you cannot beat OpenAlgo invisibly), with the signed-reconciliation layer as a later paid tier — kept viable by the Direct-adapters/no-AGPL choice (`technical-architecture.md` D7). Final call is an open question (market-driven).

## Reliability levels

| Level | Name | Includes | Verdict |
|---|---|---|---|
| 1 | Basic wrapper | Place orders; fetch positions; fetch order book. | Not enough for serious trading. |
| 2 | Safe execution library | Order validation; broker-difference handling; full logging; basic risk checks. | OK for manual-assisted algo trading. |
| 3 | **Production execution engine** | Idempotency; reconciliation; kill switch; rate limits; state machine; alerts; crash recovery; unknown-order handling. | **This is the target.** |
| 4 | OMS-lite | Multi-broker routing; multi-account; basket execution; strategy-wise positions; advanced risk; monitoring dashboard; post-trade analytics. | Long-term vision. |

## Acceptance criteria (production-ready when ALL true)

- Every order has a unique client-side reference.
- Every order is validated before submission.
- Every order is persisted before or during execution.
- No dangerous operation is blindly retried.
- Unknown order state blocks new risky orders.
- Broker order-book reconciliation works.
- Trade-book reconciliation works.
- Position reconciliation works.
- Manual intervention is detected.
- Rate limits are enforced before broker rejection.
- Kill switch is tested.
- Market-data staleness is detected.
- Session expiry is detected.
- Secrets are never logged.
- Audit logs are complete.
- Alerts are configured.
- Paper mode has been tested.
- Live mode has been tested with minimum quantity.
- Restart recovery has been tested.
- WebSocket disconnect recovery has been tested.
- Broker API timeout behavior has been tested.
- Position mismatch behavior has been tested.

## Production checklist (before real money)

- [ ] Kite adapter tested
- [ ] Kotak Neo adapter tested
- [ ] Order validation tested
- [ ] Lot-size validation tested
- [ ] Tick-size validation tested
- [ ] Instrument-master daily refresh tested
- [ ] Stale/failed instrument-master blocks trading at safe-start (tested)
- [ ] Symbol↔token resolution + expired-contract rejection tested
- [ ] Trading-calendar refresh + stale-calendar safe-start block tested
- [ ] Corporate-action (split/symbol-change) reconciliation — no false mismatch, no duplicate order — tested
- [ ] Funds/margin intraday refresh cadence + stale-margin fail-closed tested
- [ ] Risk rules tested
- [ ] Rate limiter tested
- [ ] Idempotency tested
- [ ] Unknown-order handling tested
- [ ] Order reconciliation tested
- [ ] Position reconciliation tested
- [ ] Manual-intervention detection tested
- [ ] Kill switch tested
- [ ] Square-off tested
- [ ] Basket execution tested
- [ ] Hedge-first execution tested
- [ ] Market-data staleness detection tested
- [ ] WebSocket reconnect tested
- [ ] Broker session expiry tested
- [ ] Token storage secured
- [ ] Secrets removed from logs
- [ ] Paper mode tested for at least 20 sessions
- [ ] Live mode tested with minimum quantity only
- [ ] Daily report generated
- [ ] Alerting verified
- [ ] Static-IP compliance checked if required by broker/regulation
- [ ] Manual emergency process documented

## Prior art (audit before reuse, do not substitute)

- **Official broker SDKs** (Kite Connect, Kotak Neo, Dhan, Angel SmartAPI, Fyers): used *underneath* this library, not as a substitute. They do not solve broker-neutral design, idempotency, cross-broker reconciliation, unified risk, multi-strategy isolation, or production-grade failure behavior.
- **OpenAlgo**: closer to a full platform; useful to study broker abstraction, but heavier than a clean execution-core library under full control.
- **Fenix and similar Indian abstraction libraries**: audit maintenance, broker coverage, error-handling quality, order-safety behavior, license, test coverage, adoption, and security before any live use — a weak abstraction is more dangerous than none.
