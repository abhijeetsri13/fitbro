#requires -Version 5.1
# Convenience build wrapper (Windows / MSVC multi-config).
# Usage: ./scripts/build.ps1 [-Config Release|Debug]
param(
  [ValidateSet("Release", "Debug")]
  [string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
  python -m conans.conan install . -of build --build=missing -s compiler.cppstd=20 -s build_type=$Config
  cmake -B build -S . -G "Visual Studio 17 2022" `
    -DCMAKE_TOOLCHAIN_FILE="build/build/generators/conan_toolchain.cmake"
  cmake --build build --config $Config
  ctest --test-dir build -C $Config --output-on-failure
}
finally {
  Pop-Location
}
