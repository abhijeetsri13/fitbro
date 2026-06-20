# Broker Capability Model

Backs **CAP-2**. The library maintains, per broker, a declared capability set. A strategy that requires a capability the selected broker lacks is rejected at request time (or a configured fallback is used) — never failed mid-execution.

## Capability dimensions

Each broker adapter declares support for every dimension below as `supported`, `unsupported`, or `unknown` (treated as unsupported until verified against the live SDK).

| # | Capability | Notes |
|---|---|---|
| 1 | Order placement | Core; required of every adapter. |
| 2 | Order modification | |
| 3 | Order cancellation | |
| 4 | Order book | Required for reconciliation (CAP-6). |
| 5 | Trade book | Required for reconciliation (CAP-6). |
| 6 | Positions | Required for reconciliation (CAP-6). |
| 7 | Holdings | Delivery/cash positions. |
| 8 | Margin / funds API | Enables pre-order funds check (CAP-3); not all brokers expose it. |
| 9 | Basket margin | Combined multi-leg margin; broker-dependent. |
| 10 | Market data WebSocket | Streaming ticks. |
| 11 | Order update WebSocket | Streaming order-status pushes. |
| 12 | Historical data | "Later" scope for abstraction. |
| 13 | Option chain | "Later" scope for abstraction. |
| 14 | GTT orders | |
| 15 | AMO orders | |
| 16 | Cover orders (CO) | |
| 17 | Bracket orders (BO) | |
| 18 | Multi-account support | |
| 19 | Sandbox / paper trading support | Broker-native sandbox, distinct from the library's paper mode. |
| 20 | Headless / programmatic session refresh | Whether the day's token can be renewed by an API call vs. requiring interactive daily login + TOTP (IBR-1). |
| 21 | Order-tag carry / echo | Whether a short correlation token can be stamped on the order and echoed back (drives UNKNOWN match precedence, NFR-1); note tag length limits (Kite ~20 chars). |
| 22 | Corporate-action source | A broker feed (or external provider) of split/bonus/symbol-change records the reconciler matches against (CAP-34). |
| 23 | Multi-step auth flow | Whether login is multi-step (e.g. consumer-key → session/view-token → MPIN+TOTP); adapter-owned, normalized below the boundary (IBR-6). |

## Per-broker matrix (to be confirmed against live SDKs)

Values are **declared expectations to verify during adapter implementation**, not asserted facts — every `unknown` must be resolved to `supported`/`unsupported` before that adapter is certified (production checklist, `scope-and-phasing.md`). This avoids encoding wrong capability data into the contract.

| Capability | Kite (Zerodha) | Kotak Neo |
|---|---|---|
| Order placement | supported | supported |
| Order modification | supported | supported |
| Order cancellation | supported | supported |
| Order book | supported | supported |
| Trade book | supported | supported |
| Positions | supported | supported |
| Holdings | supported | supported |
| Margin / funds API | supported | unknown |
| Basket margin | supported | unknown |
| Market data WebSocket | supported | supported |
| Order update WebSocket | supported | unknown |
| Historical data | supported | unknown |
| Option chain | unknown | unknown |
| GTT orders | supported | unknown |
| AMO orders | supported | unknown |
| Cover orders (CO) | unknown | unknown |
| Bracket orders (BO) | unknown | unknown |
| Multi-account support | unknown | unknown |
| Sandbox / paper trading | unknown | unknown |
| Headless session refresh | **unsupported** | unknown |
| Order-tag carry / echo | supported (tag ~20 chars) | unknown |
| Corporate-action source | unknown | unknown |
| Multi-step auth flow | unsupported (single access_token) | **supported** (consumer-key → session/view-token → MPIN+TOTP) |

> Open question (mirrors SPEC.md): every `unknown` is a verification task for the relevant adapter epic, not a finalized capability claim. Day-One-critical-path unknowns for Kotak Neo (resolve via an early spike, before the Kotak adapter epic): **Margin / funds API** and the exact **multi-step auth flow** — both gate Day-One funds checks (FR-6/CAP-3) and the broker-portability metric (SM-3).

## Required behavior

- **Early rejection (MVP posture = reject-only).** Capability mismatch is detected when the strategy/config is loaded or the order is requested — before any broker call. Per-capability **substitution** fallback is a "Should have soon" item, distinct from the Level-4 "broker fallback routing" (cross-broker smart routing).
- **Configured fallback (deferred, and capability-gated).** When substitution lands: it is applied and the substitution recorded in the audit ledger. **Caveat (IBR-7):** "sum per-leg margin in lieu of basket margin" is **not** a safe fallback for a net-short option basket (it over-states margin and false-rejects) — for those, fail closed or require an audited operator opt-in; restrict summed-legs to single-leg/long-only.
- **No silent degradation.** A missing capability is always surfaced (typed error or logged fallback), never silently ignored. Default for an unverified `unknown` is **unsupported**.
