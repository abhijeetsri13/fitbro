// Conformance kit driver (Story 1.12, FR-37, NFR-3). Runs the reusable
// broker_exec::conformance kit against the adversarial FakeBroker and asserts the
// headline invariant — ZERO duplicate orders across the full fault matrix, with
// UNKNOWN-handling, no-blind-retry, and reconciliation all passing.
//
// This is the Epic-1 instantiation of the kit. The SAME kit (run_conformance) is
// reused unchanged to certify the Kite adapter (Epic 2) and the Kotak Neo adapter
// (Epic 6) by passing a different BrokerFactory — proving the kit gates every
// adapter, not just the fake.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <catch2/catch_test_macros.hpp>
#include <memory>

#include "broker_exec/adapters/fake/fake_broker.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/clock_port.hpp"
#include "conformance_kit.hpp"

namespace conf = broker_exec::conformance;

namespace {

// The fake-broker factory: the kit hands it the scenario's clock + FaultConfig and
// it returns a fresh, fault-configured FakeBroker each time. This is the exact
// shape an Epic-2/6 adapter factory will take (minus the FaultConfig, which a real
// adapter ignores in favor of its recorded fixtures).
conf::BrokerFactory fake_factory() {
  return [](broker_exec::ports::ClockPort& clock, broker_exec::adapters::fake::FaultConfig fault)
             -> std::unique_ptr<broker_exec::ports::BrokerPort> {
    return std::make_unique<broker_exec::adapters::fake::FakeBroker>(clock, fault);
  };
}

}  // namespace

TEST_CASE("conformance: the full fault matrix produces zero duplicate orders",
          "[conformance][fault-matrix][zero-duplicate]") {
  const conf::ConformanceReport report = conf::run_conformance(fake_factory());

  // Scoped, so the reason survives to whichever assertion below actually fires.
  INFO("conformance failures:" << conf::failure_digest(report.failures));
  INFO("conformance SETUP failures:" << conf::failure_digest(report.setup_failures));

  // Setup first. A scenario that could not create its data directory proved
  // nothing about the library, and saying so before the property assertions stops
  // a broken runner reading as a broken zero-duplicate guarantee (#44).
  CHECK(report.setup_failures.empty());

  // Named next: the most specific statement of what actually went wrong, so it is
  // the assertion a reader sees before the arithmetic ones.
  CHECK(report.failures.empty());

  // Every scenario in the matrix ran.
  CHECK(report.scenarios_run > 0);

  // The headline invariant: not a single duplicate order anywhere in the matrix.
  CHECK(report.duplicate_orders == 0);

  // Every scenario passed all three properties (no-blind-retry, UNKNOWN handling,
  // zero duplicates) — i.e. the kit reports a clean gate.
  CHECK(report.scenarios_passed == report.scenarios_run);
  CHECK(report.ok());
}

TEST_CASE("conformance: every scenario in the matrix is exercised", "[conformance][fault-matrix]") {
  const conf::ConformanceReport report = conf::run_conformance(fake_factory());
  // The matrix covers the control + every FaultConfig knob (drop_ack,
  // ack_lost_but_placed, rate_limit, duplicate_fill, out_of_order, delay_ack):
  // seven scenarios. Pin the count so a silently-dropped scenario is caught.
  CHECK(report.scenarios_run == 7);
}
