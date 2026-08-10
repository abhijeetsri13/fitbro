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

- [x] KotakBrokerAdapter (BrokerPort impl, correlation maps, state mapping, paise)
- [x] RecordedKotakServer (fault-injection HTTP twin)
- [x] Conformance TU reusing the kit verbatim; ack-lost + place-count assertions
- [x] kotak_capabilities comments + docs/kotak-min-qty-smoke.md runbook
- [x] CMake wiring + all ctests green

## Dev Agent Record

### Agent Model Used
Opus 5 (1M context) — BMAD dev agent.

### Completion Notes List

- **PART A — `KotakBrokerAdapter : ports::BrokerPort` over `KotakRestClient`.**
  Correlation runs a TWO-rung ladder because Kotak has no verified tag echo:
  (1) `nOrdNo -> client_ref`, bound on a successful ack; (2) attribute
  corroboration on `(symbol, side, quantity)` against intents registered BEFORE
  the wire call, admitted only when the pairing is unambiguous in BOTH directions
  (exactly one un-anchored intent AND exactly one un-matched broker row carry the
  key). Anything else leaves an EMPTY client_ref (fail-closed). A successful
  corroboration immediately anchors the id, so the weak rung runs at most once per
  intent. **No speculative client tag is sent** — an unverified `jData` key is a
  live-rejection risk and a token we cannot trust to return is not an anchor.
- **State mapping is fail-closed, then quantity-driven.** An unrecognized `ordSt`
  maps to `OrderState::Unknown`, full stop. A RECOGNIZED status is handed to
  `fillnorm::normalize_fill` as a NEUTRAL token so only its quantity-first rule
  applies. The raw Kotak string is deliberately never given to fillnorm: its
  terminal-by-status matching is substring-based, and Kotak's vocabulary contains
  `"not cancelled"` / `"cancel pending"` — live orders that a `"cancel"` substring
  match would report as terminal.
- **PART B — `tests/conformance/kotak_conformance_test.cpp`**: a stateful
  `RecordedKotakServer : HttpClient` (Kotak-HTTP twin of FakeBroker) reproducing
  the fault matrix plus three Kotak-specific hazards (HTTP-200 `Not_Ok`, partial
  fill under a "complete" status, colliding manual order); an `OwningKotakAdapter`
  owning server + REST client + synthetic bundle + adapter behind one BrokerPort;
  the kit reused verbatim asserting `report.ok()` + `duplicate_orders == 0`.
- **Explicit non-vacuity kept (2-14's closed hole is not reopened):** the ack-lost
  recovery assertion checks the EXACT minted client_ref comes back AND that it
  never appeared in any wire body (so recovery cannot be an echo); the
  `place_count()` probe asserts exactly one wire place per intent.
- **Capabilities stay entirely `Unknown`.** A guard test enumerates every
  capability and fails if any was promoted, because tier-1 green must not be
  mistaken for live evidence. `docs/kotak-min-qty-smoke.md` is the tier-2 flip gate
  and maps each step to the Unknowns it resolves.

### Adversarial review round 1 — FIX-REQUIRED, all findings applied

- **HIGH-1 (stale look-alike bait).** A registration made before the wire call
  survived a DEFINITIVE rejection, so an operator's identical manual order was
  later adopted as ours. `place()`/`modify()` now `drop_pending()` unless the error
  is reconcile-first-ish (the same test `Dispatcher::is_reconcile_first` uses,
  restated locally since an adapter may not link the runtime). Anchoring also
  retires the entry, and the set is bounded at `kMaxPendingIntents`.
- **HIGH-2 (the duplicate metric could read zero during a real duplicate).** The
  kit counts rows BY CLIENT_REF, so two ack-lost duplicates make the attribute key
  ambiguous, both rows come back ref-empty, and the metric reads 0. Added
  `ConformanceTally`, collected in `OwningKotakAdapter`'s destructor (a raw server
  pointer would dangle), asserting from BROKER TRUTH: `book_size() <= 1` per
  scenario, `place_count() <= 1` per scenario, and total places == scenarios.
- **HIGH-3 (absent total read as zero -> terminal Filled).** The order total is now
  `std::optional`. With no total a WORKING status can never exceed PartiallyFilled;
  `first_number`/`first_paise` distinguish absent from present-but-garbage.
- **HIGH-4 (header overclaimed; resolver could overturn the refusal).** Verified
  `unknown_resolver.cpp` is the ONLY consumer of a broker row's side/quantity/price,
  so the adapter now publishes that correlation tuple **only for rows it positively
  correlated** — making the refusal stick at the stack level instead of being
  re-decided, more weakly, by rung 3. Pinned by a new adapter+UnknownResolver test
  WITH a positive control. Header claims rewritten; the `MatchKind::CorrelationToken`
  mislabelling (b2) is documented in the header and runbook (there is nowhere on
  `domain::Order` to signal a weak match without a cross-module ripple).
- **MEDIUM-1** fill clamped to the ordered quantity. **MEDIUM-2** new header-only
  `domain/decimal_paise.hpp`: fail-closed `std::optional` parse, overflow is a parse
  failure (the 20-digit UB), trailing garbage rejected; refdata/kite copies left
  alone as a tracked follow-up. **MEDIUM-3** restart-recovery limitation documented
  and the runbook step rewritten around the durable-intent/UNKNOWN posture.
  **MEDIUM-4** throttle deviation restated as load-bearing + the untested
  total-outage lane tracked. **MEDIUM-5** `square_off()` now returns a typed
  `NotSupported` refusal instead of failing open. **LOW** dead unsigned branch fixed;
  hardcoded funds selector documented.
- **Beyond the findings:** the malformed-field policy is now graded by what each
  read is used for — orders fail the ROW closed (orders must stay enumerable),
  trades drop only the attribution, positions and funds fail the WHOLE read (both
  size real risk). This also removed a set-but-unused `malformed` flag the first
  pass would have shipped.

### File List
- include/broker_exec/adapters/kotak/kotak_broker_adapter.hpp (new)
- src/adapters/kotak/kotak_broker_adapter.cpp (new)
- include/broker_exec/domain/decimal_paise.hpp (new — header-only, fail-closed money parse)
- tests/conformance/kotak_conformance_test.cpp (new)
- docs/kotak-min-qty-smoke.md (new)
- include/broker_exec/adapters/kotak/kotak_capabilities.hpp (modified — tier-2 gate comments)
- src/adapters/kotak/kotak_capabilities.cpp (modified — promotion table + "6.2 flips nothing")
- src/adapters/kotak/CMakeLists.txt (modified — adapter source; domain PUBLIC; fillnorm PRIVATE)
- tests/CMakeLists.txt (modified — broker_exec_kotak_conformance_tests target)
- CMakeLists.txt (modified — src/fillnorm added before src/adapters/kotak, which links it)
