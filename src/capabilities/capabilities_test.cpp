#include "broker_exec/capabilities/capabilities.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>

#include "broker_exec/errors/error.hpp"

using broker_exec::capabilities::Capability;
using broker_exec::capabilities::CapabilitySet;
using broker_exec::capabilities::kite_capabilities;
using broker_exec::capabilities::Support;
using broker_exec::capabilities::to_string;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::SuggestedAction;

namespace {

// True if `haystack` contains `needle` (used to assert an error message NAMES a
// specific capability).
[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

}  // namespace

TEST_CASE("supports() only passes for Support::Supported", "[capabilities]") {
  const CapabilitySet set{
      {Capability::PlaceOrder, Support::Supported},
      {Capability::ModifyOrder, Support::Unsupported},
      {Capability::CancelOrder, Support::Unknown},
  };

  CHECK(set.supports(Capability::PlaceOrder));         // Supported -> true
  CHECK_FALSE(set.supports(Capability::ModifyOrder));  // Unsupported -> false
  CHECK_FALSE(set.supports(Capability::CancelOrder));  // Unknown -> false (AC-2)

  CHECK(set.support_of(Capability::PlaceOrder) == Support::Supported);
  CHECK(set.support_of(Capability::ModifyOrder) == Support::Unsupported);
  CHECK(set.support_of(Capability::CancelOrder) == Support::Unknown);
}

TEST_CASE("a default/unspecified capability reads Unknown and is unsupported", "[capabilities]") {
  const CapabilitySet set{
      {Capability::PlaceOrder, Support::Supported},
  };

  // SquareOff was never set in this sparse build.
  CHECK(set.support_of(Capability::SquareOff) == Support::Unknown);
  CHECK_FALSE(set.supports(Capability::SquareOff));

  // A fully default-constructed set is Unknown everywhere.
  const CapabilitySet empty;
  CHECK(empty.support_of(Capability::PlaceOrder) == Support::Unknown);
  CHECK_FALSE(empty.supports(Capability::PlaceOrder));
}

TEST_CASE("require() returns ok for Supported and a typed error otherwise", "[capabilities]") {
  const CapabilitySet set{
      {Capability::PlaceOrder, Support::Supported},
      {Capability::ModifyOrder, Support::Unsupported},
      {Capability::CancelOrder, Support::Unknown},
  };

  SECTION("Supported -> ok") {
    const auto result = set.require(Capability::PlaceOrder);
    REQUIRE(result.has_value());
  }

  SECTION("Unsupported -> NotSupported error naming the capability") {
    const auto result = set.require(Capability::ModifyOrder);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().category == ErrorCategory::NotSupported);
    CHECK(result.error().action == SuggestedAction::DoNotRetry);
    CHECK(contains(result.error().message, to_string(Capability::ModifyOrder)));
  }

  SECTION("Unknown -> NotSupported error (AC-2: unknown == unsupported)") {
    const auto result = set.require(Capability::CancelOrder);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().category == ErrorCategory::NotSupported);
    CHECK(result.error().action == SuggestedAction::DoNotRetry);
    CHECK(contains(result.error().message, to_string(Capability::CancelOrder)));
  }
}

TEST_CASE("require_all() is fail-closed on the first unsupported capability", "[capabilities]") {
  const CapabilitySet set{
      {Capability::PlaceOrder, Support::Supported},
      {Capability::ModifyOrder, Support::Supported},
      {Capability::CancelOrder, Support::Unsupported},
      {Capability::SquareOff, Support::Supported},
  };

  SECTION("all supported -> ok") {
    const std::array<Capability, 3> needed{Capability::PlaceOrder, Capability::ModifyOrder,
                                           Capability::SquareOff};
    const auto result = set.require_all(needed);
    REQUIRE(result.has_value());
  }

  SECTION("mixed -> fails on the FIRST unsupported and names THAT capability") {
    // CancelOrder (unsupported) is reached before SquareOff (supported); the
    // failure must name CancelOrder, not the later items.
    const std::array<Capability, 3> needed{Capability::PlaceOrder, Capability::CancelOrder,
                                           Capability::SquareOff};
    const auto result = set.require_all(needed);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().category == ErrorCategory::NotSupported);
    CHECK(result.error().action == SuggestedAction::DoNotRetry);
    CHECK(contains(result.error().message, to_string(Capability::CancelOrder)));
    CHECK_FALSE(contains(result.error().message, to_string(Capability::SquareOff)));
  }

  SECTION("an unknown capability also fails closed") {
    const std::array<Capability, 2> needed{Capability::PlaceOrder, Capability::BasketMargin};
    const auto result = set.require_all(needed);
    REQUIRE_FALSE(result.has_value());
    CHECK(contains(result.error().message, to_string(Capability::BasketMargin)));
  }
}

TEST_CASE("kite_capabilities() reflects the certified Kite profile", "[capabilities]") {
  const CapabilitySet kite = kite_capabilities();

  // Certified-supported order lifecycle.
  CHECK(kite.supports(Capability::PlaceOrder));
  CHECK(kite.supports(Capability::ModifyOrder));
  CHECK(kite.supports(Capability::CancelOrder));
  CHECK(kite.supports(Capability::SquareOff));

  // Certified-unsupported: Kite has no headless session refresh (Story 2.4).
  CHECK(kite.support_of(Capability::HeadlessSessionRefresh) == Support::Unsupported);
  CHECK_FALSE(kite.supports(Capability::HeadlessSessionRefresh));

  // Genuinely-unverified capability stays Unknown -> reads unsupported until
  // certified (Story 2.14).
  CHECK(kite.support_of(Capability::BasketMargin) == Support::Unknown);
  CHECK_FALSE(kite.supports(Capability::BasketMargin));

  // require() honours the profile.
  CHECK(kite.require(Capability::PlaceOrder).has_value());
  CHECK_FALSE(kite.require(Capability::HeadlessSessionRefresh).has_value());
}
