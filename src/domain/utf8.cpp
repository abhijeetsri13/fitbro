#include "broker_exec/domain/utf8.hpp"

#include <cstddef>
#include <string>
#include <string_view>

// THE SINGLE IMPLEMENTATION of the IMP-17 canonicalisation. It was promoted here
// from src/ledger/ledger.cpp unchanged (the decoder below was differentially
// verified against nlohmann's own Höhrmann DFA and is exhaustively idempotent);
// the ledger and the intent log now both call this one definition rather than
// carrying a copy each. See the contract block in domain/utf8.hpp.

namespace broker_exec::domain {

namespace {

// The number of bytes in a WELL-FORMED UTF-8 sequence introduced by `lead`, or 0
// when `lead` can never begin one: a bare continuation byte (0x80-0xBF), the two
// overlong two-byte leads (0xC0/0xC1), or 0xF5-0xFF (which could only encode a
// code point above U+10FFFF).
// (Bytes travel as `unsigned` rather than `unsigned char` throughout this file:
// integral promotion would otherwise make every comparison against an unsigned
// literal a signed/unsigned mismatch under MSVC /W4 and -Wextra.)
[[nodiscard]] std::size_t utf8_sequence_length(unsigned lead) noexcept {
  if (lead < 0x80U)
    return 1;  // ASCII
  if (lead < 0xC2U)
    return 0;  // 0x80-0xBF continuation, 0xC0/0xC1 overlong
  if (lead < 0xE0U)
    return 2;
  if (lead < 0xF0U)
    return 3;
  if (lead < 0xF5U)
    return 4;
  return 0;  // 0xF5-0xFF: beyond U+10FFFF
}

// The inclusive range the SECOND byte of a sequence led by `lead` may take. It is
// tighter than 0x80-0xBF for exactly four leads, and those four tightenings are
// what reject the OVERLONG three- and four-byte forms (0xE0, 0xF0), the UTF-16
// SURROGATES U+D800-U+DFFF (0xED) and everything above U+10FFFF (0xF4) — i.e.
// exactly the set RFC 3629 forbids and exactly the set nlohmann's decoder
// rejects. Third/fourth bytes are always 0x80-0xBF.
void utf8_second_byte_range(unsigned lead, unsigned& lo, unsigned& hi) noexcept {
  lo = 0x80U;
  hi = 0xBFU;
  if (lead == 0xE0U) {
    lo = 0xA0U;  // no overlong 3-byte form
  } else if (lead == 0xEDU) {
    hi = 0x9FU;  // no U+D800-U+DFFF surrogate
  } else if (lead == 0xF0U) {
    lo = 0x90U;  // no overlong 4-byte form
  } else if (lead == 0xF4U) {
    hi = 0x8FU;  // nothing above U+10FFFF
  }
}

}  // namespace

std::string canonical_text(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned lead = static_cast<unsigned char>(text[i]);
    const std::size_t len = utf8_sequence_length(lead);
    if (len == 0) {
      // The maximal subpart is this single byte: it cannot lead anything.
      out.append(kUtf8Replacement.data(), kUtf8Replacement.size());
      ++i;
      continue;
    }
    if (len == 1) {
      out.push_back(text[i]);  // ASCII, verbatim
      ++i;
      continue;
    }
    unsigned lo = 0;
    unsigned hi = 0;
    utf8_second_byte_range(lead, lo, hi);
    // Consume as much of a well-formed sequence as is actually present.
    std::size_t got = 1;
    while (got < len && i + got < text.size()) {
      const unsigned cont = static_cast<unsigned char>(text[i + got]);
      const bool in_range =
          (got == 1) ? (cont >= lo && cont <= hi) : (cont >= 0x80U && cont <= 0xBFU);
      if (!in_range) {
        break;
      }
      ++got;
    }
    if (got == len) {
      out.append(text.data() + i, len);  // a complete, valid sequence: verbatim
      i += len;
      continue;
    }
    // Truncated, or broken by a byte that cannot continue it: ONE U+FFFD for the
    // maximal subpart, and `i` advances only over what we consumed, so the byte
    // that broke the run is re-read as a fresh lead on the next iteration.
    out.append(kUtf8Replacement.data(), kUtf8Replacement.size());
    i += got;
  }
  return out;
}

}  // namespace broker_exec::domain
