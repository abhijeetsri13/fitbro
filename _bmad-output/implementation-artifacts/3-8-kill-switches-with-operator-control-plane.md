# Story 3.8: Kill switches with operator control-plane

Status: ready-for-dev

## Story

As an operator,
I want to trip any kill switch on a running process,
so that I have a reliable last line of defense. (FR-31)

## Acceptance Criteria

1. **Given** a running per-account engine **When** I issue a kill via the CLI **Then** an authenticated control command
   enqueues onto the main-loop queue and flips the in-process flag (soft/strategy/broker/account keep the process alive:
   block entries, allow exits; panic cancels+squares-off+blocks).
2. **And** a kill arriving mid-dispatch takes effect within one bounded broker-call timeout, and panic exits proceed even
   while an order is UNKNOWN.
3. **And** an accepted kill is persisted so a crash replays as still-killed.

## Tasks / Subtasks

- [ ] Task 1: Add kill switches to the EXISTING `modes` module (AC: all)
  - [ ] `include/broker_exec/modes/killswitch.hpp` + `src/modes/killswitch.cpp`; add to `broker_exec_modes`. Reuses the
        Story-3.7 `Posture` (the kill maps onto the posture coordinator's `operator_floor`). Depends inward on `ports`, `errors`.
- [ ] Task 2: Kill vocabulary + state (AC: 1)
  - [ ] `enum class KillType { Soft, Strategy, Broker, Account, Panic };` (+ to_string).
  - [ ] `struct KillCommand { KillType type; std::string scope; };` (scope = strategy id / broker / account; empty for Soft/Panic).
  - [ ] `class KillState` (the in-process flag set, owned by the main loop — the SOLE writer):
    - `void apply(const KillCommand&)`: record the kill (a set of active (type, scope)); idempotent (re-applying the same kill is a no-op).
    - `[[nodiscard]] bool panic_active() const`; `[[nodiscard]] bool any_active() const`.
    - `[[nodiscard]] bool blocks_entries() const`: true if ANY kill is active (every kill blocks new entries).
    - `[[nodiscard]] bool blocks_strategy(std::string_view strategy) const`: true under a Strategy kill matching `strategy`, or any account/broker/soft/panic kill.
    - `[[nodiscard]] bool allows_risk_reducing_exits() const`: true UNLESS panic (panic's square-off is driven out-of-band; under soft/strategy/broker/account, exits are allowed — AC-1).
    - `[[nodiscard]] Posture posture() const`: `Panic` if any panic; else `SoftKill` if any kill active; else `Normal`. (Feeds the
      Story-3.7 coordinator's `operator_floor` so the gate enforces it — the single chokepoint.)
- [ ] Task 3: Authenticated control-plane (AC: 1)
  - [ ] `class KillController` (ctor injects `std::function<bool(std::string_view)> authenticate` + a persist seam
        `std::function<Result<ports::Ok>(const KillCommand&)> persist`):
    - `[[nodiscard]] Result<ports::Ok> submit(const KillCommand& cmd, std::string_view auth_token)`: if `!authenticate(auth_token)`
      -> a typed Auth Error, NO enqueue, NO persist (an unauthenticated kill is rejected — the control plane is authenticated).
      Else PERSIST first (AC-3: an accepted kill is durable BEFORE it is acknowledged), then enqueue the command onto an internal
      thread-safe queue. If persist fails -> Error, do NOT enqueue (fail-closed: never ack a kill we could not persist).
    - `[[nodiscard]] std::vector<KillCommand> drain()`: pop all queued commands (called by the main loop at the top of each
      iteration AND immediately before dispatch — so a kill takes effect within one bounded broker-call timeout, AC-2). The
      caller applies each to `KillState` (the sole writer). Document the drain-points contract.
  - [ ] Crash replay (AC-3): `[[nodiscard]] static Result<ports::Ok> replay(KillState& state, const std::function<Result<std::vector<KillCommand>>()>& load)`:
        load the persisted kills on boot and `apply` each to a fresh `KillState`, so a crash between "accepted" and "acted" — and
        a normal restart — both come back STILL KILLED.
- [ ] Task 4: Panic + UNKNOWN (AC: 2)
  - [ ] Document: under Panic the emergency engine cancels + squares off out-of-band and panic exits proceed EVEN WHILE an order
        is UNKNOWN (the UNKNOWN-pause does not block a panic exit). `KillState` records panic (`panic_active()`); the actual
        cancel/square-off is the runtime engine, not this module. `allows_risk_reducing_exits()` false under panic = the NORMAL
        gate blocks everything; the panic square-off is the out-of-band path.
- [ ] Task 5: CMake — extend `src/modes/CMakeLists.txt`
  - [ ] Add `killswitch.cpp` to `broker_exec_modes`; add `killswitch_test.cpp` to `broker_exec_modes_tests`. No new deps.
- [ ] Task 6: Tests (AC: 1, 2, 3) — `src/modes/killswitch_test.cpp` (an in-memory persist/load + a fake authenticate)
  - [ ] AC-1: a soft kill -> KillState.blocks_entries() true, allows_risk_reducing_exits() true, posture()==SoftKill, NOT panic.
  - [ ] strategy kill scoped to "alpha" -> blocks_strategy("alpha") true, blocks_strategy("beta") false (unless a broader kill);
        broker/account kill -> blocks_entries true.
  - [ ] panic -> panic_active() true, posture()==Panic, allows_risk_reducing_exits() false (out-of-band exits), blocks_entries true.
  - [ ] AC-1 auth: submit with a GOOD token -> ok(), persisted (the persist seam recorded it) + drainable; submit with a BAD token
        -> Auth Error, NOTHING persisted, NOTHING enqueued (an unauthenticated kill never takes effect).
  - [ ] AC-3 persist+replay: submit a kill (good token) -> it is persisted; then a FRESH KillState + replay(load) -> the kill is
        re-applied (still killed after a simulated restart). A persist-failure on submit -> Error and NOT enqueued.
  - [ ] AC-2 drain: a submitted kill sits in the queue until drain(); drain() returns it and applying it flips the flag (models
        the main loop taking it effect on the next iteration / before dispatch). Idempotent re-apply.
  - [ ] coordinator integration: KillState.posture() fed as the Story-3.7 `operator_floor` -> a panic kill forces Posture::Panic
        even with no detector signals (reuse PostureCoordinator.evaluate({}, killstate.posture())).

## Dev Notes

- **Authenticated, persisted, single-writer** — submit() authenticates + persists BEFORE ack; the main loop is the sole writer
  to KillState (drains the queue, applies). A kill survives a crash (replay) — AC-3. [architecture.md#TO-1 kill control-plane, FR-31]
- **Within one timeout** (AC-2) — the loop drains the kill queue at the top of each iteration AND before dispatch; with a bounded
  per-broker-call timeout, a kill lands within one timeout. Panic exits proceed even under an UNKNOWN pause. [architecture.md#TO-2]
- **Posture integration** — KillState.posture() is the Story-3.7 coordinator's `operator_floor` (Panic via the kill switch), so the
  gate (2.8) enforces the kill as the single chokepoint. [architecture.md#FR-26, FR-31]
- **No float / no-throw / fail-closed** — never ack a kill that could not be persisted; an unauthenticated kill is rejected. [docs/conventions.md]
- **Reuse:** `modes::Posture`/`PostureCoordinator` (3.7), `ports::Ok`, `errors` (Auth/Internal).

### References
- [Source: epics.md#Story 3.8] [architecture.md#TO-1/TO-2 kill control-plane, FR-31] [Source: src/modes/posture.* (3.7)]
- [Source: docs/conventions.md]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
