---
title: Broker-Neutral Trading Execution Library
status: draft
created: 2026-06-17
updated: 2026-06-17
source_spec: ../../../specs/spec-broker-neutral-execution/SPEC.md
---

# PRD: Broker-Neutral Trading Execution Library
*Working title — confirm.*

## 0. Document Purpose

This PRD is for the downstream BMAD workflow owners (architecture, epics/stories, dev) and for the builder/stakeholder reviewing scope. It is derived from the canonical spec at `_bmad-output/specs/spec-broker-neutral-execution/` (SPEC.md + 11 companions, CAP-1…CAP-34) and its resolved architecture (`technical-architecture.md` D1–D8). It does **not** duplicate the spec's companion catalogs — it references them. Vocabulary is anchored to the spec glossary (§3). Features are grouped with globally numbered Functional Requirements (FR-N) nested under them; inferred decisions carry inline `[ASSUMPTION: ...]` tags, indexed in §9. Where the spec already states a testable success criterion, the FR's testable consequences inherit it.

## 1. Vision

Algorithmic traders on Indian broker APIs lose money not to bad strategies but to bad *plumbing*: duplicate orders from blind retries, silent failures swallowed by thin SDK wrappers, unknown order states after timeouts, position drift after manual intervention, and naked short option legs after a failed hedge. The official SDKs (Kite Connect, Kotak Neo) give API access and solve none of this. This product is the **execution, safety, and broker-abstraction layer that sits between strategy code and broker APIs** — the layer the SDKs leave out.

It is a C++ (C++20) library a strategy links in-process. The strategy says "sell this NIFTY option, hedge-first"; the library validates it against four levels of risk, assigns a unique client reference, writes the intent to a durable log *before* the socket call, normalizes the broker's response, and owns the order's full lifecycle — treating any uncertain outcome as `UNKNOWN` until it reconciles against the broker (the source of truth).

The promise is not "nothing fails." Broker APIs, sessions, WebSockets, exchanges, and VPS infrastructure all fail. The promise is: **when something fails, it fails visibly, safely, without duplicate orders, and with a clear recovery path.** Success is a trader who can run a real-money options algo on a VPS and sleep at night — and switch from Kite to Kotak Neo by changing one config value.

## 2. Target User

### 2.1 Jobs To Be Done

- **Functional:** "Run my options algo on real money across Indian brokers without writing broker-specific code, and never fire a duplicate or unsafe order."
- **Emotional:** "Sleep at night — stop watching the screen — trusting that if something breaks, I'll know immediately and the system will fail safe, not silently."
- **Functional:** "When the broker and I disagree about what happened, reconcile against the broker automatically and recover without manual surgery."
- **Contextual:** "Develop and validate the whole system safely (paper, dry-run, replay) before risking a rupee, then go live with minimum quantity."
- **Social (later):** "When I dispute a fill with my broker, have a clean record of what my system intended and did." *(System-of-record foundation — see Non-Goals for the boundary.)*

### 2.2 Non-Users (v1)

- Traders wanting a no-code / GUI trading platform (this is an imported library).
- Quants wanting signal generation, indicators, or backtesting alpha (this is execution + safety, not a strategy engine).
- Regulated money-managers (PMS/AIF) needing an independently-attestable system of record — a self-hosted, self-signed ledger does not meet that bar (see §5 / §Constraints).
- Non-Indian-market traders in v1 (NSE/BSE only).

### 2.3 Key User Journeys

*Operator/developer is a single role ("Arjun"). Journeys are library-shaped; UJ-2 and UJ-3 are the load-bearing safety moments.*

- **UJ-1. Arjun places a hedged option-sell basket through one broker-neutral call.**
  - **Persona + context:** Arjun, a solo prop trader running NIFTY/BANKNIFTY weekly option-selling on a VPS, wiring his strategy to the library.
  - **Entry state:** Session healthy, safe-start passed, market open.
  - **Path:** Strategy submits a 2-leg basket (buy hedge, sell short) → library validates both legs (lot/tick/freeze/product/risk/hedge) → buys the hedge first → only on hedge confirmation sends the short sell.
  - **Climax:** Both legs return normalized, typed results; the basket is tracked as one logical trade.
  - **Resolution:** Positions and per-strategy P&L update; nothing broker-specific touched his code.
  - **Edge case:** hedge fails → short sell is never sent; Arjun is alerted.

- **UJ-2. A timeout after placement does not become a duplicate order.**
  - **Persona + context:** Same, mid-session, flaky VPS network.
  - **Entry state:** A short-sell order has just been dispatched.
  - **Path:** The `place_order` call times out → library marks the order `UNKNOWN`, persists nothing as "failed," pauses new risky orders → reconciles against order book → trade book → positions.
  - **Climax:** Reconciliation finds the order *did* reach the exchange; it resolves to `OPEN`, not a second order.
  - **Resolution:** One position, not two. Risky orders resume.

- **UJ-3. Cold-start on a new trading day blocks until the world is fresh.**
  - **Persona + context:** Arjun's VPS rebooted overnight; cron restarts the bot at 9:00.
  - **Entry state:** Process up, market not yet open.
  - **Path:** Safe-start gate runs → refreshes the instrument master and trading calendar → verifies session health, clock sanity, egress-IP, config integrity → reconciles prior state.
  - **Climax:** All green → trading is permitted; a stale instrument master would have blocked it loudly.
  - **Resolution:** Arjun gets a "ready / still safe" heartbeat on his phone.

- **UJ-4. A daily-loss breach trips the kill switch and Arjun keeps sleeping.**
  - **Persona + context:** Arjun asleep; a gap move pushes the account past its configured daily loss.
  - **Path:** Risk engine detects the breach → activates the kill switch → blocks new entries, allows exits → fires a push alert.
  - **Climax:** No new risk is taken; Arjun wakes to an alert, not a blown account.
  - **Resolution:** He decides whether to resume; the event is in the audit ledger.

- **UJ-5. Switching brokers is a config change, not a code change.**
  - **Persona + context:** Arjun moves a strategy from Kite to Kotak Neo.
  - **Path:** He flips `broker: kite` → `broker: kotak_neo` in config; capability model confirms the strategy's required features are supported.
  - **Climax:** The same unchanged strategy runs on Kotak Neo.
  - **Edge case:** a required capability the broker lacks is rejected at load, not mid-trade.

## 3. Glossary

The spec glossary at `../../../specs/spec-broker-neutral-execution/glossary.md` is the canonical vocabulary and is adopted verbatim — downstream readers use those terms exactly. The load-bearing terms this PRD relies on: **Order intent**, **Client-side reference**, **Validation gate**, **UNKNOWN state**, **Reconciliation**, **Instrument master**, **Instrument token**, **Lot size / Tick size / Freeze quantity**, **Hedge / Naked option selling**, **Basket / multi-leg trade**, **Tradable price**, **Safe-start gate**, **Write-ahead intent log**, **Kill switch**, **Trading mode**, **Virtual position**, **Net broker position**, **Process-per-account**, **Hash-chained ledger**, **Dead-man's-switch heartbeat**, **Corporate action**. New PRD-level nouns introduced below are added to the spec glossary in the same pass when they arise; none are introduced as synonyms.

## 4. Features

### 4.1 Broker-Neutral Interface & Capability Awareness
**Description:** Strategy code uses one broker-agnostic API for placing/modifying/cancelling/squaring-off orders and reading positions, trades, and funds — never touching broker-specific products, order types, statuses, symbols, segments, errors, or payloads. The library maintains a per-broker capability model and rejects (or applies a configured fallback) before execution when a strategy needs an unsupported capability. Realizes UJ-1, UJ-5. (Backs CAP-1, CAP-2; see `broker-capabilities.md`.)

**Functional Requirements:**

#### FR-1: Broker-neutral order & data API
A strategy can place, modify, cancel, square off, and read orders/positions/trades/funds through one interface that works unchanged across Kite and Kotak Neo. Realizes UJ-1, UJ-5.
**Consequences (testable):**
- The same unchanged strategy module executes against both Kite and Kotak Neo with only configuration changed.
- A scan of strategy code finds zero broker-specific identifiers.

#### FR-2: Capability model & early rejection/fallback
The library exposes each broker's capability set and resolves unsupported-capability requests at request/load time.
**Consequences (testable):**
- Requesting an unsupported capability (e.g., basket margin on a broker lacking it) is blocked at request time with a typed error or a configured fallback, never failing mid-execution.
- Every `unknown` capability in the model is treated as unsupported until verified (adapter certification).

### 4.2 Instrument & Reference-Data Management
**Description:** The library owns the externally-sourced reference data that drifts daily — the instrument master, the trading calendar, and corporate actions — refreshing, caching, and **staleness-gating** each so the system never trades on yesterday's contracts or a holiday. Realizes UJ-3. (Backs CAP-33, CAP-34, CAP-21; see `resilience-and-reconciliation.md`, `operational-modes.md`.)

**Functional Requirements:**

#### FR-3: Instrument-master lifecycle
The library refreshes, caches, and resolves the per-broker, per-segment instrument master (symbol ↔ token; lot/tick/freeze/expiry/strike/type). Realizes UJ-3.
**Consequences (testable):**
- On a new trading day the master is refreshed before the first order; a stale or failed-to-download master blocks trading at safe-start with an alert.
- A requested symbol resolves to the current token + lot/tick/freeze/expiry; an expired/unknown instrument is rejected at validation; a newly listed strike is tradable only after a refresh.

#### FR-4: Trading-calendar source & staleness gate
The library sources, refreshes, caches, and staleness-gates the holiday/special/Muhurat calendar.
**Consequences (testable):**
- A stale or missing calendar blocks trading at safe-start rather than risking orders on a holiday or missed special session.
- New holidays/special sessions take effect only after a refresh.

#### FR-5: Corporate-action awareness
A broker-side position change from a split/bonus/symbol-ISIN change/F&O adjustment is recognized as a corporate action, not a mismatch.
**Consequences (testable):**
- A simulated split/symbol-change on a held instrument is reconciled as a corporate action (position re-based, token re-resolved) without a false position-mismatch alert and without a duplicate corrective order.

### 4.3 Order Validation & Safety Gate
**Description:** No order reaches a broker without passing a mandatory, non-bypassable validation gate. (Backs CAP-3; see `risk-controls.md`.)

**Functional Requirements:**

#### FR-6: Pre-submission validation gate
Every order passes lot-size/freeze, tick-size, product, exchange, funds, risk, trading-window, duplicate, hedge, and kill-switch checks before any broker call.
**Consequences (testable):**
- An order violating any check is rejected before a broker API call occurs, with the failed check named.
- Strategy code has no path to bypass the gate.
- Lot/tick/freeze/expiry values are sourced from the current instrument master (FR-3).

### 4.4 Idempotent Placement & Order Lifecycle
**Description:** Every order intent is uniquely referenced and durably recorded before dispatch; the library owns the full lifecycle and never blindly retries a dangerous operation. Realizes UJ-2. (Backs CAP-4, CAP-5, CAP-14, CAP-26; see `order-lifecycle.md`, `architecture-diagrams.md`.)

**Functional Requirements:**

#### FR-7: Unique client-side reference & idempotency
Every order intent carries a unique client-side reference; a repeated request returns the existing order.
**Consequences (testable):**
- Submitting the same intent twice (including after restart, late response, or duplicate signal) yields exactly one broker order; the duplicate is returned, blocked, or merged per config.

#### FR-8: Write-ahead intent log
The library records "about to send order X with client-ref R" and fsyncs it **before** the broker socket write; it replays the log on boot.
**Consequences (testable):**
- After a `SIGKILL` between the fsync and the socket write, restart replays the intent log, reconciles each entry against the broker, and produces zero duplicate orders.

#### FR-9: Order lifecycle state machine incl. UNKNOWN
The library tracks each order through its lifecycle and treats uncertain responses as `UNKNOWN`, pausing new risky orders until reconciled. Realizes UJ-2.
**Consequences (testable):**
- A simulated timeout after a real placement transitions the order to `UNKNOWN`, blocks new risky orders, and resolves only after checking order book → trade book → positions.

#### FR-10: No blind retry of dangerous operations
Reads may retry; place/modify/cancel/square-off never auto-repeat on timeout — they mark uncertain and reconcile first.
**Consequences (testable):**
- A timeout on a dangerous operation triggers reconcile-then-decide, never an immediate repeat; safe reads retry transparently.

### 4.5 Reconciliation & State Integrity
**Description:** The broker is the source of truth; the library continuously reconciles and recovers, distinguishing real mismatches from corporate actions, and never trades immediately on restart. Realizes UJ-3. (Backs CAP-6, CAP-16, CAP-24; see `resilience-and-reconciliation.md`.)

**Functional Requirements:**

#### FR-11: Continuous reconciliation
Orders, trades, positions, holdings, and funds are reconciled against the broker on all defined triggers (startup, login, reconnect, post-unknown, around square-off, periodic, on WS disconnect, on suspected manual intervention).
**Consequences (testable):**
- On each trigger, local state is rebuilt from broker data; any mismatch raises an alert and optionally blocks new orders per config.

#### FR-12: Manual-intervention detection
A position changed manually in the broker app is detected and reconciled.
**Consequences (testable):**
- A position the bot believes open but the user closed manually is detected on reconcile, and no duplicate exit is sent.

#### FR-13: Funds/margin refresh cadence
Funds/margin are refreshed on a configured intraday cadence and before margin-sensitive orders.
**Consequences (testable):**
- The funds/margin view used for a pre-order margin check is never older than the configured cadence; on stale-and-unrefreshable, the check fails closed (block + alert).

#### FR-14: Crash recovery / reconcile-before-resume
After a crash the library reconciles before resuming and never trades immediately on restart.
**Consequences (testable):**
- After a kill-and-restart with an in-flight order, the library loads state, checks session, fetches order book/trades/positions, resolves unknowns, and resumes only when safe.

### 4.6 Risk Engine
**Description:** Account-, strategy-, instrument-, and order-level risk on every order, plus option-selling and basket safety and pre-trade margin-shock simulation. Realizes UJ-1. (Backs CAP-7, CAP-8, CAP-9, CAP-32; see `risk-controls.md`.)

**Functional Requirements:**

#### FR-15: Four-level risk rules
The risk engine evaluates account/strategy/instrument/order rules on every order.
**Consequences (testable):**
- Each configured limit (daily loss, daily profit lock, max deployed capital, max margin usage, max open positions, max orders/day, max lots, market-order block, slippage cap, illiquid/stale-data block, time windows) demonstrably blocks an offending order in tests.

#### FR-16: Option-selling safety
Short option orders require hedge-first execution and naked-sell prevention, with configured emergency behavior on hedge failure. Realizes UJ-1.
**Consequences (testable):**
- If the hedge fails, the short sell is not sent; if the short sell succeeds but the hedge fails, an immediate alert fires and the configured emergency action runs.

#### FR-17: Basket / multi-leg execution
Multi-leg trades execute as one logical unit with leg dependency, partial-fill handling, and rollback.
**Consequences (testable):**
- In a forced one-leg-fails scenario, dependent legs are not sent and already-executed legs are exited/cancelled or flagged per the configured basket policy.

#### FR-18: Margin/SPAN shock simulation
The library models margin now and under a configured volatility shock against the broker's RMS auto-square-off threshold before submission.
**Consequences (testable):**
- A basket whose projected margin under the configured shock would cross the auto-square-off threshold is flagged/blocked before submission, not discovered mid-position.

### 4.7 Market-Data Reliability
**Description:** Market data is exposed with explicit tradability states; the library detects "connected-but-mute" feeds and monitors its own process health. (Backs CAP-10, CAP-29; see `resilience-and-reconciliation.md`.)

**Functional Requirements:**

#### FR-19: Market-data states & tradable-price validation
Data is exposed as Live/Stale/Disconnected/Delayed/Unknown with auto-reconnect, subscription management, tick de-dup, and configured REST fallback; a strategy can ask "is this price tradable?"
**Consequences (testable):**
- Stale/disconnected data is detected and reported; price-sensitive entries are blocked while data is not Live; REST fallback engages when configured.

#### FR-20: Liveness & process self-health watchdogs
The library detects connected-but-mute feeds and monitors disk/memory/handles, feeding safe degradation.
**Consequences (testable):**
- A still-connected WebSocket with no ticks past threshold forces reconnect-or-degrade; a self-health limit (e.g., disk for WAL/audit) triggers safe degradation + alert before it corrupts logging.

### 4.8 Session, Safe-Start, Time & Rate Limits
**Description:** Sessions are managed and health-checked; a cold-boot gate refuses to trade until the world is verified; time is injectable and skew/stall-detected; requests are throttled with exit priority. Realizes UJ-3. (Backs CAP-11, CAP-28, CAP-27, CAP-12; see `technical-architecture.md` D2/D5/D6/D8.)

**Functional Requirements:**

#### FR-21: Session management & health
Login, token storage/expiry, validation, re-login notification, and health across accounts/brokers; live trading does not start without confirmed session health.
**Consequences (testable):**
- Live mode does not begin until session health passes; token expiry/login/TOTP failure raises a typed alert.

#### FR-22: Safe-start cold-boot gate
Before the first order, verify session, reconciliation, clock sanity, config integrity, fresh instrument master, fresh calendar, and egress-IP allowlist. Realizes UJ-3.
**Consequences (testable):**
- On boot/restart, trading does not begin until every safe-start check passes; a forced egress-IP mismatch, incomplete reconciliation, or stale reference data blocks trading loudly.

#### FR-23: Clock abstraction & skew/stall detection
Time is accessed through an injectable clock; the library detects clock skew and process/event-loop stalls and degrades to exit-only.
**Consequences (testable):**
- Time-dependent behavior is deterministically testable with the injected clock; induced drift/stall beyond threshold forces exit-only + alert.

#### FR-24: Rate-limit protection with exit priority
Requests are throttled/queued under broker-specific limits, prioritizing exits.
**Consequences (testable):**
- Under induced rate pressure, exit orders dispatch ahead of entries and the library slows + alerts rather than letting the broker reject.

### 4.9 Error Classification & Safe Degradation
**Description:** Broker errors are normalized to stable categories with suggested actions; failure scenarios degrade to defined safe postures. (Backs CAP-13, CAP-25; see `error-taxonomy.md`.)

**Functional Requirements:**

#### FR-25: Normalized error taxonomy & suggested action
Broker errors map to stable categories, each carrying a suggested action; strategy code never parses broker error text.
**Consequences (testable):**
- Representative Kite and Kotak Neo errors for the same condition map to the same category + action; strategy code contains no broker error-string parsing.

#### FR-26: Safe degradation postures
On broker outage, stale data, unknown order, position mismatch, risk breach, or session expiry, the library degrades to a defined safe posture.
**Consequences (testable):**
- Each scenario produces its specified behavior (block entries, allow safe exits, alert, reconcile, or kill switch) without duplicate orders or silent failure.

### 4.10 Observability, Audit & Alerting
**Description:** Full per-order provenance, structured logs, a tamper-evident ledger, an EOD reconciliation report, a push heartbeat, and tested alerting. Realizes UJ-4. (Backs CAP-15, CAP-30; see `resilience-and-reconciliation.md`.)

**Functional Requirements:**

#### FR-27: Per-order provenance & reports
Record strategy, broker, account, all timestamps, broker response, status changes, trade execution, risk-check results, reconciliation result, P&L, error reason, and manual overrides; emit structured logs, audit, trade journal, and daily/error/reconciliation reports.
**Consequences (testable):**
- For any executed order, a complete audit record reconstructs the full decision path.

#### FR-28: Alerting with dead-man's-switch
One `AlertSink` with Telegram + generic webhook; a heartbeat whose absence an external watcher alarms on; `send_test_alert()` per channel. Realizes UJ-4.
**Consequences (testable):**
- Each named alert condition (order rejected/unknown, WS disconnected, position mismatch, daily loss breached, kill switch, login failed, square-off failed, margin insufficient, stale data, repeated rejection) fires in test; killing the alerter triggers the absence alarm.

#### FR-29: Tamper-evident ledger, EOD report & position heartbeat
A hash-chained, periodically-signed ledger atop the intent log; an EOD signed reconciliation report; a periodic position/exposure heartbeat.
**Consequences (testable):**
- Any modification to a past ledger entry breaks the hash chain and is detectable; the EOD report states intended/sent/confirmed/reconciled vs broker; the heartbeat reaches the operator.
- The ledger is **never** presented as legal/dispute proof (see §5).

### 4.11 Operational Controls — Modes & Kill Switches
**Description:** Selectable trading modes and five kill-switch types, manually and event-triggered. Realizes UJ-4. (Backs CAP-17, CAP-18; see `operational-modes.md`.)

**Functional Requirements:**

#### FR-30: Trading modes
Live, paper, dry-run, replay, monitor-only, exit-only, and emergency modes are config-selectable.
**Consequences (testable):**
- Each mode exhibits its defined behavior (paper simulates without execution; dry-run validates but never executes; monitor-only places nothing; exit-only blocks entries; emergency allows only cancel/square-off).

#### FR-31: Kill switches
Soft, strategy, broker, account, and panic kill switches, manually and event-triggered. Realizes UJ-4.
**Consequences (testable):**
- Each type produces its specified behavior (soft blocks entries/allows exits; panic cancels open orders, squares off, blocks future entries) when triggered.

### 4.12 Multi-Strategy & Multi-Account
**Description:** Isolated strategies and accounts; one OS process per account bounds blast radius. (Backs CAP-19, CAP-20; see `technical-architecture.md` D3.)

**Functional Requirements:**

#### FR-32: Multi-strategy isolation
Each strategy has isolated risk limits, tags, P&L, and virtual positions; no cross-strategy square-off unless netting is configured; global account risk still enforced.
**Consequences (testable):**
- Two strategies trading the same instrument keep separate virtual positions and P&L; neither squares off the other's position unless netting is enabled; a global account limit still blocks combined exposure.

#### FR-33: Multi-account / process-per-account
One-or-many brokers each with one-or-many accounts, each isolated; one OS process per account with a thin supervisor. `[ASSUMPTION: single-account MVP ships single-process behind a clean account boundary; multi-process supervisor lands with account #2.]`
**Consequences (testable):**
- Two accounts run with independent state; an account-level kill switch affects only its own account; SIGKILL of one account's process leaves the other trading and the supervisor restarts the dead one from its own intent log.

### 4.13 Configuration & Secrets
**Description:** Everything operational is config-driven; secrets never leak. (Backs CAP-22, CAP-23; see `scope-and-phasing.md`.)

**Functional Requirements:**

#### FR-34: Config-driven operation
Brokers, risk limits, hours, allowed instruments/products/order-types, slippage, modes, alerts, storage, credentials source, rate limits, reconciliation interval, and kill-switch behavior are all configurable without code changes.
**Consequences (testable):**
- The same codebase runs in dev, paper, live-small, and live-prod with only configuration changed.

#### FR-35: Secret hygiene
Credentials/tokens come from env or a secrets manager, encrypted at rest, never logged.
**Consequences (testable):**
- No audit/log/error output contains an API key, secret, access token, TOTP secret, password, or MPIN; paper and live credentials are separable.

### 4.14 Operator Surface & Testing Harness
**Description:** A thin operator surface and an adversarial test harness that make the safety promises verifiable. (Backs CAP-31, D8.)

**Functional Requirements:**

#### FR-36: Library + thin CLI + localhost health endpoint
Importable library plus a thin CLI (`status`, `reconcile`, `replay-intent-log`, `safe-start-check`, `send-test-alert`, `verify-ip`, `kill`) and a localhost-only health endpoint (`/healthz`, `/ready`). Read-only except `kill`.
**Consequences (testable):**
- The CLI and endpoint contain no logic of their own (thin shells over the public API); the health endpoint exposes session state, last-heartbeat/tick age, in-flight count, clock sanity, replay-clean flag; the supervisor uses it to detect a wedged process.

#### FR-37: Adversarial fake broker & adapter conformance kit
An in-test fake broker injects faults (delayed/dropped acks, duplicate fills, out-of-order events, 429s, ack-lost-but-placed); a reusable conformance kit certifies every adapter.
**Consequences (testable):**
- UNKNOWN-handling, no-blind-retry, and reconciliation are verified by injecting each fault; a new broker adapter is certified only when the conformance kit passes green.

## 5. Non-Goals (Explicit)

- Not a thin wrapper around `pykiteconnect` / the Kotak Neo SDK; not built on OpenAlgo or any broker-bridge platform (direct adapters only).
- Not a strategy engine — it executes and protects; it does not generate signals, indicators, or alpha.
- Not a backtesting framework (replay mode exists for safety/dev, not as the product's purpose).
- Not a no-code / GUI / black-box trading platform; a monitoring dashboard and Prometheus/Grafana metrics are out of MVP scope.
- Does not promise failures never happen — it promises safe, visible, duplicate-free failure with a recovery path.
- **No third-party-facing "proof" tooling in MVP** (dispute packets, external verifiers, "give-to-your-CA" exports, pricing) — deferred and evidence-gated. The tamper-evident ledger ships as plumbing, not a billed product.
- **Does not claim its self-hosted, self-signed ledger has legal/audit standing** in a broker dispute — it provides operator confidence, not third-party standing.

## 6. MVP Scope

### 6.1 In Scope
Kite + Kotak Neo (direct adapters); broker-neutral order/status/positions/trades/margins; capability model; validation gate; four-level risk checks; idempotency + write-ahead intent log; no-blind-retry; order + position + trade reconciliation; instrument-master daily refresh + symbol/token resolution + staleness gating; trading-calendar source + staleness gating; funds/margin intraday refresh cadence; clock abstraction + stall detection; safe-start cold-boot gate; rate-limit protection; normalized error taxonomy; audit logs; Telegram + webhook alerts + dead-man's-switch heartbeat; kill switches; paper + dry-run modes; config-driven broker selection; thin CLI + localhost health endpoint; Python 3.11+ packaging; adversarial fake broker + adapter conformance kit (the SM-1 verification substrate — promoted to Day-One per adversarial-review finding COH-4).

### 6.2 Out of Scope for MVP
- WebSocket market data + order-update streaming, stale-data detection refinements, basket execution, hedge-first, margin/SPAN shock sim, strategy-wise P&L/positions, expiry-day rules, auto square-off, corporate-action awareness, crash recovery polish, multi-account supervisor, tamper-evident ledger + EOD report + heartbeat — **"Should have soon"** (fast-follow). *(The fake broker + conformance kit moved to §6.1 In Scope — it is the SM-1 verification substrate, per adversarial-review finding COH-4.)* `[NOTE FOR PM: hedge-first + basket are emotionally load-bearing for the option-seller JTBD — revisit pulling them earlier if timeline permits.]`
- Broker fallback, smart order routing, historical-data/option-chain abstraction, Greeks, replay polish, dashboard, Prometheus/Grafana, cloud templates, third-party proof tooling — **"Later"**.

## 7. Success Metrics

**Primary**
- **SM-1: Zero duplicate orders under fault injection.** Across the full fake-broker fault matrix (timeout-after-place, ack-lost-but-placed, restart mid-flight), duplicate-order count = 0. Validates FR-7, FR-8, FR-9, FR-10, FR-14, FR-37.
- **SM-2: Production checklist pass rate = 100%.** All acceptance-criteria items in `scope-and-phasing.md` pass before any real-money use. Validates the whole spec.
- **SM-3: Broker-portability.** An unchanged option-selling strategy runs on both Kite and Kotak Neo with only config changed. Validates FR-1, FR-2.

**Secondary**
- **SM-4: Safe-start correctness.** 100% of cold-boots block trading until session, reconciliation, clock, instrument master, and calendar are verified fresh. Validates FR-22, FR-3, FR-4.
- **SM-5: Alert reliability.** Every named alert condition fires in test, and a killed alerter triggers the dead-man's-switch absence alarm. Validates FR-28.
- **SM-6: Paper-mode soak.** ≥20 paper-mode sessions with no unhandled error and complete audit records. Validates FR-27, FR-30.

**Counter-metrics (do not optimize)**
- **SM-C1: Do not minimize order latency at the cost of the safety gate.** Validation + intent-log fsync add latency by design; counterbalances any pressure from SM-1's "fast recovery." Never bypass the gate or the fsync to shave milliseconds.
- **SM-C2: Do not suppress alerts to improve a "quiet dashboard" metric.** A noisy-but-correct alert stream beats a silent one; counterbalances SM-5 being gamed by under-alerting.

## 8. Open Questions

1. **Product positioning & distribution** — open-core on public PyPI (core free, signed-reconciliation as a later paid tier) vs private/self-infrastructure? Needs a market read, not more analysis. `[ASSUMPTION: public-PyPI open-core is the working assumption; kept viable by the no-AGPL/direct-adapters choice.]`
2. **Target segment for the "never lose the broker argument" wedge** — is the prop/PMS segment worth pursuing and large enough to anchor positioning? Gated on usage telemetry (do operators export the reconciliation record after incidents?).
3. **Independent anchoring for proof** — if the system-of-record product is ever pursued, what third-party timestamping/notarization gives the ledger standing the audited party doesn't control?
4. **Replay-mode data source format** — confirm replay consumes the library's own recorded intent/event logs vs an external feed. `[ASSUMPTION: own recorded logs.]`
5. **Static-IP/regulatory responsibility boundary** — confirmed as "enforce the operator's declared egress IP at safe-start; document the regulatory obligation; never claim to enforce the regulation." Flag if a stricter posture is required.

## 9. Assumptions Index

- §4.12 FR-33 — single-account MVP ships single-process behind a clean account boundary; multi-process supervisor lands with account #2.
- §8 Q1 — public-PyPI open-core is the working distribution assumption.
- §8 Q4 — replay mode consumes the library's own recorded logs.
- §Constraints / Runtime — **C++20** (CMake + Conan; see architecture.md "Technology Stack Revision — Full Native C++"); one OS process per account on a single VPS baseline; SQLite-projection + fsync'd intent log persistence; synchronous safety core with threads at I/O edges (D1–D3 carried as foundational; D4/D5/D7 superseded by the C++ revision).
- §Cross-Cutting NFRs — quantitative latency/throughput budgets below are `[ASSUMPTION]` starting targets to confirm during architecture.

---

## Cross-Cutting NFRs

- **Durability:** an order's intent is on disk (fsync'd) before its socket write; no order is dispatched whose intent was not first recorded.
- **Correctness over speed:** the order-decision path is synchronous, single-threaded, and deterministic under an injected clock; async is confined to a single adapter if ever needed and never reaches the safety core.
- **Reliability:** zero duplicate orders is a hard invariant, not a target; uncertain state always resolves toward broker truth.
- **Persisted-state schema versioning:** every persisted record is version-stamped; the library refuses to start on an unrecognized schema version (fail visibly) rather than risk a misread that causes a duplicate order. `[ASSUMPTION: explicit forward migrations can follow MVP; the version stamp cannot.]`
- **Performance budgets `[ASSUMPTION — confirm in architecture]`:** validation-gate + intent-log fsync overhead per order < ~50 ms on the target VPS; reconciliation of a full order/trade/position set < ~5 s; market-data tick-to-tradable-state evaluation < ~100 ms.
- **Observability:** the emitted event schema is treated as a versioned public contract (dashboards/alerts bind to it); field renames are breaking changes.
- **Footprint:** baseline runs on a single modest VPS; one process per account, no external database daemon at MVP.

## Constraints and Guardrails

- **Safety:** broker is the source of truth; never blind-retry dangerous ops; UNKNOWN blocks new risky orders; live trading and post-restart trading require the safe-start gate; under rate-limit pressure exits beat entries; no trading on stale reference data.
- **Security:** secrets from env/secrets-manager, encrypted at rest, never in logs/errors; health endpoint bound to localhost only; operator surface read-only except `kill`.
- **Compliance (surface, don't claim):** enforce the operator's declared egress IP at safe-start and document static-IP/SEBI obligations; the SEBI retail-algo framework is a tailwind for *auditable execution* but accountability sits with broker/registered provider — the library does not claim to discharge it.

## Why Now

Indian retail/prop algo trading is growing on paid broker APIs (Kite Connect) with a thriving bridge ecosystem (OpenAlgo et al.) that solves *connectivity* but not *safety*. SEBI's retail-algo framework (2024–25) raises the bar on auditable, broker-routed execution. The unmet need — a production-grade execution-safety core that prevents duplicate/unsafe orders and recovers cleanly — is exactly the gap competitors leave open.

## Developer-Product Surface

- **Public API surface:** the broker-neutral order/data interface, typed result objects (never raw broker dicts), the config schema, the CLI verbs, and the emitted event schema. These are the stable contract.
- **Versioning & deprecation:** semantic versioning; the persisted-state schema and event schema are versioned independently and never broken silently. `[ASSUMPTION: SemVer with a documented deprecation window — confirm policy.]`
- **Runtime targets & dependencies:** **C++20**, CMake ≥ 3.28 + Conan v2 (binary cache); `include/` + `src/` layout; broker adapters as separate CMake targets implementing the Kite/Kotak REST+WS protocol (no official C++ SDK); CI matrix gcc+clang Debug/Release + ASan/UBSan/TSan. Core libs: cpr/libcurl, IXWebSocket, nlohmann/json, toml++, spdlog/fmt, OpenSSL+libsodium, SQLite C API, CLI11, cpp-httplib, Catch2.

## Risk and Mitigations

| Risk | Mitigation |
|---|---|
| A subtle bug in the safety core causes a real-money duplicate/unsafe order | Synchronous deterministic core; fsync-before-send; fake-broker fault matrix (FR-37); zero-duplicate as a tested invariant (SM-1). |
| Broker SDK changes behavior silently (rejection codes, partial-fill semantics) | Adapter conformance kit (FR-37); `unknown` capabilities treated as unsupported until verified. |
| Trading on stale reference data (instruments/calendar) | Daily refresh + safe-start staleness gate (FR-3, FR-4, FR-22). |
| Operator over-trusts the ledger as legal proof | Explicit non-goal + constraint; ledger never presented as dispute-grade. |
| Scope creep (more brokers, GUI, routing) dilutes the safety MVP | Hard non-goals (§5); "earn the second broker by surviving the first two." |
| AGPL/licensing contamination from a connectivity platform | Direct adapters only; no OpenAlgo runtime dependency. |
