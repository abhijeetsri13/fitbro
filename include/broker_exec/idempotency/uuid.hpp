#pragma once

// broker_exec::idempotency — concrete UUID v4 generators (Story 1.7).
//
// Two implementations of the UuidGenerator seam declared in idempotency.hpp:
//   * RandomUuidGenerator      — production default; seeds a std::mt19937_64 from
//                                std::random_device. NON-deterministic by design.
//   * SeededUuidGenerator       — deterministic; seeds mt19937_64 from a caller
//                                fixed seed. For tests (and replay-style determinism)
//                                so client-refs are reproducible.
//
// Both produce a canonical RFC-4122-shaped UUID v4 string: 8-4-4-4-12 lowercase
// hex with the version nibble forced to 4 and the variant bits to 10xx. The
// uniqueness/quality here is sufficient for a client-ref nonce — it is paired
// with the deterministic signal signature and the UNIQUE(client_ref) backstop.
//
// CROSS-PLATFORM: <random> only (std::random_device, std::mt19937_64). No OS
// APIs, no time-based seeding, no `#ifdef`.

#include <cstdint>
#include <random>
#include <string>

#include "broker_exec/idempotency/idempotency.hpp"

namespace broker_exec::idempotency {

// Format the two 64-bit halves of a 128-bit value as a canonical UUID v4 string
// (8-4-4-4-12 lowercase hex), forcing the version and variant bits. Exposed so
// both generators share one formatter and tests can assert exact bytes.
[[nodiscard]] std::string format_uuid_v4(std::uint64_t hi, std::uint64_t lo);

// Production generator: std::random_device-seeded std::mt19937_64. Each next()
// draws two 64-bit words. Non-deterministic (no fixed seed, never time-based).
class RandomUuidGenerator final : public UuidGenerator {
 public:
  RandomUuidGenerator();

  [[nodiscard]] std::string next() override;

 private:
  std::mt19937_64 engine_;
};

// Deterministic generator: std::mt19937_64 seeded from a fixed value. Same seed
// + same call count -> same UUID sequence. For deterministic tests/replay.
class SeededUuidGenerator final : public UuidGenerator {
 public:
  explicit SeededUuidGenerator(std::uint64_t seed) : engine_(seed) {}

  [[nodiscard]] std::string next() override;

 private:
  std::mt19937_64 engine_;
};

}  // namespace broker_exec::idempotency
