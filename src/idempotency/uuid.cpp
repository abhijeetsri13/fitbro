#include "broker_exec/idempotency/uuid.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace broker_exec::idempotency {
namespace {

// Lowercase hex digits for fast nibble formatting (no locale, no <iomanip>).
constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                       '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

// Append the low `nibbles` hex digits of `value`, most-significant first.
void append_hex(std::string& out, std::uint64_t value, int nibbles) {
  for (int shift = (nibbles - 1) * 4; shift >= 0; shift -= 4) {
    out += kHex[static_cast<std::size_t>((value >> shift) & 0xFU)];
  }
}

}  // namespace

std::string format_uuid_v4(std::uint64_t hi, std::uint64_t lo) {
  // RFC-4122 v4 bit-fiddling on the 128-bit value (hi:lo):
  //   version nibble (bits 12..15 of the 7th byte, i.e. hi bits 12..15) -> 0100
  //   variant bits   (top two bits of the 9th byte, i.e. lo bits 62..63) -> 10
  hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
  lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

  // Layout: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
  //   hi[63..32] = 8 hex (time-low)   hi[31..16] = 4 hex (time-mid)
  //   hi[15..0]  = 4 hex (time-hi+version)
  //   lo[63..48] = 4 hex (variant+clock-seq)   lo[47..0] = 12 hex (node)
  std::string out;
  out.reserve(36);
  append_hex(out, (hi >> 32) & 0xFFFFFFFFULL, 8);
  out += '-';
  append_hex(out, (hi >> 16) & 0xFFFFULL, 4);
  out += '-';
  append_hex(out, hi & 0xFFFFULL, 4);
  out += '-';
  append_hex(out, (lo >> 48) & 0xFFFFULL, 4);
  out += '-';
  append_hex(out, lo & 0xFFFFFFFFFFFFULL, 12);
  return out;
}

RandomUuidGenerator::RandomUuidGenerator() {
  // Seed mt19937_64 from random_device. A small seed sequence pulls enough
  // entropy to seed the 64-bit engine well (random_device yields 32-bit words).
  std::random_device rd;
  std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
  engine_.seed(seq);
}

std::string RandomUuidGenerator::next() {
  const std::uint64_t hi = engine_();
  const std::uint64_t lo = engine_();
  return format_uuid_v4(hi, lo);
}

std::string SeededUuidGenerator::next() {
  const std::uint64_t hi = engine_();
  const std::uint64_t lo = engine_();
  return format_uuid_v4(hi, lo);
}

}  // namespace broker_exec::idempotency
