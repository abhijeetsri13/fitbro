# Story 1.5: Write-ahead intent log (fsync, hash-chained, replayable)

Status: review
Epic: 1 — Order-Safety Substrate

## Story

As an operator,
I want every order intent durably recorded before any send,
So that a crash never produces a duplicate or an un-enumerable order. (FR-8, NFR-1)

## Acceptance Criteria

1. **Given** an order about to be sent, **when** the intent is recorded, **then**
   the record (monotonic `seq` + `client_ref` + prev-record SHA-256) is appended
   as one JSON line and `fsync`'d (via `platform::durable_sync`) **before**
   returning, so the caller can socket-send only after durability.
2. **And** on boot the log replays head→tail, rebuilding the in-memory
   `client_ref` index and enumerating every order that might have been sent.
3. **And** tampering with any past record is detectable via the broken hash
   chain (replay returns an `ErrorCategory::Internal` error naming the first bad
   seq).

## Tasks

- [x] `include/broker_exec/intentlog/intent_log.hpp` — binding public contract:
      `IntentOp`, `to_string(IntentOp)`, `intent_op_from_string`, `kSchemaVersion`,
      `IntentRecord`, `IntentLog` (`open`/`append`/`replay`/`last_for`/`next_seq`).
- [x] `src/intentlog/sha256.{hpp,cpp}` — vendored, public-domain-style SHA-256
      (FIPS 180-4), pure C++20, streaming `update()` + one-shot `sha256_hex()`.
- [x] `src/intentlog/intent_log.cpp` — canonical serialization, hash chaining,
      append (single `fflush` + single `durable_sync`), head→tail replay with
      three chain checks (seq monotonicity, prev_hash linkage, recomputed hash).
- [x] `src/intentlog/sha256_test.cpp` — NIST known-answer vectors (empty, "abc",
      448-bit message, 1e6 'a') + streaming-equals-one-shot.
- [x] `src/intentlog/intent_log_test.cpp` — (a) append N → replay rebuilds index
      and enumerates; (b) `last_for` latest-wins; (c) byte-tamper → replay detects
      broken chain at seq 1; (d) reopen + replay after "restart" yields the same
      index + hashes and continues the chain (durability); (e) `IntentOp`
      wire-name round-trip. Temp files via `temp_directory_path()`, cleaned up.
- [x] `src/intentlog/CMakeLists.txt` — `broker_exec_intentlog` STATIC + alias
      `broker_exec::intentlog` + `broker_exec_intentlog_tests` with `add_test`.

## Dev Notes

### Canonical-hash format (FROZEN — Story 1.7 replays against this)

The record `hash` is SHA-256 over a deterministic byte stream of every field
EXCEPT `hash`. Variable-length fields are length-prefixed (`<len>:<value>`) so no
two distinct field sets can collide via concatenation. Field order and framing
are frozen; any change is a `kSchemaVersion` bump.

```
canonical(record) =
  "v" + dec(schema_version)              + "\n"
  "s" + dec(seq)                         + "\n"
  "o" + to_string(op)                    + "\n"
  "t" + dec(wall_ts_ns)                  + "\n"
  "c" + dec(len(client_ref))   + ":" + client_ref   + "\n"
  "p" + dec(len(payload_json)) + ":" + payload_json  + "\n"
  "h" + dec(len(prev_hash))    + ":" + prev_hash     + "\n"
```

`prev_hash` is the previous record's `hash`, or the literal `"GENESIS"` for
seq 1. Because `prev_hash` is part of the canonical bytes, every record's hash
transitively commits to the entire log prefix: tampering with any past record
changes its hash, which (a) fails its own recomputed-hash check and (b) breaks
the next record's `prev_hash` linkage — caught on replay, first bad seq named.

The **hash is over the canonical form, never the JSON text** — so JSON key order
or whitespace can never affect integrity. The on-disk JSON line is a *projection*
(keys: `schema_version`, `seq`, `client_ref`, `op`, `payload`, `wall_ts_ns`,
`prev_hash`, `hash`). `payload_json` is stored as a JSON *string* (escaped), so
replay round-trips the exact bytes back and the recomputed hash always matches.

`IntentOp` wire names (also part of the hashed bytes): `place_order`,
`modify_order`, `cancel_order`, `square_off`, `result`, `child_slice`.

### Durability (the single fsync on the hot path)

`append()` writes one line, then exactly one `std::fflush` (C buffer → OS) +
one `platform::durable_sync(portable_fileno(file_))` (OS → stable storage), and
only then commits in-memory state (`last_hash_`, `next_seq_`, index) and returns.
A failure at any step returns before committing, so the in-memory chain never
runs ahead of disk. Story 1.9's `dispatch()` may socket-send only after a
successful `append()`. File is C `std::FILE*` opened `"ab"` (append, no
truncate); replay reads a separate `"rb"` stream so the append cursor is
untouched. No OS APIs / `#ifdef` here — durability is entirely behind the
`broker_exec::platform` seam (binding cross-platform convention).

### void/Result decision

Per `expected.hpp` there is no `expected<void>`. Fallible calls that "return
nothing" use a value-bearing `Result<T>`: `open()` returns `Result<IntentLog>`,
`append()` returns `Result<IntentRecord>` (the fully-populated record), and
`replay()` returns `Result<std::vector<IntentRecord>>` (also rebuilds the index
as a side effect). Errors are values via `broker_exec::fail(make_error(...))`,
never thrown (no-throw-across-the-boundary policy). The pure in-memory queries
`last_for()` and `next_seq()` are infallible (`std::optional` / plain value).

### Replay / boot semantics

`open()` does NOT auto-replay (binding API). Boot sequence is `open()` then
`replay()` once before the first `append()`. Replay tolerates a missing file
(empty index, the correct cold-start state) and a trailing blank line. On a
broken chain it returns `ErrorCategory::Internal` with the first bad seq in the
message; it never partially-applies a tampered tail to the index.

### Cross-platform / warnings

C++20 stdlib + `nlohmann_json` + the vendored SHA-256 only. `std::filesystem`
for paths, `std::FILE*` for I/O so `portable_fileno`/`durable_sync` apply on
every OS. No `<unistd.h>`, no `#ifdef`, `#pragma once`, 2-space / 100-col
clang-format. Written to compile clean under MSVC `/W4 /permissive- /WX` and
gcc/clang strict (`-Wall -Wextra -Wpedantic -Werror -Wshadow`): explicit
`static_cast` on all `uint32_t`/`size_t`/`int64_t`/`unsigned char` conversions,
`duration_cast` to `nanoseconds` for the timestamp (no narrowing), total enum
switch with a trailing fallback `return`, move-only `IntentLog` with an
RAII `std::fclose` in the destructor / move-assign.

### Scope boundary

Owned paths only: `include/broker_exec/intentlog/*.hpp`, `src/intentlog/*`. The
top-level `CMakeLists.txt`, `tests/`, and other modules are **not** edited — the
orchestrator adds `add_subdirectory(src/intentlog)` at the integration point.
Did not run cmake/conan/build (orchestrator builds centrally).

## Completion Record

Files created:

- `include/broker_exec/intentlog/intent_log.hpp` — `IntentOp`, `IntentRecord`,
  `IntentLog` public contract + `kSchemaVersion` + op string helpers.
- `src/intentlog/sha256.hpp` — vendored `Sha256` (streaming) + `sha256_hex`.
- `src/intentlog/sha256.cpp` — FIPS 180-4 SHA-256 implementation.
- `src/intentlog/intent_log.cpp` — canonical hashing, append+fsync, replay+verify.
- `src/intentlog/sha256_test.cpp` — NIST known-answer vectors + streaming test.
- `src/intentlog/intent_log_test.cpp` — append/replay/index, tamper-detection,
  durability-across-restart, op round-trip (TestClock + temp dir, cleaned up).
- `src/intentlog/CMakeLists.txt` — `broker_exec_intentlog` target + tests.

Integration note for the orchestrator: add `add_subdirectory(src/intentlog)` to
the top-level `CMakeLists.txt` integration point (after `src/clock`, which
provides `broker_exec_clock` for the test target). No other module is touched.

Verification: not built locally by design (orchestrator owns the central build).
Code mirrors the established module pattern (`src/platform`, `src/clock`) and is
written to the `/W4 /WX` + gcc/clang-strict bar; SHA-256 correctness is pinned by
NIST vectors and the tamper/durability ACs by the co-located Catch2 suite.
