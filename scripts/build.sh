#!/usr/bin/env bash
# Convenience build wrapper (Linux/macOS, Ninja single-config).
# Usage: ./scripts/build.sh [Release|Debug]
set -euo pipefail
CONFIG="${1:-Release}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

conan install . -of build --build=missing -s compiler.cppstd=20 \
  -s "build_type=${CONFIG}" \
  -c tools.cmake.cmaketoolchain:generator=Ninja
cmake -B build -S . -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="build/build/${CONFIG}/generators/conan_toolchain.cmake" \
  -DCMAKE_BUILD_TYPE="${CONFIG}"
cmake --build build
ctest --test-dir build --output-on-failure
