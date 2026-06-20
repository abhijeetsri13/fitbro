# Glossary — Broker-Neutral Trading Execution Library

Domain terms downstream skills and agents must read to interpret SPEC.md correctly. Indian-market and broker-specific vocabulary, plus library-internal terms.

## Brokers & SDKs

| Term | Meaning |
|---|---|
| **Kite / Zerodha Kite** | Zerodha's trading platform; accessed via Kite Connect API and the `pykiteconnect` Python SDK. First supported broker. |
| **Kotak Neo** | Kotak Securities' trading platform; accessed via the Kotak Neo Python SDK. Second supported broker. |
| **Broker adapter** | Library component implementing the broker-neutral interface for one broker by translating to/from that broker's SDK. |
| **Capability model** | Per-broker declaration of which features the broker supports (see `broker-capabilities.md`). |

## Order & product vocabulary

| Term | Meaning |
|---|---|
| **Product type** | How a position is held/margined. Kite examples: `MIS` (intraday), `NRML` (overnight F&O), `CNC` (delivery equity). Brokers name these differently — normalized by the library. |
| **Order type** | `MARKET`, `LIMIT`, `SL` (stop-loss limit), `SL-M` (stop-loss market) — broker values differ; normalized. |
| **Instrument master / scrip master** | The broker's downloadable list of all tradable instruments with their tokens, lot/tick/freeze, expiry, strike, and type. Changes daily (F&O); refreshed before each trading day and cached (CAP-33). Kite = "instrument dump"; Kotak Neo = "scrip master". |
| **Instrument token** | Broker-specific numeric ID for an instrument, used for order placement and market-data subscription; resolved from a broker-neutral symbol via the instrument master. |
| **Lot size** | Minimum tradable quantity unit for a derivative contract; orders must be whole multiples. |
| **Freeze quantity** | Exchange-imposed max quantity per single order; larger orders must be split. |
| **Tick size** | Minimum price increment for an instrument; order price must align. |
| **GTT** | Good-Till-Triggered order — rests at the broker until a trigger condition fires. |
| **AMO** | After-Market Order — accepted while the exchange is closed, queued for next session. |
| **CO (Cover Order)** | Order bundled with a compulsory stop-loss. |
| **BO (Bracket Order)** | Order bundled with target + stop-loss + optional trailing SL. |
| **Square off** | Closing an open position by sending the offsetting order. |
| **Client-side reference** | Library-generated unique ID attached to every order intent to enforce idempotency (CAP-4). |
| **Order intent** | The library's record of a requested order before/independent of the broker's acceptance; the unit of idempotency and persistence. |

## Options & risk vocabulary

| Term | Meaning |
|---|---|
| **Hedge** | A protective long option bought to cap the risk of a short option; "hedge-first" buys it before selling the short leg (CAP-8). |
| **Naked option selling** | Selling an option with no hedge — the library prevents this when configured (CAP-8). |
| **Basket / multi-leg trade** | Several orders forming one logical trade (e.g., a hedged option spread), executed with leg dependency and rollback (CAP-9). |
| **Basket margin** | Combined margin for a multi-leg basket, lower than the sum of legs; only some brokers expose it. |
| **OTM** | Out-of-the-money option. "Far OTM buying" and "low-premium selling" are configurable blocks. |
| **Intraday** | Positions opened and closed within one session (MIS-style); auto square-off applies. |
| **Expiry day** | The day a derivative contract expires; stricter rules may apply. |
| **RMS** | Broker Risk Management System; source of broker-side rejections ("RMS rejection"). |
| **Margin / funds** | Capital required/available; checked pre-order where the broker exposes it. |

## Market data & session vocabulary

| Term | Meaning |
|---|---|
| **LTP** | Last Traded Price. |
| **Tradable price** | A price the library certifies as safe to act on (data state = Live, not Stale/Disconnected/Delayed/Unknown) — CAP-10. |
| **WebSocket (market data / order updates)** | Streaming channels for ticks and order-status pushes; both may disconnect and must auto-reconnect. |
| **Stale tick** | Market data that has stopped updating beyond a threshold; blocks price-sensitive entries. |
| **TOTP** | Time-based One-Time Password used in broker login flows. |
| **MPIN** | Mobile PIN used by some brokers (e.g., Kotak) during login; a secret that must never be logged. |
| **Access token** | Short-lived broker session token; stored encrypted, expiry detected. |
| **Muhurat trading** | Special ceremonial trading session on Diwali; part of the market calendar (CAP-21). |

## Library-internal terms

| Term | Meaning |
|---|---|
| **Validation gate / gatekeeper** | The mandatory pre-submission check pipeline (CAP-3); strategies cannot bypass it. |
| **Reconciliation** | Rebuilding local state from broker order book, trade book, positions, holdings, and funds (CAP-6). |
| **UNKNOWN state** | An order whose true status is uncertain (e.g., post-timeout); blocks new risky orders until resolved (CAP-5). |
| **Kill switch** | A control that halts some or all trading; five types (soft/strategy/broker/account/panic) — `operational-modes.md`. |
| **Trading mode** | One of live/paper/dry-run/replay/monitor-only/exit-only/emergency — `operational-modes.md`. |
| **Virtual position** | A strategy-scoped position view, distinct from the net broker position (CAP-19). |
| **Net broker position** | The broker's actual net position for an instrument across all strategies. |
| **Write-ahead intent log** | Append-only, fsync'd record of "about to send order X with client-ref R," written before the broker socket call; replayed on boot (CAP-26, `technical-architecture.md` D1). |
| **Safe-start gate** | Cold-boot check (session, reconciliation, clock, config, egress-IP) that must pass before the first order (CAP-28). |
| **Clock abstraction** | Injectable time source (`now`/`sleep`/`monotonic`) enabling deterministic tests, faithful replay, and skew/stall detection (CAP-27). |
| **Hash-chained ledger** | Audit ledger where each entry commits the prior entry's hash, making tampering detectable; periodically signed (CAP-30). System-of-record *foundation*, not legal proof. |
| **Dead-man's-switch heartbeat** | Periodic "alive" signal whose *absence* an external watcher alarms on — so silent alerter failure is itself detected (CAP-30, D5). |
| **SPAN / exposure margin** | Exchange-defined derivative margin components; can balloon mid-position on a volatility spike, triggering broker RMS auto-square-off — modeled pre-trade by CAP-32. |
| **Process-per-account** | One OS process per trading account, each with its own intent log/store/kill switch, bounding kill-switch blast radius (D3). |
| **OpenAlgo** | A self-hosted, AGPL-v3 broker-connectivity *platform* (Flask+React, REST/WebSocket) for 30+ Indian brokers. Prior-art studied here, **not** a runtime dependency (D7). |
| **Fake broker** | An adversarial in-test broker that injects faults (delayed/dropped acks, duplicate fills, 429s) to verify UNKNOWN-handling and reconciliation (CAP-31). |
