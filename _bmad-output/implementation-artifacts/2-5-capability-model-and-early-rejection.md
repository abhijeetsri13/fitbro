# Story 2.5: Capability model and early rejection

Status: ready-for-dev

## Story

As a strategy author,
I want unsupported capabilities rejected at load/request time,
so that I never fail mid-trade on a missing broker feature. (FR-2)

## Acceptance Criteria

1. **Given** the per-broker capability set **When** a strategy requires a capability the broker lacks
   **Then** it is rejected early with a typed error (MVP posture = reject-only; substitution deferred).
2. **And** an unverified `unknown` capability is treated as unsupported.
3. **And** the capability set is queryable by strategy code.

## Tasks / Subtasks

- [ ] Task 1: `capabilities` module (AC: all)
  - [ ] `include/broker_exec/capabilities/` + `src/capabilities/`; target `broker_exec_capabilities` (+ alias). Pure
        value logic; depends inward only on `errors` (+ nothing transport). NO adapter/SDK dep.
  - [ ] `enum class Capability { ... }` — a representative set spanning the broker contract dimensions named in the
        spec/architecture, e.g.: PlaceOrder, ModifyOrder, CancelOrder, SquareOff, BasketMargin, OrderUpdateWebsocket,
        HeadlessSessionRefresh, GttOrders, AmoOrders, CoverOrder, BracketOrder, TagCarry, MarginShockSim. (Only model
        what's plausibly used now + the ones the architecture calls out; additive later.)
  - [ ] Tri-state support: `enum class Support { Supported, Unsupported, Unknown };` — **Unknown is treated as
        Unsupported** at the gate (AC-2): an unverified capability never silently passes.
- [ ] Task 2: `CapabilitySet` value type (AC: 1, 2, 3)
  - [ ] Immutable-after-build map from `Capability` -> `Support` (a fixed-size array keyed by enum is fine). Default for
        any unspecified capability = `Unknown`.
  - [ ] `[[nodiscard]] bool supports(Capability) const` — true ONLY if `Support::Supported` (Unknown/Unsupported -> false).
  - [ ] `[[nodiscard]] Support support_of(Capability) const` — exposes the tri-state for diagnostics.
  - [ ] `[[nodiscard]] Result<Ok> require(Capability) const` — `ok()` if Supported; else a typed
        `errors::Error{ ErrorCategory::NotSupported, SuggestedAction::DoNotRetry }` naming the capability (no substitution).
  - [ ] `[[nodiscard]] Result<Ok> require_all(std::span<const Capability>) const` — fail-closed on the FIRST unsupported,
        naming it (for load-time strategy capability checks).
  - [ ] A small builder (e.g. `CapabilitySet::builder().set(cap, Support).build()`), or a constructor taking a list of
        (Capability, Support) pairs. Whatever is cleanest + immutable result.
- [ ] Task 3: Kite capability profile (AC: 1, 2, 3)
  - [ ] `kite_capabilities()` (in this module or a tiny `capabilities/kite.hpp`) returning the Kite `CapabilitySet`:
        PlaceOrder/ModifyOrder/CancelOrder/SquareOff = Supported; HeadlessSessionRefresh = Unsupported (Story 2.4 reality);
        BasketMargin / OrderUpdateWebsocket / MarginShockSim / GttOrders etc. = Unknown or a defensible value per the
        architecture (leave genuinely-unverified ones as Unknown so they read unsupported until certified in Story 2.14).
- [ ] Task 4: CMake (orchestrator pre-wires root add_subdirectory(src/capabilities); NO new Conan dep)
  - [ ] `src/capabilities/CMakeLists.txt`: target links `broker_exec::errors` (PUBLIC, for Result/Error), warnings+
        sanitizers PRIVATE; test exe `broker_exec_capabilities_tests`.
- [ ] Task 5: Tests (AC: 1, 2, 3) — `src/capabilities/capabilities_test.cpp`
  - [ ] supports(Supported)==true; supports(Unsupported)==false; supports(Unknown)==false (AC-2).
  - [ ] require(Supported)->ok; require(Unsupported)->NotSupported Error naming the capability; require(Unknown)->NotSupported.
  - [ ] require_all fails closed on the first unsupported and names it; passes when all supported.
  - [ ] kite_capabilities(): PlaceOrder supported; HeadlessSessionRefresh NOT supported; an unverified one reads unsupported.
  - [ ] A default/unspecified capability reads Unknown -> unsupported.

## Dev Notes

- **MVP posture = reject-only** (no per-capability substitution; that's "Should-have-soon"). [architecture.md#CAP-2 de-conflated, RCT-3]
- **Unknown == unsupported** at the gate — never let an unverified capability pass. [epics.md#Story 2.5 AC, architecture.md#capability unknowns]
- **Queryable by strategy:** `supports()`/`require()` are the public surface a strategy/loader calls before trading.
- **Boundary:** pure value module; depends only on `errors`. Adapters (Story 2.14) attach a `CapabilitySet` to the
  BrokerPort; the gate (Story 2.8) and safe-start (2.13) consult it. Don't add a transport/SDK dependency here.
- **Errors:** `ErrorCategory::NotSupported` already exists; `SuggestedAction::DoNotRetry`. Name the capability in the message.

### References
- [Source: epics.md#Story 2.5] [architecture.md#IA-2 capability set, #RCT-3 CAP-2 de-conflated]
- [Source: docs/conventions.md] [Source: include/broker_exec/errors/error.hpp, ports/broker_port.hpp]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
