# Story 1.4: Typed error taxonomy with suggested actions

Status: review
Epic: 1 — Order-Safety Substrate

## Story

As a strategy author,
I want broker/library errors normalized to typed categories with a suggested action,
So that strategy code never parses raw broker text. (FR-25)

## Acceptance Criteria

1. **Given** the error model, **when** any failure arises, **then** it maps to a
   stable category carrying a `SuggestedAction` enum (retry-safe / do-not-retry /
   reconcile-first / block-strategy / re-establish-session / cancel / square-off /
   raise-alert).
2. **And** errors are returned as `expected<T, Error>` internally and never
   thrown across the strategy boundary.
3. **And** a unit test asserts representative raw errors map to the expected
   category + action.

## Tasks

- [x] `include/broker_exec/errors/error.hpp` — `ErrorCategory`, `SuggestedAction`,
      `Error` (the binding shared type contract for parallel ports stories).
- [x] `to_string(ErrorCategory)` / `to_string(SuggestedAction)` — stable,
      log/serialization-friendly names (NFR-8 contract).
- [x] `default_action_for(ErrorCategory)` — broker-neutral baseline action map.
- [x] `make_error(...)` factory using the default action.
- [x] `classify_http(int status, std::string_view body)` and
      `classify(std::string_view broker_code, std::string_view message)` — the
      single place raw broker/transport text is parsed (redaction-safe; the body
      is never copied into the Error).
- [x] `include/broker_exec/expected.hpp` — minimal header-only `expected<T,E>`
      + `unexpected<E>` (C++20-safe; see Dev Notes for the decision).
- [x] `include/broker_exec/result.hpp` — project-wide
      `broker_exec::Result<T> = expected<T, errors::Error>` alias + `fail()` helper.
- [x] `src/errors/error.cpp` — implementations.
- [x] `src/errors/error_test.cpp` — co-located Catch2 unit tests.
- [x] `src/errors/CMakeLists.txt` — `broker_exec_errors` STATIC + alias
      `broker_exec::errors` + `broker_exec_errors_tests` exe under
      `if(BROKER_EXEC_BUILD_TESTS)` with `add_test`. Mirrors `src/platform`.

## Dev Notes

### `expected<>` decision (READ — affects the integrator and the ports story)

`std::expected` is a **C++23** library feature. The project targets **C++20**,
and `<expected>` is not present on every C++20 toolchain in the CI matrix
(MSVC/gcc/clang, Debug+Release+sanitizers). To guarantee an identical build on
all of them without a fragile feature-test gate, this story ships its own minimal
**`broker_exec::expected<T,E>`** (`include/broker_exec/expected.hpp`).

- Subset implemented: `has_value()`, `explicit operator bool`, `value()`,
  `error()`, `operator*`, construction from a value or from `unexpected(E)`,
  plus copy/move/assign with correct active-alternative lifetime management
  (manual union + placement-new — exercised by the test under ASan).
- **Member names match `std::expected`** on purpose: a future move to C++23 can
  alias `expected = std::expected` with zero caller changes.
- The alias lives in `namespace broker_exec` (NOT `errors`):
  `template <class T> using Result = expected<T, errors::Error>;`.
- Error state is constructed with `broker_exec::unexpected(err)` or the
  `broker_exec::fail(err)` convenience wrapper.

### No-throw policy

Fallible internal calls return `Result<T>`; errors are values, never exceptions.
Nothing throws across the strategy-facing boundary (conventions.md "Errors").

### Taxonomy / action mapping

Categories and the action vocabulary follow
`_bmad-output/specs/.../error-taxonomy.md` (CAP-13/CAP-14):

- Retry-safety coupling: an unqualified `Timeout` defaults to `ReconcileFirst`
  (the safe choice for dangerous place/modify/cancel ops — never a blind repeat);
  a read-path caller can still treat `Transient`/`RateLimited` as `RetrySafe`.
- `classify_http` keys on the HTTP status (the broker-neutral transport signal)
  and uses the body only as a refinement hint — the body is **never** stored on
  the `Error` (it can carry tokens); `broker_code` is just `"HTTP <status>"`.
- `classify` preserves the short `broker_code` and derives a minimal,
  redaction-safe message — it does not echo the raw broker message verbatim.
- CAP-13 cross-broker equivalence is asserted: two broker dialects for the same
  condition (insufficient funds) normalize to the same category + action.

### Cross-platform / warnings

C++20 stdlib only (`<string>`, `<string_view>`, `<utility>`, `<new>`,
`<type_traits>`). No OS APIs, no `#ifdef`. `#pragma once`. 2-space / 100-col
clang-format. Written to compile clean under MSVC `/W4 /permissive- /WX` and
gcc/clang strict (`-Wall -Wextra -Wpedantic -Werror`): no narrowing,
signed/unsigned compares use `std::size_t`, every enum switch is total with a
trailing fallback `return` (no C4715), accessors are caller-checked `noexcept`.

### Scope boundary

Owned paths only: `include/broker_exec/errors/*.hpp`, `include/broker_exec/expected.hpp`,
`include/broker_exec/result.hpp`, `src/errors/*`. The top-level `CMakeLists.txt`
is **not** edited here — the orchestrator adds `add_subdirectory(src/errors)` at
the integration point. Did not run cmake/conan/build (orchestrator builds
centrally).

## Completion Record

Files created:

- `include/broker_exec/errors/error.hpp` — taxonomy types + function declarations.
- `include/broker_exec/expected.hpp` — minimal `expected<T,E>` + `unexpected<E>`.
- `include/broker_exec/result.hpp` — `broker_exec::Result<T>` alias + `fail()`.
- `src/errors/error.cpp` — `to_string`, `default_action_for`, `make_error`,
  `classify_http`, `classify`.
- `src/errors/error_test.cpp` — Catch2 tests (taxonomy mapping, redaction-safety,
  CAP-13 equivalence, Result no-throw, expected<> lifetime).
- `src/errors/CMakeLists.txt` — `broker_exec_errors` target + tests.

Integration note for the orchestrator: add `add_subdirectory(src/errors)` to the
top-level `CMakeLists.txt` integration point (alongside `src/platform`,
`src/domain`, ...). No other module is touched.

Verification: not built locally by design (orchestrator owns the central build).
Code is written to the established module pattern (mirrors `src/platform`) and to
the `/W4 /WX` + gcc/clang-strict bar.
