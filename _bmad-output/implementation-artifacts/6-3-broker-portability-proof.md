# Story 6.3: Broker-portability proof

Status: ready-for-dev
Epic: 6 (Breadth — Kotak Neo Adapter & Multi-Account)
FRs: FR-1, FR-2 · SM-3 · Architecture: IA-1/IA-2 (hexagonal), CAP-2 reject-only

## Story

As a strategy author,
I want to switch brokers by config,
So that my strategy is not locked to one broker. (SM-3)

## Acceptance Criteria

1. **Config-flip**: an unchanged option-selling strategy wired against
   `ports::BrokerPort` runs against Kite AND Kotak by flipping a broker config
   value only — zero strategy-code changes (proven by a test that runs the SAME
   strategy function against both adapters' recorded servers).
2. **Load-time rejection**: a required-but-unsupported/unknown capability is
   rejected at load/composition time (CapabilitySet::require_all at wiring),
   not mid-trade.
3. **Zero broker identifiers in strategy**: an automated scan test asserts the
   strategy TU references no broker-specific identifier (kite/kotak/zerodha
   substrings) — the C++ analog of the epics' "code scan".

## Planning decisions (binding for dev)

- **New module**: `broker_exec::composition` (or extend an existing runtime-side
  module if cleaner) with a `make_broker_adapter(config, deps) -> Result<unique_ptr<ports::BrokerPort>>`
  factory: `broker = "kite" | "kotak"` from the typed config; unknown name →
  fail-closed Validation error. The factory calls `require_all(required_caps)`
  against the chosen adapter's CapabilitySet BEFORE returning (AC-2) — a strategy
  declares its required capabilities as data.
- **Proof test**: a small representative strategy routine (place hedge → place
  short → read positions; pure BrokerPort calls) in the test, executed twice:
  once with the Kite adapter over RecordedKiteServer, once with the Kotak adapter
  over RecordedKotakServer (both exist after 6-2). Assert identical decision-path
  outcomes (orders placed, states) modulo broker order-ids.
- **Scan test**: read the strategy TU source at test time (std::filesystem +
  ifstream of the committed source path via a CMake-configured path or a
  constexpr string embed) and assert no "kite"/"kotak" substring (case-insensitive)
  in the strategy section. Keep it simple + deterministic.
- **Capability gate**: since Kotak mutations sit at Unknown until tier-2, the
  AC-2 test proves BOTH directions: requiring PlaceOrder against Kotak → rejected
  at load (Unknown fail-closed); against Kite → passes. That IS the demonstration
  of "rejected at load, not mid-trade" (CAP-2 reject-only MVP posture). The
  config-flip run test (AC-1) uses a required-caps set both satisfy, or wires the
  Kotak adapter with a test-overridden CapabilitySet documented as
  fixture-certification posture.
- **No new Conan dep.**

## Tasks

- [ ] composition factory: config → adapter, require_all at load, fail-closed
- [ ] same-strategy-two-brokers proof test (recorded servers)
- [ ] load-time rejection test (Unknown → reject; supported → pass)
- [ ] broker-identifier scan test
- [ ] CMake wiring + all ctests green
