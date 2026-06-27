#include <catch2/catch_test_macros.hpp>

#include "broker_exec/domain/version.hpp"

// Placeholder smoke test proving the toolchain, Conan dependency resolution,
// the domain target, and ctest are all wired correctly. Real domain tests
// replace/augment this from Story 1.2 onward.
TEST_CASE("scaffold: library version is reported", "[scaffold]") {
  REQUIRE(broker_exec::domain::library_version() == "0.1.0");
}
