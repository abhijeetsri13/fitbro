# Story 1.6: SQLite projection and numbered migrations

Status: review
Epic: 1 — Order-Safety Substrate

## Story

As a platform maintainer,
I want a queryable SQLite read-model rebuildable from the intent log,
So that state queries are cheap and crash-safe. (FR-7 backstop, NFR-4)

## Acceptance Criteria

1. **Given** the SQLite C API store (WAL, `synchronous=FULL`), **when** the app
   starts, **then** numbered migrations apply transactionally with a
   `schema_version` row; an unknown/newer version refuses to start, an older
   readable one is supported (apply pending migrations forward).
2. **And** a corrupt/half-migrated projection triggers rebuild-from-intent-log
   rather than refuse-to-start (detection + a clean rebuild entry point; the
   actual replay→apply is Story 1.7/runtime).
3. **And** `orders/trades/positions/funds/risk_events/audit` tables exist with
   `UNIQUE(client_ref)` on `orders`.

## Tasks

- [x] `include/broker_exec/store/store.hpp` — the binding `Store` API (consumed by
      Story 1.7): `open`, `open_or_rebuild` + nested `OpenOutcome`,
      `schema_version`, `insert_order`/`upsert_order`/`find_order`/`all_orders`,
      `insert_trade`/`upsert_position`/`find_position`, funds/risk/audit
      insert+query, `reset`. `Ok` = empty success struct (not `std::monostate`).
- [x] Projection-local row structs (`Funds`, `RiskEvent`, `AuditRecord`) for
      tables the domain layer does not model. Domain `Order`/`Trade`/`Position`
      are the row types for those tables.
- [x] `src/store/sqlite_util.hpp` — module-private RAII: `Statement` (always
      `sqlite3_finalize`), fluent typed binds (`text()`/`i64()`/`bind_status()`),
      `prepare`/`step_done`/`exec`, and a uniform raw-code → `errors::Error`
      mapper with safe fixed messages.
- [x] `src/store/store.cpp` — open (WAL + `synchronous=FULL` PRAGMAs), numbered
      transactional migrations behind `PRAGMA user_version`, newer-version
      refuse-to-start, half-migration/corruption detection, `reset()`
      drop+recreate, and all parameterized CRUD.
- [x] `src/store/store_test.cpp` — Catch2 tests (see Dev Notes for the matrix).
- [x] `src/store/CMakeLists.txt` — `broker_exec_store` STATIC + alias
      `broker_exec::store` + `broker_exec_store_tests` with `add_test`. Mirrors
      `src/platform`.

## Dev Notes

### Migration mechanism (NFR-4)

- The schema stamp is **`PRAGMA user_version`** — a 32-bit int reserved in the
  SQLite file header for exactly this. It avoids a bootstrap table (no
  "does the version table exist yet?" chicken-and-egg) and reads atomically. It
  is the `schema_version` row the AC calls for.
- `kCurrentSchemaVersion = 1`. Migrations are an **ordered, append-only** array;
  `kMigrations[i]` takes a database from version `i` to `i+1`. A shipped
  migration's SQL is **never edited** — a schema change adds a new entry.
- Each migration runs inside its own `BEGIN IMMEDIATE … COMMIT` **together with
  the `user_version` bump**, so a crash mid-apply leaves the database either
  fully at N or fully at N-1 — never half-applied on disk (transactional, AC-1).
- Open logic: read `user_version`; `> current` → refuse to start (typed
  `Internal`/`DoNotRetry` Error — NFR-4 visible fail); `≤ current` → apply the
  missing migrations forward (older readable is supported); `== current` → no-op
  (idempotent re-open).

### Corruption / half-migration detection → rebuild (AC-2)

The projection is a **derived** read-model (source of truth = the intent log,
Story 1.5), so a damaged projection is *recoverable* rather than fatal:

- **Physical corruption** is probed with `PRAGMA quick_check` on open (a fresh
  `sqlite3_open` does not touch pages, so corruption is otherwise latent until a
  read). A non-"ok" verdict — or a read that itself fails (corruption-shaped) —
  is treated as recoverable on the `open_or_rebuild` path.
- **Half-migration** is detected structurally: at a non-zero `user_version`,
  every table the schema promises must exist (queried from `sqlite_master`); a
  missing table ⇒ half-migrated.
- `open()` (strict) returns a typed Error for either condition.
  `open_or_rebuild()` instead calls **`reset()`** (drop every table + rebuild the
  current schema) and returns `OpenOutcome{ store, needs_rebuild=true }` so the
  caller replays the intent log into the clean store. The replay→apply
  composition itself is Story 1.7/runtime — this story owns *detection* + the
  clean rebuild entry point only.
- A **newer** `user_version` is NOT corruption — it is an operator/deployment
  fault — so `open_or_rebuild` still hard-refuses it (does not wipe data).

### `OpenOutcome` nesting (compile note for the integrator)

`OpenOutcome` has a by-value `Store store` member, which requires the enclosing
`Store` to be a *complete* type. It is therefore **declared** inside `Store`
(`struct OpenOutcome;`) and **defined out-of-line** (`struct Store::OpenOutcome
{ … };`) below the class. The member-function declaration
`static Result<OpenOutcome> open_or_rebuild(…)` only needs the type *declared*
(a function declaration does not instantiate `expected<OpenOutcome>`); the .cpp
sees the complete definition. This preserves the binding name `Store::OpenOutcome`
and shape `{ Store store; bool needs_rebuild; }` that Story 1.7 consumes.

### Storage encoding (no float; stable enum text)

- Money/Price are stored as **integer paise** (`int64`), Quantity as an `int64`
  count — never `double`/`float` (binding money-path convention).
- Enums (`Side`/`OrderType`/`Product`/`OrderState`) are stored as their
  `domain::to_string()` **text** (the NFR-8 stable names) and decoded back in the
  store (the store cannot edit `domain`, so the parse helpers live here).
- All SQL is **parameterized** (`sqlite3_bind_*` via the fluent `Statement`
  binder). The only string-formatted SQL is the `PRAGMA user_version = N` literal
  and `DROP TABLE <name>` over the fixed internal table-name list — never a
  caller value.

### Idempotency / dedupe semantics

- `orders.client_ref` is `UNIQUE`. `insert_order` maps a UNIQUE violation to
  `ErrorCategory::DuplicateOrder` (the Story 1.7 backstop); `upsert_order` does
  `INSERT … ON CONFLICT(client_ref) DO UPDATE`.
- `trades.trade_id` is `UNIQUE` and `insert_trade` uses `INSERT OR IGNORE`, so a
  reconcile replay re-seeing a fill does not double-count.
- `positions.symbol` and `funds.account` are `UNIQUE` (one row each; upsert).

### RAII / safety / cross-platform

- `sqlite3*` is owned by a `std::unique_ptr<sqlite3, ConnectionDeleter>`
  (`sqlite3_close_v2`); every `sqlite3_stmt*` by the `Statement` RAII wrapper
  (`sqlite3_finalize` in the destructor) — no leaks under ASan/TSan. The public
  header forward-declares `struct sqlite3` and never includes `<sqlite3.h>`.
- Paths flow through `std::filesystem::path` → `path.string()` for the SQLite
  open (UTF-8/active-codepage filename); `":memory:"` is supported for fast
  tests. No OS APIs, no `#ifdef`. WAL is silently ignored by an in-memory db,
  which is fine (file-backed tests exercise the WAL path).
- Written for MSVC `/W4 /permissive- /WX` and gcc/clang
  `-Wall -Wextra -Wpedantic -Werror -Wshadow`: explicit `int`↔`sqlite3_int64`
  casts (no narrowing), `[[nodiscard]]` results checked, total enum decoding,
  2-space/100-col clang-format. One canonical SQLite `reinterpret_cast` (text
  column) carries a focused `NOLINT` (clang-tidy is advisory here —
  `WarningsAsErrors: ''`).

### Tests (`src/store/store_test.cpp`)

- (a) fresh open creates all six tables at `schema_version == 1`.
- (b) re-open is idempotent (still v1, prior row preserved — no re-migration).
- (c) a forced future `user_version` (999) → `open` AND `open_or_rebuild` both
  return `Internal`/`DoNotRetry` Error (newer = refuse, not rebuild).
- (d) `insert_order` twice on one `client_ref` → second is `DuplicateOrder`;
  `upsert_order` updates in place; `find_order` round-trips a `domain::Order`
  byte-for-byte; missing ref → `nullopt` (not an error).
- (e) `reset()` empties and re-creates a usable schema.
- (f) `open_or_rebuild` on a half-migrated db (a table dropped under the v1
  stamp) recovers with `needs_rebuild=true` and the dropped table restored; on a
  clean fresh db it reports `needs_rebuild=false`.
- trades/positions/funds/risk_events/audit insert + read-back (incl. trade
  idempotency on `trade_id`).
- Temp files use a RAII `TempDb` that also removes the `-wal`/`-shm` sidecars;
  `:memory:` is used where no reopen is needed.

### Scope boundary

Owned paths only: `include/broker_exec/store/*.hpp`, `src/store/*`. Top-level
`CMakeLists.txt`, `tests/`, and other modules are NOT edited — the orchestrator
adds `add_subdirectory(src/store)` at the integration point. Did not run
cmake/conan/build (orchestrator builds centrally).

## Completion Record

Files created:

- `include/broker_exec/store/store.hpp` — `Store` (binding API), `Ok`,
  `Store::OpenOutcome`, `Funds`/`RiskEvent`/`AuditRecord` row structs.
- `src/store/sqlite_util.hpp` — module-private RAII `Statement`, prepare/step/
  exec helpers, raw-code → `errors::Error` mapper.
- `src/store/store.cpp` — open/migrate/version-gate/corruption-detect/reset +
  parameterized CRUD; numbered transactional migrations.
- `src/store/store_test.cpp` — Catch2 tests (the matrix above).
- `src/store/CMakeLists.txt` — `broker_exec_store` target + tests (mirrors
  `src/platform`).

Integration note for the orchestrator: add `add_subdirectory(src/store)` to the
top-level `CMakeLists.txt` integration point (alongside `src/platform`,
`src/domain`, `src/errors`, …). `find_package(SQLite3 REQUIRED)` is already
present at top level (line 39). No other module is touched.

Verification: not built locally by design (no compiler on the dev host;
orchestrator owns the central build). Code is written to the established module
pattern and the `/W4 /WX` + gcc/clang-strict + ASan/TSan bar; the
incomplete-type nesting of `OpenOutcome` was resolved with an out-of-line
definition (see Dev Notes) to keep the binding name and stay portable across
MSVC/gcc/clang.
