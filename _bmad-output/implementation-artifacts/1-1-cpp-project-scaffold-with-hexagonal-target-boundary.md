# Story 1.1: C++ project scaffold with hexagonal target boundary

Status: done
Epic: 1 — Order-Safety Substrate

## Story

As a platform maintainer,
I want a CMake + Conan C++20 project with the layered target structure and CI,
So that every later story builds on an enforced architecture and green pipeline.

## Acceptance Criteria

1. Given a clean checkout, the documented CMake+Conan configure/build builds on gcc and clang (Debug+Release) with `-Wall -Wextra -Werror`.
2. `include/broker_exec/` + `src/{domain,ports,adapters,...}` exist as separate CMake targets where the `domain` target links no adapter/transport target (verified by a build-time check).
3. CI runs clang-format, clang-tidy, ASan/UBSan/TSan builds, and `ctest`, all green on an empty placeholder test.

## Tasks

- [x] Conan v2 recipe (`conanfile.py`) — Catch2 now; transport deps added per-module later.
- [x] Top-level `CMakeLists.txt`: C++20, module path, compile_commands export, layered `add_subdirectory` integration point.
- [x] `cmake/CompilerWarnings.cmake` — `-Wall -Wextra -Wpedantic -Werror` (gcc/clang) / `/W4 /permissive- /WX` (MSVC) via an INTERFACE target linked PRIVATE.
- [x] `cmake/Sanitizers.cmake` — opt-in ASan/UBSan/TSan (gcc/clang only).
- [x] `cmake/HexagonalBoundary.cmake` — `enforce_no_dependency()` BFS over the transitive link graph; configure-time FATAL_ERROR on violation (import-linter analog).
- [x] Layered targets: `broker_exec::domain` (STATIC, pure), `broker_exec::ports` (INTERFACE), `broker_exec::adapters` (INTERFACE, the only SDK-importing layer).
- [x] Placeholder `ctest` over `broker_exec::domain::library_version()`.
- [x] `.clang-format` (Google/c++20), `.clang-tidy` (bugprone/cppcoreguidelines/...).
- [x] `.github/workflows/ci.yml` — gcc+clang × Debug+Release, ASan+UBSan / TSan, clang-format, clang-tidy.
- [x] `docs/building.md` + `scripts/build.ps1` / `scripts/build.sh`.

## Dev Notes / Conventions (binding for all later stories)

- **Namespaces:** `broker_exec::<module>`. Public headers under `include/broker_exec/<module>/`, sources under `src/<module>/`.
- **CMake target per module:** `broker_exec_<module>` + alias `broker_exec::<module>`; link `broker_exec_warnings` + `broker_exec_sanitizers` PRIVATE.
- **No floating point in any money/price path** (lint + review enforced); fixed-point integer paise from Story 1.2.
- **Hexagonal boundary:** `domain`/`ports` may never link `adapters`; broker SDKs live only in `adapters`.
- **Module-local unit tests:** each module defines `broker_exec_<module>_tests` (Catch2::Catch2WithMain) under `if(BROKER_EXEC_BUILD_TESTS)` and `add_test`s it — keeps modules self-contained for parallel work. `tests/` is reserved for cross-module/integration suites (conformance kit, SIGKILL harness).

## Completion Record

- Local verification (Windows / MSVC 19.40, VS 2022): `conan install` → `cmake --preset conan-default` → `cmake --build --preset conan-release` → `ctest --preset conan-release` = **1/1 passed**.
- Boundary check observed at configure: `Boundary OK: broker_exec_domain does not link broker_exec_adapters`.
- gcc/clang + sanitizer matrix is the GitHub Actions canonical verification (`.github/workflows/ci.yml`), runs on push.
