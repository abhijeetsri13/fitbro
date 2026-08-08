# Story 6.2: Kotak Neo adapter certification

Status: ready-for-dev
Epic: 6 (Breadth — Kotak Neo Adapter & Multi-Account)
FRs: FR-1, FR-2, FR-37 · Architecture: IA-2, TO-6 (two-tier certification), CAP-13

## Story

As a platform maintainer,
I want the Kotak adapter certified,
So that it is held to the same safety bar as Kite.

## Acceptance Criteria

1. **Conformance green**: the SAME conformance kit that certifies the Kite adapter
   (tests/conformance, `broker_exec::testing` kit) runs against a
   `KotakBrokerAdapter : ports::BrokerPort` and passes the full fault matrix with
   ZERO duplicates (tier-1 cert). Every Kotak `unknown` exercised by the kit is
   resolved **with a committed fixture** (recorded-response strings).
2. **Capability differences degrade safely**: capability deltas vs Kite (basket
   margin, order-update WS, headless refresh) are reflected in `kotak_capabilities()`
   tri-state and an unsupported/unknown required capability is REJECTED at
   `CapabilitySet::require()` (early, not mid-trade).
3. **Live-min-qty smoke documented**: a `docs/kotak-min-qty-smoke.md` runbook mirrors
   the Kite one (tier-2 operator step); tier-2 items tracked (paths/envelopes/auth
   verified live, capability unknowns resolved).

## Planning decisions (binding for dev)

- **Mirror 2-14 exactly** (read its story file + the kite conformance test): build
  1. `KotakBrokerAdapter : ports::BrokerPort` over `KotakRestClient` —
     tag/order-id → client_ref correlation maps (Kotak has no tag echo verified:
     use order-id map primary, attribute corroboration fallback, fail-closed stay-
     UNKNOWN per arch NFR-1 precedence), Kotak status → OrderState mapping
     (open/complete/rejected/cancelled/trigger pending…), integer paise (Kotak
     prices are decimal strings — parse via the same decimal→paise path used by
     the instrument master, NO float), no-throw, scrubbed.
  2. `RecordedKotakServer` — stateful Kotak-HTTP twin of FakeBroker implementing the
     scripted `HttpClient` seam: place accepts jData, returns `{"stat":"Ok","nOrdNo":…}`,
     orderbook reflects state, fault-injection knobs matching the FakeBroker matrix
     (ack-lost, timeout-after-accept, reject, partial fill, duplicate-tag collision).
  3. Conformance test TU `tests/conformance/` (or mirroring where kite's lives)
     running the SAME kit verbatim against the Kotak adapter.
- **Explicit non-vacuity**: include the ack-lost recovery assertion + no-blind-retry
  place-count probe exactly as the Kite conformance does (2-14 review closed that
  hole; do not reopen it).
- **OrderState mapping fail-closed**: an unrecognized Kotak status string maps to
  UNKNOWN-equivalent (never Filled/Cancelled guess). Use `fillnorm` canonical
  normalizer semantics: drive off filled qty when present.
- **Capabilities**: after 6-1 review, mutations sit at Unknown (fixture-only).
  Certification against RecordedKotakServer is still tier-1 fixture evidence, NOT
  live: keep them Unknown but add `docs/kotak-min-qty-smoke.md` as the tier-2 gate
  that flips them. State this explicitly in kotak_capabilities() comments.
- **No new Conan dep.**

## Tasks

- [ ] KotakBrokerAdapter (BrokerPort impl, correlation maps, state mapping, paise)
- [ ] RecordedKotakServer (fault-injection HTTP twin)
- [ ] Conformance TU reusing the kit verbatim; ack-lost + place-count assertions
- [ ] kotak_capabilities comments + docs/kotak-min-qty-smoke.md runbook
- [ ] CMake wiring + all ctests green
