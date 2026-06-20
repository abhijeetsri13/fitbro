# Risk Controls, Order Validation, Option-Selling & Basket Safety

Backs **CAP-3, CAP-7, CAP-8, CAP-9**. Risk is a first-class, four-level engine plus the pre-submission validation gate, option-selling safety, and basket execution rules. Every item below is a configurable rule that must demonstrably block (or transform) an offending order in tests.

## Pre-submission validation gate (CAP-3)

The gate runs before any broker call; an order failing any check is rejected with the failed check named. The lot-size, tick-size, freeze-quantity, exchange, and expiry values used below are sourced from the **current instrument master** (CAP-33) — a stale or unresolved master blocks trading at safe-start, so the gate never validates against yesterday's contracts.

| Check | Required behavior |
|---|---|
| Quantity | Multiple of lot size; an order over the exchange/broker **freeze limit is sliced into child orders** (default) rather than rejected — see `order-lifecycle.md` "Freeze-quantity slicing". Reject-on-over-freeze is an optional config. |
| Price | Aligned to tick size. |
| Product | Valid product type for the broker and exchange segment. |
| Exchange | Instrument belongs to the correct exchange. |
| Funds | Margin/funds checked where the broker exposes it, against a view no older than the configured cadence; on stale-and-unrefreshable, **fail closed** (block + alert). |
| Risk | Order does not violate account/strategy/instrument/order risk. |
| Time | New entries blocked outside configured trading windows. |
| Duplicate | Same signal does not create duplicate orders (CAP-4). |
| Hedge | Option selling verifies hedge rules where configured. |
| Kill switch | If active, only allowed emergency actions pass. |
| UNKNOWN-pause | If an order is `UNKNOWN` in this account process, new risky entries are blocked (risk-reducing exits exempt) — see `order-lifecycle.md`. |

## Level 1 — Account-level risk (CAP-7)

- Maximum daily loss
- Maximum daily profit lock
- Maximum deployed capital
- Maximum margin usage
- Maximum number of open positions
- Maximum orders per day
- Maximum trades per strategy
- Maximum lots per instrument
- Maximum loss per instrument
- Broker-level trading block
- Account-level kill switch

## Level 2 — Strategy-level risk (CAP-7, CAP-19)

- Restrict to selected instruments
- Restrict to selected brokers
- Restrict to selected product types
- Restrict to selected order types
- No trading after a configured time
- Exit-only orders after a cut-off time
- Own daily loss limit
- Own max lots and max margin limit
- Individually stoppable without stopping other strategies

## Level 3 — Instrument-level risk (CAP-7)

- Block illiquid instruments
- Block contracts with wide bid-ask spread
- Block stale market data (ties to CAP-10)
- Block instruments near expiry (if configured)
- Block far-OTM option buying (if configured)
- Block low-premium option selling (if configured)
- Block high-volatility event windows (if configured)
- Enforce lot size and tick size
- Enforce freeze-quantity handling

## Level 4 — Order-level risk (CAP-7)

- Block market orders unless explicitly allowed
- Enforce price protection
- Enforce max slippage
- Enforce trigger-price validity
- Enforce stop-loss order validity
- Enforce product type
- Enforce exchange
- Enforce allowed quantity
- Enforce hedge requirements for option selling
- Enforce margin availability

## Option-selling safety (CAP-8)

- Hedge-first execution (buy hedge before selling the short option)
- Basket-level margin validation (where broker supports it)
- Naked option-selling prevention
- Expiry-day stricter rules
- Intraday-only enforcement
- Auto square-off
- Maximum loss per strategy
- Maximum short option lots
- Stop-loss and trailing-stop behavior
- Partial-fill handling
- Emergency hedge or emergency exit behavior

**Hard rules:**
- If the hedge order fails, the short option sell order is **not** sent.
- If the short sell succeeds but the hedge fails, alert immediately and run the configured emergency behavior.

## Basket / multi-leg execution (CAP-9)

The library treats a multi-leg option trade as **one logical trade** unless explicitly configured otherwise. The hedge-first basket flow diagram is in `architecture-diagrams.md`.

| Basket behavior | Meaning |
|---|---|
| Hedge-first | Buy hedge before selling the short option. |
| Exit-first | Exit risky leg before opening a replacement leg. |
| All-or-none (simulated) | If one leg fails, cancel/exit already-executed legs where possible. |
| Partial-execution handling | Detect and respond when only some legs execute. |
| Basket margin check | Check combined margin where the broker supports it. |
| Leg dependency | Do not send dependent legs if prerequisite legs fail. |

**Basket-margin fallback is capability-gated (IBR-7).** "Sum of per-leg margins" is **not** a safe generic fallback for a net-short option basket — SPAN/exposure netting makes real basket margin far lower, so summing legs massively over-states it and false-rejects affordable hedged baskets. Where the broker exposes no basket/SPAN margin source: the net-short-basket margin check **fails closed** (block + alert "cannot validate basket margin on this broker") or requires an explicit, audit-logged operator opt-in — never a silent summed-legs substitution. The summed-legs estimate is restricted to single-leg / long-only contexts. The **margin/SPAN shock simulation (CAP-32)** is offered **only** where a basket/SPAN margin source exists; otherwise it reports "unavailable on this broker" via the capability gate, never a summed-legs approximation presented as a shock result.
