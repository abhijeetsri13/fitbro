# Story 6.5b: Multi-account process wiring (remainder of 6.5)

Status: ready-for-dev
Epic: 6 · FR-33 · Architecture: ID-1, COH-2/IBR-4 (shared refdata cache + cross-process lock), TO-5

## Context

6-5's decision core (SupervisorPolicy: exit-code contract, restart backoff,
absence alarm, crash-loop breaker) landed earlier (commit 23ecee5). Remaining:
the OS-facing wiring, done portably.

## Acceptance Criteria (scoped to what is testable in this repo)

1. **Per-account data-dir layout**: an `AccountDataDir` helper owns the layout
   (intent log, sqlite, token store, ledger, kill journal under one root per
   account id); creation is idempotent, permissions via the existing platform
   perms seam (0700 dir); two accounts NEVER share a path (collision-proof
   test: distinct ids → disjoint trees; same id → same tree).
2. **Shared refdata cache + cross-process lock**: a `SharedRefdataCache` keyed by
   `(broker, segment, trading_date)` outside the per-account dirs; acquisition via
   a cross-process file lock (platform seam: lock file create-exclusive /
   O_EXCL-style atomic create with stale-lock detection by age; Windows+POSIX
   impls in src/platform/). First process wins the download (injected fetch
   seam), siblings read the cached artifact. Tests: winner-writes/loser-reads,
   stale-lock takeover, corrupt-cache → re-download not crash.
3. **Supervisor spawn policy (decision layer, not OS exec)**: extend
   `SupervisorPolicy`/new `SupervisorPlan` to manage N accounts: per-account
   independent restart state (one account's crash/backoff NEVER touches
   another's — isolation test), absence-alarm per account, and a
   `deploy/systemd/broker-exec@.service` template unit file (ID-1) committed +
   documented (README section) — the actual exec/systemd path is operator-side.
4. All existing ctests stay green.

## Planning decisions (binding for dev)

- NO real process spawning in tests (no fork/CreateProcess in the library); the
  supervisor stays a decision core + the systemd template does OS supervision
  (that IS the architecture: ID-1 systemd template units). Document this split.
- Cross-process lock: `src/platform/` gains `file_lock.hpp/.cpp` — atomic
  create-exclusive lockfile with pid+timestamp payload, steal only when older
  than a configured staleness (default generous, e.g. 10 min) AND validated
  refetch; fail-closed on ambiguity (wait/error, never two writers). Use
  std::filesystem atomic create semantics (open with std::ios::noreplace is
  C++23 — use platform seam CreateFileW/open(O_CREAT|O_EXCL) inside platform/).
- SharedRefdataCache composes with existing refdata::InstrumentMaster /
  TradingCalendar via their injected fetch seams — the cache provides the
  fetch_csv/fetch_json function that first consults the shared file.
- Tests simulate two "processes" as two objects in one test process contending
  on the same lock path (real cross-process contention is tier-2 operator
  verification; the atomicity primitive is the platform seam's contract).

## Tasks

- [ ] platform file_lock (Windows + POSIX impls, atomic create-exclusive, stale takeover)
- [ ] AccountDataDir layout helper (+perms, collision tests)
- [ ] SharedRefdataCache (broker,segment,date) + winner/loser/corrupt tests
- [ ] SupervisorPlan multi-account isolation + per-account absence alarm
- [ ] deploy/systemd/broker-exec@.service + README operator doc
- [ ] CMake wiring + all ctests green
