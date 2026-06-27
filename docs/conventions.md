# Engineering Conventions (binding for all stories)

These are enforced by CI and review. New modules must follow them exactly.

## Layout & naming

- Namespaces: `broker_exec::<module>`.
- Public headers: `include/broker_exec/<module>/*.hpp`. Sources: `src/<module>/*.cpp`.
- One CMake target per module: `broker_exec_<module>` + alias `broker_exec::<module>`.
- Link `broker_exec_warnings` and `broker_exec_sanitizers` **PRIVATE** in every first-party target.
- C++20 (`target_compile_features(... cxx_std_20)`), no compiler extensions.

## Cross-platform (Windows, Linux, macOS) — REQUIRED

The library is **cross-platform**. CI builds and tests on Linux (gcc/clang),
Windows (MSVC), and macOS (clang). To keep it that way:

- **No OS-specific API or `#ifdef _WIN32` / POSIX `#include` outside `src/platform/`.**
  All OS divergence (durable fsync, file permissions, sockets/UDS, process
  control, paths) lives behind the `broker_exec::platform` seam. Business logic
  calls the portable surface.
- Use the C++20 standard library for portability: `std::filesystem` for paths
  (never hand-built `/` or `\` strings), `std::chrono::steady_clock` for
  monotonic time, `std::chrono::system_clock` for wall time (both via the
  injected `ClockPort`, never called directly outside the system Clock impl).
- No assumptions about line endings, path separators, case-sensitivity, or
  `HOME`/`%APPDATA%` — resolve via `std::filesystem` and config.
- Platform-specific deployment artifacts (systemd unit, Windows service) are
  additive and isolated; the engine core must not depend on either.
- The durability primitive is `broker_exec::platform::durable_sync(fd)`
  (POSIX `fsync` / Windows `_commit`). Anything needing on-disk durability uses
  it — do not call `fsync`/`FlushFileBuffers` directly.

## Money / prices

- **No `double`/`float` in any money or price path** (lint + review enforced).
  Fixed-point integer paise (`int64`) via the `Money`/`Price` types (Story 1.2).

## Errors

- Internal fallible calls return `std::expected<T, Error>` (Story 1.4); never
  throw across the strategy-facing boundary.

## Tests

- Each module defines `broker_exec_<module>_tests` (Catch2 `Catch2::Catch2WithMain`)
  under `if(BROKER_EXEC_BUILD_TESTS)` and registers it with `add_test`. This
  keeps modules self-contained so parallel work never collides on a shared test
  target. The top-level `tests/` directory is reserved for cross-module /
  integration suites (conformance kit, SIGKILL durability harness).

## Architecture boundary

- `domain` and `ports` must never link `adapters` (broker SDKs/transports live
  only in `adapters`). Enforced at configure time by
  `enforce_no_dependency()` in `cmake/HexagonalBoundary.cmake`.
- The single broker-mutation path is `dispatch()` (Story 1.9): record → fsync →
  send → record. No store/ledger write outside the main loop.
