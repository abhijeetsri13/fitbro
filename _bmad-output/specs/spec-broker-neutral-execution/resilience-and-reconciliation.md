# Resilience, Reconciliation, Market Data, Sessions, Rate Limits, Recovery & Observability

Backs **CAP-6, CAP-10, CAP-11, CAP-12, CAP-15, CAP-24, CAP-25**. The operational-resilience contract: how the library stays correct across reconnects, failures, and restarts, and what it must observe and persist.

## Reconciliation (CAP-6)

The broker is the source of truth; local state is a working copy. Reconcile these continuously:

| Data | Purpose |
|---|---|
| Orders | Confirm accepted / rejected / open / filled / cancelled state. |
| Trades | Confirm actual executions. |
| Positions | Detect mismatches and manual intervention. |
| Holdings | Track delivery/cash positions. |
| Funds & margin | Avoid rejected orders and margin surprises. |
| Open orders | Manage cancellations and exits safely. |

**Reconciliation triggers:** on startup; after login; after reconnect; after any unknown order; before square-off; after square-off; periodically during market hours; whenever the WebSocket disconnects; whenever manual intervention is suspected.

On mismatch: alert the user and optionally block new orders. Manual-intervention detection (CAP-16): a position the bot believes open but the user closed manually is detected here, and no duplicate exit is sent.

**Cadence is capability-driven and adaptive (IBR-5).** Where the broker exposes an order-update push (postback / order-update WebSocket), that push is the primary fill / state-change / UNKNOWN-resolution trigger, with the periodic poll as backstop; where it is unsupported/unknown (treated as unsupported — notably Kotak Neo), **polling is the only path**, so the periodic cadence is the latency floor for resolving an UNKNOWN (which blocks all new risky entries) and detecting a fill that needs an exit. The "reconciliation interval" (CAP-22) is therefore **adaptive**: tight (~1–2 s) whenever any order is `SENT`/`UNKNOWN` or any position is open; loose (~15–30 s) when flat. Reconciliation reads draw reserved rate-limit budget **above** new entries but **below** exits, so aggressive polling neither starves exits nor is starved by entry traffic.

**Execution model (COH-1/CC-4).** Reconciliation is *fetch-off-loop, apply-on-loop*: the scheduler thread performs broker reads only and enqueues an immutable result; the synchronous main loop applies every diff (state transitions, UNKNOWN→RECONCILED, corporate-action re-base, ledger writes). No `store`/`ledger` write occurs outside the main loop.

## Market data reliability (CAP-10)

Both REST quotes and WebSocket streaming are supported; WebSocket data is not assumed correct.

| Pain point | Behavior |
|---|---|
| WebSocket disconnects | **Auth-aware reconnect** — distinguish a transport failure (bounded-backoff retry, capped attempts) from a token/auth failure; after N failed attempts, call session validation (CAP-11) instead of re-looping. On an invalid session, **stop socket retries**, raise "broker session invalid" (not "WebSocket disconnected"), and route to the session-expired posture. Tick and order-update streams are separate, each with its own auth/reconnect policy (IBR-3). |
| Tick data stops | Detect stale data. |
| Duplicate ticks | Ignore / de-duplicate. |
| Late ticks | Mark as delayed. |
| Too many symbols | Manage subscriptions intelligently. |
| LTP missing | Fall back to REST quote if configured. |
| Market-data mismatch | Alert and pause the affected strategy if needed. |

**Exposed data states:** `Live`, `Stale`, `Disconnected`, `Delayed`, `Unknown`. A strategy can ask "is this price tradable?" (tradable ⇔ `Live`), not only "what is the LTP?".

## Session management (CAP-11)

Supports session **establishment**, login flow, token storage, token-expiry detection, session validation, re-login notification, multiple accounts, multiple brokers, paper/live separation, and session health check. Alert on: token expiry; login failure; TOTP failure; order API unavailable; market data disconnected; broker session invalid. **Live trading does not start unless session health is confirmed.** (Secret hygiene per CAP-23: tokens stored encrypted; never logged. The login is adapter-owned and may be multi-step — e.g. Kotak Neo's consumer-key → session/view-token → MPIN+TOTP — returning a normalized `HEALTHY / NEEDS_REAUTH / FAILED`; "token storage" is an opaque per-broker session bundle, not one token.)

**Establishment vs expiry-detection (IBR-1).** These are two distinct mechanisms. Some brokers have **no programmatic/headless session refresh** — Zerodha Kite mints an `access_token` once per day via interactive login + TOTP that dies the next morning (~6am IST) and cannot be renewed by an API call. So **establishment** is a first-class adapter step (`establish_session`) that runs **before** the safe-start gate (CAP-28): the default path is an operator-supplied `request_token`/token-of-day at boot (the adapter exchanges it for the `access_token` and stores it encrypted per SE-2); an optional automated TOTP-login adapter may exist but is flagged best-effort and broker-ToS-sensitive. A per-broker capability flag, **"headless session refresh"**, governs behavior: Kite = unsupported (establishment required; never attempt an API "refresh"), Kotak Neo = unknown-until-verified. On a cold-start with a dead daily token, safe-start correctly **blocks** until the day's token is established — UJ-3 stays fail-closed-correct.

## Rate-limit protection (CAP-12)

Protect the user before the broker rejects. Support: orders/sec, orders/min, daily order limit, modifications-per-order, quote-request limit, historical-data-request limit, request queueing, broker-specific throttling, **priority for exit orders**, lower priority for non-critical requests. When rate-limited: slow down, alert, and **preserve exit capability**.

## Failure / degradation behavior (CAP-25)

| Condition | Required behavior |
|---|---|
| Broker API down | Block new entries; allow safe exits if possible; alert; keep retrying health checks; avoid request spam. |
| Market data stale | Block price-sensitive entries; allow exits with configured protection; alert; try fallback quote source if configured. |
| Order state unknown | Pause new risky entries process-wide (exits exempt); reconcile order book / trades / positions; alert; resume only when resolved. |
| Order UNKNOWN **and** broker unreachable (NFR-2) | Retry reconciliation on a bounded budget with backoff; on exhaustion move the order to terminal `MANUAL_INTERVENTION_REQUIRED` (queryable on `/healthz` + CLI), fire the dead-man's-switch escalation, keep the pause in force. **Do not auto-square-off** an unconfirmed position — exit stays operator-initiated. |
| Position mismatch | Stop the affected strategy; alert; rebuild position state from broker; avoid duplicate exit; require manual/configured recovery. |
| Risk limit breached | Activate kill switch; block new entries; optionally cancel open orders; optionally square off; generate risk event. |
| Session expired | Block trading; alert; **re-establish session (interactive daily login may be required where the broker has no headless refresh)**; require operator login if needed. |

## Data persistence & crash recovery (CAP-24)

**Persist:** order intents; broker order IDs; order state changes; trades; position snapshots; risk events; broker responses; error events; system health events.

**Restart sequence (never trade immediately on restart):**
1. Load last-known state.
2. Check broker session.
3. Fetch latest broker order book.
4. Fetch latest trades.
5. Fetch latest positions.
6. Resolve unknown orders.
7. Resume only if safe.

## Observability & auditability (CAP-15)

**Record per order:** initiating strategy; broker; account; signal time; validation time; time sent to broker; broker response; order status changes; trade execution; risk checks passed/failed; reconciliation result; final P&L; error reason; manual override (if any).

**Outputs:** structured logs; audit logs; trade journal; daily report; error report; real-time alerts; health dashboard.

**Alert conditions:** order rejected; order unknown; WebSocket disconnected; position mismatch; daily loss breached; kill switch activated; broker login failed; square-off failed; margin insufficient; stale market data; repeated order rejection.

**Alert delivery (D5):** one `AlertSink` interface; MVP channels are Telegram + generic webhook (email deferred). A **dead-man's-switch heartbeat** is mandatory: the alerter emits a periodic "alive" and an *external* watcher alarms on its absence — silent alert failure is invisible until the incident you needed it for. Every channel ships behind `send_test_alert()` invoked from the production checklist.

## Instrument-master lifecycle (CAP-33)

The instrument/scrip master changes **daily** in Indian markets (F&O strikes and weekly expiries are added; expired contracts drop off), so it is owned as a first-class lifecycle, not a one-off fetch.

| Concern | Behavior |
|---|---|
| Scheduled refresh | Download the per-broker, per-segment master at least once daily, **before the first trade** (and on demand) — Kite instrument dump, Kotak Neo scrip master. |
| Caching & persistence | Cache locally, versioned by trading date; survives restart so a mid-day restart does not force a re-download to trade. |
| Symbol ↔ token resolution | Broker-neutral symbol → current broker instrument token (and reverse) for both order placement and market-data subscription (CAP-10). |
| Derived metadata | Lot size, tick size, freeze quantity, expiry, strike, instrument type — the source feeding the validation gate (CAP-3) and instrument-level risk (CAP-7). |
| Staleness gating | A master that is not current (older than today / older than threshold) or that failed to download **blocks trading at safe-start** (CAP-28) with an alert — never trade on a stale master. |
| New / expired contracts | Newly listed strikes/expiries become tradable only after they appear in a refreshed master; expired/unknown instruments are rejected at validation. |
| Refresh failure | Treated as a safe-to-retry read with backoff; persistent failure raises an alert and blocks entries that depend on unresolved instruments, without request spam (ties to CAP-25). |

## Corporate-action awareness (CAP-34)

A corporate action changes a position at the broker without any order being placed — which naive reconciliation would flag as a mismatch or manual intervention, risking a false alert or a duplicate corrective order.

| Action | Effect to recognize |
|---|---|
| Stock split / consolidation | Held quantity and average price re-based by the ratio; not a fill, not a manual change. |
| Bonus issue | Quantity increases; treat as corporate action, reconcile holdings accordingly. |
| Symbol / ISIN change | Tradingsymbol and instrument token change; re-resolve via the refreshed instrument master (CAP-33). |
| F&O contract adjustment | Lot size / strike adjustments on the underlying's contracts; pick up from the refreshed master. |

**Behavior:** on reconciliation (CAP-6), a quantity/price/symbol change that matches a known corporate action is classified as such — it does **not** raise a position-mismatch alert (CAP-6) or a manual-intervention signal (CAP-16), and it does **not** trigger a corrective order. The instrument-master token mapping is updated so subsequent orders and subscriptions use the new identity. The corporate-action source is configured (broker feed or external provider); absence is surfaced, not silently assumed.

## Funds / margin refresh cadence (CAP-6, CAP-7)

Funds and margin change intraday (MTM, new positions, SPAN/exposure shifts), so the funds/margin view is not a once-at-startup fetch:

- Refreshed on a **configured intraday cadence**, and additionally **before margin-sensitive orders** and **after fills**.
- A pre-order margin check (CAP-3) never runs against a view older than the configured cadence; if the latest view cannot be refreshed, the margin check fails closed (block + alert) rather than trusting a stale figure.
- Feeds account-level max-margin-usage risk (CAP-7) and the margin/SPAN shock simulation (CAP-32).

## Tamper-evident ledger & reporting (CAP-30)

The audit record is built on the write-ahead intent log (CAP-26) and hardened into a **system-of-record foundation** — *not* a legal/dispute-grade artifact (see SPEC Non-goals):

- **Hash-chained, append-only:** each ledger entry commits the previous entry's hash, so modification **by a party without the chain-signing key** (or accidental corruption) breaks the chain and is detectable. Modification by the key-holder is **not** detectable from the ledger alone — that needs the independent third-party anchoring tracked as Open Question 3 (SEC-2).
- **Periodically signed:** the chain head is signed at intervals (per-account key-id stamped) for non-repudiation against non-key-holders; the public key is written to the account data dir and the EOD report so the operator can verify.
- **Redaction is bound to this persistence path (SEC-3/SEC-6).** Records written to the intent log and ledger pass the same secret-shape scrubber as logs, via an allowlist of persisted fields at the `dispatch()` chokepoint; raw broker session/auth responses are **never** written to the inspectable chain (they live only in the SE-2 encrypted token store). The CAP-23 "no secret in any output" test covers the intent log and ledger, not just structlog.
- **EOD signed reconciliation report:** a timestamped statement of what the library intended, sent, confirmed, and reconciled against the broker — the artifact that lets the operator close the laptop.
- **Push position/exposure heartbeat:** a periodic "still safe" signal to the operator's phone (flat/hedged/exposure), so the job becomes "stop watching," not "watch a dashboard."

## Liveness & process self-health watchdogs (CAP-29)

Beyond market-data staleness (CAP-10), the library models its own liveness:

- **"Connected-but-mute" feed watchdog:** a still-open WebSocket delivering no ticks past a threshold is treated as stale — forced reconnect or degrade. ("Connected" is not "live.")
- **Process self-health:** disk (audit/WAL growth + log rotation), memory, and file-handle limits are monitored; breaching a limit triggers safe degradation and an alert *before* it silently kills logging or the intent log.
- **Clock/stall feed-in:** clock-skew and event-loop/process-stall detection (CAP-27) route into the same safe-degradation posture (degrade to exit-only).
