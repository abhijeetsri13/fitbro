# Error Taxonomy & Suggested Actions

Backs **CAP-13, CAP-14**. Broker errors are normalized into stable categories, each carrying a suggested action. Strategy code reacts to the **category**, never to broker-specific error text.

## Error categories → suggested action

| Category | Typical cause | Suggested action(s) |
|---|---|---|
| Authentication error | Bad/expired credentials at login | Re-establish session (interactive daily login may be required where the broker lacks headless refresh); raise manual alert if it fails. |
| Session expired | Token no longer valid | Re-establish session; block trading until restored. "Refresh session" applies only to brokers whose `headless session refresh` capability is supported — for Kite, re-establishment requires the operator's daily login (IBR-1). |
| Insufficient funds | Margin shortfall | Do not retry; block strategy / raise alert. |
| RMS rejection | Broker risk system refused | Do not retry; reconcile; raise alert. |
| Invalid symbol | Wrong/unknown instrument | Do not retry; block strategy (config/data error). |
| Invalid quantity | Lot violation (or freeze violation in reject-mode) | Do not retry; fix at validation layer. In slice-mode an over-freeze quantity is **sliced**, not classified here (it is a transform, not a rejection). |
| Invalid price | Tick/limit violation | Do not retry; fix at validation layer. |
| Exchange rejection | Exchange refused order | Do not retry; reconcile; raise alert. |
| Rate limit | Too many requests | Slow down / queue; preserve exit priority (CAP-12). |
| Network timeout | No/late response | **Reconcile first** for dangerous ops; safe-retry reads. |
| Broker server error | 5xx / broker outage | Reconcile; block new entries; retry health checks without spam (CAP-25). |
| Market closed | Outside session | Block (unless AMO configured); wait for session. |
| Order already filled | Modify/cancel on a filled order | Do not retry; reconcile and update state. |
| Order already cancelled | Modify/cancel on a cancelled order | Do not retry; reconcile and update state. |
| Unknown response | Unparseable / ambiguous | Mark order `UNKNOWN`; reconcile (CAP-5). |

## Suggested-action vocabulary

The normalized action attached to each error is drawn from this fixed set, so strategies and the runtime can switch on it deterministically:

- Retry safe request
- Do not retry
- Reconcile first
- Block strategy
- Refresh session
- Cancel open orders
- Square off
- Raise manual alert

## Retry-safety coupling (CAP-14)

Error handling composes with the retry-safety classification in `order-lifecycle.md`:

- A **network timeout** on a **read** → "Retry safe request" (backoff).
- A **network timeout** on a **dangerous operation** (place/modify/cancel/square-off) → "Reconcile first" → mark order `UNKNOWN`, never an immediate repeat.

## Required behavior

- Each broker adapter maps its raw error responses onto these categories and actions; the mapping is part of the adapter's certification.
- Representative Kite and Kotak Neo errors for the same condition must resolve to the **same** category and action (CAP-13 success criterion).
- No broker error string crosses the strategy boundary.
