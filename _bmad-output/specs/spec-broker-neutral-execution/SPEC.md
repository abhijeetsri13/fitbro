---
id: SPEC-broker-neutral-execution
title: Broker-Neutral Trading Execution Library
status: draft
created: 2026-06-16
updated: 2026-06-17
companions:
  - glossary.md
  - broker-capabilities.md
  - order-lifecycle.md
  - risk-controls.md
  - error-taxonomy.md
  - resilience-and-reconciliation.md
  - operational-modes.md
  - scope-and-phasing.md
  - architecture-diagrams.md
  - technical-architecture.md
sources:
  - "(in-chat) Broker-Neutral Trading Execution Library Specification — 30-section requirement supplied 2026-06-16"
---

> **Canonical contract.** This SPEC and the files in `companions:` are the complete, preservation-validated contract for what to build, test, and validate. The source requirement listed in frontmatter is for traceability only — consult it for narrative rationale this contract intentionally omits.

# Broker-Neutral Trading Execution Library

## Why

A **vision to realize**, driven by a **pain to solve**: algorithmic traders on Indian broker APIs (Zerodha Kite, Kotak Neo) repeatedly suffer the same production failures — duplicate orders from blind retries, silent failures swallowed by thin SDK wrappers, unknown order states after timeouts, position drift after manual intervention, naked short option legs after a failed hedge, and broker-specific quirks leaking into strategy code so a strategy cannot move brokers. Official SDKs give API access but solve none of this. The force behind this work is to build the **execution, safety, and broker-abstraction layer that sits between strategy code and broker APIs** — the layer the SDKs leave out. The promise is not "nothing fails"; broker APIs, sessions, WebSockets, exchanges, and VPS infrastructure all fail. The promise is: **when something fails, it fails visibly, safely, without duplicate orders, and with a clear recovery path.** This matters now because real money is at stake and a weak abstraction is more dangerous than none.

## Capabilities

- id: CAP-1
  intent: Strategy code places, modifies, cancels, and squares off orders and reads positions, trades, and funds through one broker-agnostic interface, never referencing broker-specific product names, order types, statuses, symbol formats, segment codes, errors, or WebSocket payloads.
  success: The same unchanged strategy module executes against both Kite and Kotak Neo with only configuration changed; a scan of strategy code finds zero broker-specific identifiers.

- id: CAP-2
  intent: The library exposes a per-broker capability model and, before execution, rejects (or applies a configured fallback) when a strategy requires a capability the selected broker does not support. (See `broker-capabilities.md`.)
  success: Requesting an unsupported capability (e.g., basket margin on a broker lacking it) is blocked at request time with a typed error or a configured fallback, never failing mid-execution.

- id: CAP-3
  intent: Every order passes a validation gate — lot-size/freeze quantity (an over-freeze quantity is **sliced** into child orders by default, not rejected), tick size, product, exchange, funds, risk, trading-window, duplicate, hedge, kill-switch, and **UNKNOWN-pause** checks — before any broker call is made. (See `risk-controls.md`.)
  success: An order violating any check is rejected before a broker API call occurs, the rejection names the failed check, and strategy code has no path to bypass the gate; an entry submitted while an UNKNOWN-pause is active is blocked, while a risk-reducing exit is not.

- id: CAP-4
  intent: Every order intent carries a unique client-side reference so that a repeated request for the same intent returns the existing order rather than creating a duplicate. (See `order-lifecycle.md`.)
  success: Submitting the same intent twice — including after a process restart, a late broker response, or a duplicate strategy signal — yields exactly one broker order; the duplicate is returned, blocked, or merged per configuration.

- id: CAP-5
  intent: The library owns each order through a defined lifecycle state machine and treats any uncertain broker response as `UNKNOWN` until reconciled, pausing new risky orders — scoped to the whole **account process**, but **exempting risk-reducing operations** (square-off, cancel, hedge-completion, configured emergency hedge/exit) — while any order is `UNKNOWN`. (See `order-lifecycle.md`.)
  success: A simulated API timeout after a real placement transitions the order to `UNKNOWN`, blocks new risky entries process-wide, still permits a square-off/cancel, and resolves the state only after checking order book, trade book, and positions.

- id: CAP-6
  intent: The library continuously reconciles orders, trades, positions, holdings, and funds against the broker as the source of truth, on every defined trigger (startup, login, reconnect, post-unknown, around square-off, periodic, on WebSocket disconnect, on suspected manual intervention). Funds and margin are additionally refreshed on a configured intraday cadence and before margin-sensitive orders, so a margin check never runs against a view older than that cadence. (See `resilience-and-reconciliation.md`.)
  success: On each trigger, local state is rebuilt from broker data; any mismatch raises an alert and optionally blocks new orders per configuration; the funds/margin view used for a pre-order margin check is never older than the configured refresh cadence.

- id: CAP-7
  intent: A risk engine evaluates account-, strategy-, instrument-, and order-level rules on every order. (See `risk-controls.md`.)
  success: Each configured limit (daily loss, daily profit lock, max deployed capital, max margin usage, max open positions, max orders/day, max lots, market-order block, slippage cap, illiquid/stale-data block, time windows, etc.) demonstrably blocks an offending order in tests.

- id: CAP-8
  intent: Short option orders are protected by hedge-first execution and naked-sell prevention, with configured emergency behavior when a hedge fails. (See `risk-controls.md`.)
  success: If the hedge leg fails, the short sell is not sent; if the short sell succeeds but the hedge fails, an immediate alert fires and the configured emergency action (emergency hedge or emergency exit) runs.

- id: CAP-9
  intent: Multi-leg trades are executed as one logical unit honoring leg dependency, basket-margin validation where supported, partial-fill handling, and rollback (cancel/exit) of executed legs when a prerequisite leg fails. (See `risk-controls.md`, `architecture-diagrams.md`.)
  success: In a forced one-leg-fails scenario, dependent legs are not sent and already-executed legs are exited/cancelled or flagged per the configured basket policy; multi-leg option trades are never treated as unrelated single orders unless explicitly configured.

- id: CAP-10
  intent: Market data is exposed with explicit states (Live, Stale, Disconnected, Delayed, Unknown) so a strategy can ask "is this price tradable?" — not only "what is the LTP?" — with auto-reconnect, subscription management, tick de-duplication, and configured REST fallback. (See `resilience-and-reconciliation.md`.)
  success: Stale or disconnected data is detected and reported; price-sensitive entries are blocked while data is not Live; REST quote fallback engages when configured; affected strategies pause on data mismatch.

- id: CAP-11
  intent: The library manages broker sessions — daily session **establishment** (distinct from expiry-detection: brokers like Kite mint a token once per day via interactive login + TOTP with **no headless refresh**, so establishment runs as a first-class adapter step, defaulting to an operator-supplied request-token/token-of-day at boot), login, token storage, expiry detection, validation, re-login notification, and health checks — across multiple accounts and brokers, and refuses to start live trading without a freshly established, healthy session. (See `resilience-and-reconciliation.md`.)
  success: Establishment runs before the safe-start gate; a dead daily token blocks trading until re-established (never silently "refreshed" when the broker has no programmatic refresh); token expiry, login failure, TOTP failure, or an invalid session each raise a typed alert.

- id: CAP-12
  intent: The library throttles and queues requests under broker-specific rate limits (per-second, per-minute, daily, modifications-per-order, quote, historical), prioritizing exit orders over entries and preserving exit capability when throttled. (See `resilience-and-reconciliation.md`.)
  success: Under induced rate pressure, exit orders dispatch ahead of new entries and the library slows and alerts rather than letting the broker reject requests.

- id: CAP-13
  intent: Broker errors are normalized into stable categories, each carrying a suggested action, so strategy code never parses broker-specific error text. (See `error-taxonomy.md`.)
  success: Representative Kite and Kotak Neo error responses for the same condition map to the same category and suggested action; strategy code contains no broker error-string parsing.

- id: CAP-14
  intent: The library distinguishes safe-to-retry reads from dangerous operations (place, modify, cancel, square off) and never auto-repeats a dangerous operation on timeout — it marks state uncertain and reconciles first. (See `error-taxonomy.md`, `order-lifecycle.md`.)
  success: A timeout on a dangerous operation triggers reconcile-then-decide and never an immediate repeat; safe read operations retry transparently.

- id: CAP-15
  intent: The library records full per-order provenance (initiating strategy, broker, account, signal/validation/sent timestamps, broker response, status changes, trade execution, risk-check results, reconciliation result, final P&L, error reason, manual override) and emits structured logs, an audit trail, a trade journal, daily/error/reconciliation reports, health checks, and real-time alerts. (See `resilience-and-reconciliation.md`.)
  success: For any executed order, a complete audit record reconstructs the full decision path; each named alert condition (order rejected, order unknown, WebSocket disconnected, position mismatch, daily loss breached, kill switch activated, login failed, square-off failed, margin insufficient, stale data, repeated rejection) fires in test.

- id: CAP-16
  intent: The library detects orders or positions changed manually from a broker app and reconciles internal state instead of acting on a stale assumption.
  success: A position the bot believes is open but the user closed manually in the broker app is detected on reconcile, and no duplicate exit order is sent.

- id: CAP-17
  intent: Soft, strategy, broker, account, and panic kill switches are supported, triggerable both manually and by defined risk events. (See `operational-modes.md`.)
  success: Each kill-switch type produces its specified behavior when triggered (e.g., soft blocks entries while allowing exits; panic cancels open orders, squares off, and blocks future entries).

- id: CAP-18
  intent: Live, paper, dry-run, replay, monitor-only, exit-only, and emergency modes are selectable by configuration. (See `operational-modes.md`.)
  success: Each mode exhibits its defined behavior — e.g., paper simulates without broker execution, dry-run validates but never executes, monitor-only places no new orders, exit-only blocks entries, emergency allows only cancel and square-off.

- id: CAP-19
  intent: Each strategy has isolated risk limits, order tags, P&L, and virtual positions; strategies do not close each other's positions unless netting is explicitly configured, while global account risk is still enforced; both net-broker and strategy-wise position views are available.
  success: Two strategies trading the same instrument keep separate virtual positions and P&L and neither squares off the other's position unless netting is explicitly enabled, yet a global account limit still blocks the combined exposure.

- id: CAP-20
  intent: The library supports one-or-many brokers each with one-or-many accounts, each account isolated in credentials, risk limits, positions, orders, funds, logs, and kill switch.
  success: Two accounts run concurrently with independent state, and an account-level kill switch affects only its own account.

- id: CAP-21
  intent: The library enforces Indian market time awareness — sessions (pre-open, normal, special, Muhurat), holidays, expiry-day rules, entry cut-off, square-off windows, and strategy-specific allowed windows. The trading calendar (holidays, special/Muhurat sessions) is sourced from a configured provider, refreshed, cached, and staleness-gated like the instrument master (CAP-33). (See `operational-modes.md`.)
  success: New intraday entries after the configured cut-off are blocked, and order placement to a closed exchange is blocked unless AMO is explicitly configured; a stale or missing trading calendar blocks trading at safe-start rather than risking orders on a holiday or a missed special session.

- id: CAP-22
  intent: Brokers (enabled, default, fallback), risk limits, trading hours, allowed instruments/products/order-types, slippage limits, market-order permissions, alert channels, storage mode, paper/live mode, credentials source, rate limits, reconciliation interval, and kill-switch behavior are all configurable without code changes. (See `scope-and-phasing.md`.)
  success: The same codebase runs in development, paper, live-small-capital, and live-production with only configuration changed.

- id: CAP-23
  intent: Credentials and tokens are sourced from environment or a secrets manager and encrypted at rest, and secrets never appear in logs, error messages, **or the persisted intent log / audit ledger** — redaction is bound to the persistence path, not only the logging path, and session/auth responses are excluded from the inspectable hash chain entirely.
  success: Under test, no log, error, audit, **intent-log, or ledger** output contains an API key, API secret, access token, TOTP secret, password, or MPIN — including secrets carried in SDK exception text/tracebacks or raw broker session responses; paper and live credentials are separable.

- id: CAP-24
  intent: All critical trading state (order intents, broker order IDs, state changes, trades, position snapshots, risk events, broker responses, error events, health events) is persisted so that after a crash the library reconciles before resuming and never begins trading immediately on restart. (See `resilience-and-reconciliation.md`.)
  success: After a kill-and-restart with an in-flight order, the library loads last-known state, checks session, fetches broker order book/trades/positions, resolves unknown orders, and resumes only when safe.

- id: CAP-25
  intent: On broker API outage, stale market data, unknown order state, position mismatch, risk-limit breach, or session expiry, the library degrades to a defined safe posture rather than failing silently. (See `resilience-and-reconciliation.md`.)
  success: Each failure scenario produces its specified behavior (block entries, allow safe exits where possible, alert, reconcile, or activate kill switch) without duplicate orders or silent failure.

- id: CAP-26
  intent: The library writes a durable, append-only **write-ahead intent log** — recording "about to send order X with client-ref R" and fsyncing it **before** the broker socket write — and replays it on boot to enumerate every order it might have sent. (See `technical-architecture.md` D1.)
  success: After a `SIGKILL` between the fsync and the socket write, restart replays the intent log, reconciles each entry against the broker, and produces zero duplicate orders.

- id: CAP-27
  intent: Time is accessed through an injectable clock abstraction, and the library detects clock skew (vs a trusted reference) and event-loop/process stalls, degrading to exit-only when either exceeds threshold. (See `technical-architecture.md` D2.)
  success: With the clock injected, time-dependent behavior (rate windows, session/expiry checks, backoff) is deterministically testable; an induced wall-clock drift or process stall beyond threshold forces exit-only and an alert.

- id: CAP-28
  intent: Before placing the first order, a **safe-start cold-boot gate** verifies session health, reconciliation completeness, clock sanity, config integrity, a **fresh instrument master** (CAP-33), and the egress-IP allowlist — and refuses to trade (fail closed) if any check fails. (See `technical-architecture.md` D6, D8.)
  success: On a fresh boot or VPS restart, trading does not begin until every safe-start check passes; a forced egress-IP mismatch, incomplete reconciliation, or stale instrument master blocks trading loudly.

- id: CAP-29
  intent: The library runs liveness watchdogs on every live feed ("connected-but-mute" detection — a still-open WebSocket with no ticks past threshold) and on its own process health (disk, memory, handles, log rotation), feeding the safe-degradation posture. (See `resilience-and-reconciliation.md`, `technical-architecture.md` D5.)
  success: A WebSocket that stays connected but stops delivering ticks is detected and forces reconnect-or-degrade; a self-health limit (e.g., disk for audit/WAL) triggers safe degradation and an alert before it corrupts logging.

- id: CAP-30
  intent: The audit/reconciliation record is a **tamper-evident, hash-chained, periodically-signed** ledger built on the intent log (CAP-26), and the library produces an end-of-session signed reconciliation report and a push position/exposure heartbeat. (See `resilience-and-reconciliation.md`, `scope-and-phasing.md`.)
  success: A modification by any party **without the chain-signing key** (a separate process, a log-shipping pipeline, an operator without key access) — or accidental corruption — breaks the hash chain and is detectable; modification by the **key-holder is not detectable from the ledger alone** (that requires the independent third-party anchoring tracked as Open Question 3). An EOD signed report states what the library intended/sent/confirmed/reconciled vs the broker; a periodic heartbeat confirms "still safe" to the operator's phone. (This is a *system-of-record foundation*, not a claim of legal/dispute standing — see Non-goals.)

- id: CAP-31
  intent: The library ships an adversarial **fake broker** for fault injection (delayed/dropped acks, duplicate fills, out-of-order events, 429s, ack-lost-but-placed) and a reusable **broker-adapter conformance test kit** every new adapter must pass. (See `technical-architecture.md` D2.)
  success: The UNKNOWN-state machine, no-blind-retry, and reconciliation are verified by injecting each fault via the fake broker; a new broker adapter is certified only when the conformance kit passes green.

- id: CAP-32
  intent: For option selling, the library performs **pre-trade margin/SPAN shock simulation** — modeling margin consumption now and under a configured volatility shock, against the broker's RMS auto-square-off threshold. (See `risk-controls.md`.)
  success: A basket whose projected margin under the configured shock would cross the broker's auto-square-off threshold is flagged/blocked before submission, not discovered mid-position.

- id: CAP-33
  intent: The library owns the **instrument-master lifecycle** per broker and exchange segment — scheduled refresh (at least daily, before the first trade) of the Kite instrument dump / Kotak Neo scrip master, a local cache that survives restart, broker-neutral **symbol ↔ instrument-token resolution**, and the derived contract metadata (lot size, tick size, freeze quantity, expiry, strike, instrument type) the validation gate and instrument-level risk depend on. (See `resilience-and-reconciliation.md`, `risk-controls.md`.)
  success: On a new trading day the master is refreshed before the first order; a stale (not current) or failed-to-download master blocks trading at safe-start with an alert; a requested symbol resolves to the current token plus lot/tick/freeze/expiry, while an expired or unknown instrument is rejected at the validation gate; a newly listed strike/expiry becomes tradable only after it appears in a refreshed master.

- id: CAP-34
  intent: The library is aware of corporate actions (stock splits, bonuses, symbol/ISIN changes, F&O contract adjustments) so that a broker-side change to a held position's quantity, average price, or symbol is recognized as a corporate action — not misread as a position mismatch (CAP-6) or manual intervention (CAP-16) — and the instrument-master token mapping (CAP-33) is updated accordingly. (See `resilience-and-reconciliation.md`.)
  success: A simulated split or symbol change on a held instrument is reconciled as a corporate action (position re-based, token re-resolved) without raising a false position-mismatch alert and without emitting a duplicate corrective order.

## Constraints

- **Broker is the source of truth; local state is a working copy.** Every divergence resolves toward broker data, never the reverse.
- **Never blindly retry a dangerous operation.** `place_order`, `modify_order`, `cancel_order`, and `square_off` are never auto-repeated on timeout — mark uncertain, reconcile, then decide.
- **No order reaches a broker without passing the validation and risk gate.** The gate is not bypassable from strategy code.
- **Every order intent has a unique client-side reference and is persisted before or during execution.** No order is executed that was not first recorded.
- **An `UNKNOWN` order state blocks new risky orders for the whole account process** until reconciled via order book, trade book, and positions — but **never blocks a risk-reducing exit** (square-off, cancel, hedge-completion, configured emergency hedge/exit).
- **No broker-specific detail leaks into strategy code** — products, order types, statuses, symbols, segments, errors, and payloads are all normalized below the strategy boundary.
- **Live trading must not start** without confirmed broker session health, and after a restart not before reconciliation completes.
- **Under rate-limit pressure, exit orders take priority over entry orders.**
- **Secrets are never written to logs or error messages.**
- **Initial scope is C++ (C++20), Indian markets (NSE/BSE), brokers Zerodha Kite and Kotak Neo**, with new brokers added via capability-based adapters without changing strategy logic.
- **There is no official C++ broker SDK** (Kite/Kotak ship Python-only SDKs), so each adapter **implements the documented Kite Connect / Kotak Neo REST + WebSocket protocol itself**, behind the adapter port — broker specifics are never exposed to strategy code, and no OpenAlgo or other broker-bridge platform is a runtime dependency (`technical-architecture.md` D7, architecture.md "Technology Stack Revision — Full Native C++").
- **The intent log is fsynced before the broker socket write** — no order is dispatched whose intent is not already durable on disk (`technical-architecture.md` D1).
- **The order-decision path is synchronous and single-threaded**; concurrency is confined to I/O edges. Async, if ever needed, is isolated to a single adapter and never reaches the safety core (`technical-architecture.md` D2).
- **One OS process per account** — each owns its own intent log, store, and kill switch, bounding kill-switch blast radius to exactly one account (`technical-architecture.md` D3).
- **Live trading must not start until the safe-start gate passes** (session, reconciliation, clock sanity, config, egress-IP) — extends the session-health constraint above.
- **The audit ledger is tamper-evident (hash-chained, signed) but is never represented as legal or dispute-grade proof** — it provides operator confidence, not third-party standing.
- **No instrument is traded on a stale or unresolved instrument master.** The master is refreshed before each trading day, gates safe-start, and is the single source of lot size, tick size, freeze quantity, and expiry used by the validation gate (CAP-33).

## Non-goals

- Not a thin wrapper around a broker SDK — in C++ there is no official SDK; the library implements the documented Kite Connect / Kotak Neo REST + WebSocket protocol itself, behind the adapter port (and still adds the full safety layer on top).
- Not a strategy engine — it executes and protects; it does not generate signals.
- Not a backtesting-only framework (replay mode exists for safety/dev, not as the product's purpose).
- Not a no-code or black-box algo trading platform.
- Does not promise that failures will never happen; it promises safe, visible, duplicate-free failure with a recovery path.
- A monitoring dashboard and Prometheus/Grafana metrics are explicitly out of MVP scope (see `scope-and-phasing.md` — "Later").
- **Not built on OpenAlgo or any broker-bridge platform.** OpenAlgo is prior-art (a self-hosted AGPL connectivity platform), studied but not depended upon (`technical-architecture.md` D7).
- **No third-party-facing "proof" tooling in MVP** — dispute packets, externally-runnable verifiers, "hand-this-to-your-CA" exports, and any pricing of the ledger are out of scope until (a) usage telemetry shows operators actually export the record after incidents *and* (b) independent third-party anchoring (notarization/timestamping) is solved. The tamper-evident ledger (CAP-30) ships as plumbing, not as a billed product.
- **Does not claim its self-hosted, self-signed ledger has legal/audit standing in a broker dispute** — the exchange timestamp governs; the ledger sells confidence, not standing.

## Success signal

A trader runs an unchanged option-selling strategy against Kite, flips one config value to Kotak Neo, and it runs — and when a broker API call times out after the order actually reached the exchange, the library marks the order `UNKNOWN`, reconciles against the broker, and does **not** fire a second order: one position, not two. The library is considered production-ready when it satisfies the full acceptance criteria and production checklist in `scope-and-phasing.md` — including idempotency, unknown-order handling, order/trade/position reconciliation, manual-intervention detection, kill switch, rate-limit enforcement, market-data staleness detection, session-expiry detection, secret hygiene, restart recovery, paper mode tested for at least 20 sessions, and live mode tested with minimum quantity only.

## Assumptions

- The library is a **C++20 library** (CMake + Conan) linked by strategy code in-process; the operator surface is the library + a thin CLI + a localhost health endpoint (architecture.md "Technology Stack Revision — Full Native C++", `technical-architecture.md` D8).
- Markets are **Indian (NSE/BSE)** — implied by lot/freeze/tick rules, Muhurat trading, and the named brokers.
- **Replay mode** consumes the library's own recorded intent log / event logs (CAP-26) rather than a third-party historical feed.
- **GTT, AMO, Cover, and Bracket orders** are capability-modeled (CAP-2) but not MVP "must-have" execution paths; they are awareness/normalization targets first (see `scope-and-phasing.md`).
- **Runtime baseline is one OS process per account** on a single VPS, managed by a thin supervisor (`technical-architecture.md` D3); multi-VPS/cloud templates are a "Later" concern.
- **Resolved architecture decisions** (persistence, concurrency, multi-account, packaging, alerting, static-IP, OpenAlgo stance, operator surface) are captured in `technical-architecture.md` D1–D8 and treated as foundational for the architecture phase.
- **Product posture is staged:** build the reliability library now ("run a real-money algo and sleep at night"); the tamper-evident ledger (CAP-30) ships as a *system-of-record foundation*, with the "execution system of record" product deferred and evidence-gated (see `scope-and-phasing.md`).

## Open Questions

- **Product positioning & distribution:** open-core on **public PyPI** (core free to compete with OpenAlgo for the same community, signed-reconciliation as a later paid tier) vs **private/self-infrastructure**? The Direct-adapters/no-AGPL choice (D7) keeps open-core viable; the call needs a market read, not more analysis.
- **Target segment for the "never lose the broker argument" wedge:** is the prop-desk / small-fund / PMS segment worth pursuing, and is it large enough to anchor positioning? Gated on real sizing and on usage telemetry (do operators export the reconciliation record after incidents?).
- **Independent anchoring for proof:** if the system-of-record product is ever pursued, what third-party timestamping/notarization (or broker/vendor partnership) gives the ledger standing the audited party doesn't control?
