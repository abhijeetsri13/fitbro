#pragma once

// A compact, public-domain-style SHA-256 (FIPS 180-4) — pure C++20, no OS APIs
// and no third-party dependency. The intent log hash-chains each record with
// SHA-256 (Story 1.5): tampering with any past record breaks the chain and is
// detectable on replay. This is *not* used for any cryptographic-secret purpose;
// it is an integrity/tamper-evidence checksum over canonical record bytes.
//
// Correctness is pinned by the NIST known-answer vectors in the module tests
// (empty string, "abc", and the 448-bit message). Streaming `update()` lets the
// canonical serializer feed bytes without first concatenating them.
//
// Cross-platform: C++20 standard library only (`<array>`, `<cstdint>`,
// `<cstddef>`, `<string>`, `<string_view>`). No `#ifdef`, no narrowing.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace broker_exec::intentlog {

// Incremental SHA-256. Construct, `update()` zero or more times, then `hex()`
// once to obtain the lowercase 64-char digest. The object is single-shot:
// `hex()` finalizes internal state and must be called at most once.
class Sha256 {
 public:
  Sha256() noexcept;

  // Absorb `len` bytes from `data`.
  void update(const void* data, std::size_t len) noexcept;
  void update(std::string_view data) noexcept;

  // Finalize and return the lowercase hex digest (64 chars). Single-shot.
  [[nodiscard]] std::string hex();

 private:
  void transform(const unsigned char* block) noexcept;

  std::array<std::uint32_t, 8> state_;
  std::array<unsigned char, 64> buffer_;
  std::uint64_t bit_len_;
  std::size_t buffer_len_;
};

// Convenience one-shot: lowercase hex SHA-256 of the whole input.
[[nodiscard]] std::string sha256_hex(std::string_view data);

}  // namespace broker_exec::intentlog
