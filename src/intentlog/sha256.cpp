#include "sha256.hpp"

#include <cstddef>
#include <cstdint>

// SHA-256 (FIPS 180-4). Compact reference implementation; pinned by the NIST
// known-answer vectors in sha256_test.cpp. Pure C++ — portable across the CI
// matrix with no narrowing under /W4 /WX and gcc/clang -Wall -Wextra -Wpedantic.

namespace broker_exec::intentlog {

namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t x, std::uint32_t n) noexcept {
  return (x >> n) | (x << (32u - n));
}

}  // namespace

Sha256::Sha256() noexcept
    : state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu,
             0x1f83d9abu, 0x5be0cd19u},
      buffer_{},
      bit_len_(0),
      buffer_len_(0) {}

void Sha256::transform(const unsigned char* block) noexcept {
  std::array<std::uint32_t, 64> w{};
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const void* data, std::size_t len) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < len; ++i) {
    buffer_[buffer_len_++] = bytes[i];
    bit_len_ += 8;
    if (buffer_len_ == 64) {
      transform(buffer_.data());
      buffer_len_ = 0;
    }
  }
}

void Sha256::update(std::string_view data) noexcept { update(data.data(), data.size()); }

std::string Sha256::hex() {
  // Append the 0x80 padding byte, then zeros up to a 56-byte boundary, then the
  // 64-bit big-endian message length in bits.
  const std::uint64_t total_bits = bit_len_;
  unsigned char pad = 0x80;
  update(&pad, 1);
  pad = 0x00;
  while (buffer_len_ != 56) {
    update(&pad, 1);
  }

  std::array<unsigned char, 8> length_bytes{};
  for (std::size_t i = 0; i < 8; ++i) {
    length_bytes[i] = static_cast<unsigned char>((total_bits >> (56u - i * 8u)) & 0xffu);
  }
  // buffer_len_ is exactly 56 here; the 8 length bytes fill the block to 64,
  // then a single final transform produces the digest. (Written directly into
  // buffer_, not via update(), so bit_len_ is not perturbed after finalization.)
  for (std::size_t i = 0; i < 8; ++i) {
    buffer_[buffer_len_++] = length_bytes[i];
  }
  transform(buffer_.data());
  buffer_len_ = 0;

  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (std::size_t i = 0; i < 8; ++i) {
    const std::uint32_t word = state_[i];
    for (std::size_t shift = 0; shift < 4; ++shift) {
      const auto byte =
          static_cast<unsigned char>((word >> (24u - shift * 8u)) & 0xffu);
      out.push_back(kHexDigits[(byte >> 4) & 0x0f]);
      out.push_back(kHexDigits[byte & 0x0f]);
    }
  }
  return out;
}

std::string sha256_hex(std::string_view data) {
  Sha256 hasher;
  hasher.update(data);
  return hasher.hex();
}

}  // namespace broker_exec::intentlog
