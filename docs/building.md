# Building

Broker-neutral trading execution library — C++20, CMake (≥3.28) + Conan v2.

## Prerequisites

- CMake ≥ 3.28
- A C++20 compiler: GCC 13+, Clang 16+, or MSVC 19.4x (VS 2022)
- Python 3.10+ with Conan v2 (`pip install "conan>=2.0"`)

## First-time Conan setup

```bash
conan profile detect --force
```

## Configure, build, test

### Linux / macOS (Ninja, single-config)

```bash
conan install . -of build --build=missing -s compiler.cppstd=20 \
  -c tools.cmake.cmaketoolchain:generator=Ninja
cmake -B build -S . -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=build/build/Release/generators/conan_toolchain.cmake
cmake --build build
ctest --test-dir build --output-on-failure
```

### Windows (Visual Studio 2022, multi-config)

```powershell
conan install . -of build --build=missing -s compiler.cppstd=20
cmake -B build -S . -G "Visual Studio 17 2022" `
  -DCMAKE_TOOLCHAIN_FILE=build/build/generators/conan_toolchain.cmake
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Convenience wrappers: `scripts/build.ps1` (Windows) and `scripts/build.sh` (Linux/macOS).

## Sanitizers (gcc/clang only)

```bash
cmake -B build -S . -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=build/build/Debug/generators/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Debug -DBROKER_EXEC_SANITIZER=address,undefined
```

## Architecture boundary

The configure step runs `enforce_no_dependency()` (see `cmake/HexagonalBoundary.cmake`):
the `domain` and `ports` layers may not link `adapters`. A violation is a hard
configure error — the build-time analog of an import-linter contract.

## CI

`.github/workflows/ci.yml` runs the canonical verification: gcc + clang ×
Debug + Release, ASan/UBSan + TSan sanitizer builds, clang-format, and
clang-tidy. The SIGKILL durability harness (Story 1.12) is added to this
pipeline when it lands.
