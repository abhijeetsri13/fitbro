// BROKER-IDENTIFIER SOURCE SCAN (Story 6.3, AC-3) — the C++ analog of the epics'
// "code scan".
//
// WHAT IT DOES: reads the committed source of the portable strategy translation
// unit at TEST TIME and fails if any broker's name appears anywhere in it —
// code, identifier, include or comment. The paths are injected by CMake
// (`PORTABLE_STRATEGY_SRC` / `PORTABLE_STRATEGY_HDR` compile definitions) so the
// test always reads the exact files that were compiled into this binary, on
// every platform, with no path assembled by hand.
//
// WHY A SOURCE SCAN AND NOT JUST A COMPILE CHECK: the strategy already compiles
// without linking an adapter, which proves it has no broker TYPE dependency. It
// does not prove the absence of the thing that actually erodes portability — a
// name. `if (broker_name == "...")`, a comment saying "this only works on X", a
// symbol-format assumption smuggled in as a string literal: none of those break
// the build, all of them make the next broker a code change. The scan catches the
// name, which is the earliest observable symptom.
//
// NON-VACUITY IS ASSERTED, NOT ASSUMED. A scan that silently read an empty file,
// or the wrong file, would pass forever. So the test proves it read real content
// (non-trivial length) and that the content is the strategy it claims to scan
// (it contains a known anchor), before it concludes anything from the absence of
// a substring.
//
// DETERMINISTIC: pure file read + lowercase + substring search. No globbing, no
// directory walking, no build-order dependence.
//
// Cross-platform: C++20 standard library only (std::filesystem paths, never a
// hand-built separator). No OS APIs, no `#ifdef`, no float.

#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#ifndef PORTABLE_STRATEGY_SRC
#error "PORTABLE_STRATEGY_SRC must be injected by CMake (see tests/CMakeLists.txt)"
#endif
#ifndef PORTABLE_STRATEGY_HDR
#error "PORTABLE_STRATEGY_HDR must be injected by CMake (see tests/CMakeLists.txt)"
#endif

namespace {

// The forbidden substrings, matched case-insensitively. Broker names only — this
// is the list that grows when a broker is added, and the ONE place it grows.
constexpr std::string_view kForbidden[] = {"kite", "kotak", "zerodha"};

// A string that MUST be present, so an empty/misdirected read fails loudly
// instead of passing by absence.
constexpr std::string_view kAnchor = "brokerport";

// Shortest plausible real content for either file; well under their actual size
// and well over anything a truncated read would produce.
constexpr std::size_t kMinimumSourceBytes = 512;

[[nodiscard]] std::string read_all(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::string{};
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

[[nodiscard]] std::string to_lower(std::string text) {
  for (char& c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

// Report the 1-based line number of the first occurrence, for a message an
// author can act on directly.
[[nodiscard]] std::size_t line_of(const std::string& lowered, std::size_t offset) {
  std::size_t line = 1;
  for (std::size_t i = 0; i < offset && i < lowered.size(); ++i) {
    if (lowered[i] == '\n') {
      ++line;
    }
  }
  return line;
}

void scan_one(const std::filesystem::path& path) {
  UNSCOPED_INFO("scanning: " << path.string());

  REQUIRE(std::filesystem::exists(path));

  const std::string source = read_all(path);
  // NON-VACUITY, PART 1: we really read the file.
  REQUIRE(source.size() >= kMinimumSourceBytes);

  const std::string lowered = to_lower(source);
  // NON-VACUITY, PART 2: it really is the strategy translation unit — the one
  // thing every part of it must mention is the port it is written against.
  REQUIRE(lowered.find(kAnchor) != std::string::npos);

  // AC-3 ITSELF.
  for (const std::string_view needle : kForbidden) {
    const std::size_t at = lowered.find(needle);
    if (at != std::string::npos) {
      UNSCOPED_INFO("broker identifier '" << needle << "' found at line " << line_of(lowered, at)
                                          << " — a portable strategy may not name a broker. "
                                             "If you need broker-specific behaviour, widen "
                                             "ports::BrokerPort or the capability model instead.");
    }
    CHECK(at == std::string::npos);
  }
}

}  // namespace

TEST_CASE("portability: the strategy translation unit names no broker (AC-3)",
          "[portability][ac3][scan]") {
  SECTION("implementation") {
    scan_one(std::filesystem::path(PORTABLE_STRATEGY_SRC));
  }
  SECTION("header") {
    scan_one(std::filesystem::path(PORTABLE_STRATEGY_HDR));
  }
}

TEST_CASE("portability: the scan itself would catch a broker identifier",
          "[portability][ac3][scan]") {
  // A guard test for the guard: if `to_lower` or the search were broken, the
  // scan above would pass on ANY input, including a file full of broker names.
  // This pins the detector against synthetic content so the AC-3 result means
  // something.
  const std::string planted =
      "// this line mentions Kite\nvoid f() { /* KOTAK */ }\n// and Zerodha too\n";
  const std::string lowered = to_lower(planted);
  for (const std::string_view needle : kForbidden) {
    UNSCOPED_INFO("detector must find: " << needle);
    CHECK(lowered.find(needle) != std::string::npos);
  }

  // ...and does not fire on ordinary portable prose.
  const std::string clean = to_lower("// place the hedge, then the short leg\n");
  for (const std::string_view needle : kForbidden) {
    CHECK(clean.find(needle) == std::string::npos);
  }
}
