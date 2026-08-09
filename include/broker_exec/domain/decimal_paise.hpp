#pragma once

// broker_exec::domain — FAIL-CLOSED decimal <-> integer-paise text conversion.
//
// WHY THIS EXISTS: money on a broker wire arrives as TEXT ("1450.25"), and the
// binding project convention is that no `double`/`float` may touch a money path.
// Three modules had grown their own copy of that parse, and the copies did not
// agree on the thing that matters most — WHAT TO DO WITH INPUT THAT IS NOT A
// NUMBER. A truncating parse silently reads "12.3.4.5" as 1230 paise and
// "abc" as 0, which is how a garbled field becomes a confident wrong price.
//
// THE CONTRACT HERE IS THE OPPOSITE: anything we cannot parse EXACTLY is
// `std::nullopt`, and the caller must decide (the Kotak adapter treats it as a
// field it does not understand and forces the row to reconcile). There is no
// "best effort" reading of money.
//
//   * leading/trailing ASCII whitespace is trimmed;
//   * an optional single leading '+'/'-' is accepted;
//   * at least one digit is REQUIRED (""/"-"/"." -> nullopt);
//   * exactly one '.' is permitted; a second one -> nullopt;
//   * ANY non-digit after the sign (other than that single '.') -> nullopt;
//   * up to two fractional digits are significant and are zero-padded
//     ("1450.5" -> 145050); further fractional digits must still BE digits and
//     are truncated (sub-paise precision does not exist);
//   * OVERFLOW is a parse failure, not wraparound: an input whose value cannot
//     be represented in int64 paise yields nullopt rather than undefined
//     behaviour (a 20-digit field is a real thing a broker can send).
//
// THE FOLLOW-UP IS DONE (IMP-14). `src/refdata/instrument_master.cpp` and
// `src/adapters/kite/kite_broker_adapter.cpp` carried private copies until
// Story 6.2 shipped this header for Kotak only; both are now migrated and THIS
// IS THE ONLY DECIMAL->PAISE PARSE IN THE TREE. What the migration changed:
//
//   * refdata was already fail-closed, so it is a pure de-duplication with one
//     safety gain — its copy computed `rupees * 10 + digit` UNCHECKED, so a
//     20-digit `tick_size` was signed-integer overflow (UB). That row is now
//     rejected as unparseable.
//   * the KITE copy was fail-OPEN and wrong on the primary live broker: it
//     `break`ed on the first non-digit and returned the partial value, so
//     "1,450.25" read as Rs 1.00, "N/A" as 0, "1.45e3" as Rs 1.45, and a 20-digit
//     field overflowed. Every money read in that adapter now goes through this
//     parser, and a field that is PRESENT BUT UNREADABLE fails its row (or its
//     whole read) closed — the same contract Kotak has used since Story 6.2.
//     ABSENT is unchanged and still means zero: absent is not an error.
//
// A fourth copy must not appear. If a new adapter needs decimal money, it
// includes this header.
//
// Header-only and dependency-free so any layer may use it without a link edge.
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`, NO FLOAT.

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace broker_exec::domain {

namespace detail {

[[nodiscard]] inline constexpr bool is_ascii_digit(char c) noexcept {
  return c >= '0' && c <= '9';
}

[[nodiscard]] inline constexpr bool is_ascii_space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

[[nodiscard]] inline constexpr std::string_view trim_ascii(std::string_view s) noexcept {
  std::size_t begin = 0;
  while (begin < s.size() && is_ascii_space(s[begin])) {
    ++begin;
  }
  std::size_t end = s.size();
  while (end > begin && is_ascii_space(s[end - 1])) {
    --end;
  }
  return s.substr(begin, end - begin);
}

// Append one digit to a non-negative accumulator, refusing to overflow.
// Returns false (and leaves `value` unspecified-but-valid) when the digit would
// exceed int64 range — the caller turns that into a parse failure.
[[nodiscard]] inline constexpr bool push_digit(std::int64_t& value, char digit) noexcept {
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  const std::int64_t d = static_cast<std::int64_t>(digit - '0');
  if (value > (kMax - d) / 10) {
    return false;
  }
  value = value * 10 + d;
  return true;
}

}  // namespace detail

// Parse a plain integer count ("50", "-3", " 7 ") with the same fail-closed and
// overflow-safe rules. Quantities are counts, never money, but a garbled count is
// exactly as dangerous, so it gets the same treatment.
[[nodiscard]] inline std::optional<std::int64_t> parse_int64(std::string_view in) noexcept {
  const std::string_view s = detail::trim_ascii(in);
  if (s.empty()) {
    return std::nullopt;
  }
  std::size_t i = 0;
  bool negative = false;
  if (s[i] == '+' || s[i] == '-') {
    negative = s[i] == '-';
    ++i;
  }
  std::int64_t value = 0;
  bool any_digit = false;
  for (; i < s.size(); ++i) {
    if (!detail::is_ascii_digit(s[i])) {
      return std::nullopt;  // trailing garbage is a parse FAILURE, not a stop signal
    }
    if (!detail::push_digit(value, s[i])) {
      return std::nullopt;  // overflow -> fail closed, never wrap
    }
    any_digit = true;
  }
  if (!any_digit) {
    return std::nullopt;
  }
  return negative ? -value : value;
}

// Parse a rupee-decimal string into EXACT integer paise, or nullopt. See the
// contract at the top of this header — in particular, malformed input is never
// coerced to a number.
[[nodiscard]] inline std::optional<std::int64_t> parse_decimal_paise(std::string_view in) noexcept {
  const std::string_view s = detail::trim_ascii(in);
  if (s.empty()) {
    return std::nullopt;
  }

  std::size_t i = 0;
  bool negative = false;
  if (s[i] == '+' || s[i] == '-') {
    negative = s[i] == '-';
    ++i;
  }

  std::int64_t rupees = 0;
  bool any_digit = false;
  for (; i < s.size() && s[i] != '.'; ++i) {
    if (!detail::is_ascii_digit(s[i])) {
      return std::nullopt;
    }
    if (!detail::push_digit(rupees, s[i])) {
      return std::nullopt;
    }
    any_digit = true;
  }

  std::int64_t frac = 0;
  int frac_digits = 0;
  if (i < s.size() && s[i] == '.') {
    ++i;
    for (; i < s.size(); ++i) {
      if (!detail::is_ascii_digit(s[i])) {
        return std::nullopt;  // a second '.' or any other byte -> fail closed
      }
      any_digit = true;
      if (frac_digits < 2) {
        frac = frac * 10 + static_cast<std::int64_t>(s[i] - '0');
        ++frac_digits;
      }
      // Beyond two fractional digits the value is truncated: sub-paise precision
      // does not exist. The digits must still BE digits, which the check above
      // enforces, so "1.239" is 123 paise but "1.2x9" is nullopt.
    }
  }

  if (!any_digit) {
    return std::nullopt;  // "", "+", ".", "-." are not numbers
  }
  while (frac_digits < 2) {  // "1450.5" -> 50 paise, not 5
    frac *= 10;
    ++frac_digits;
  }

  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  if (rupees > (kMax - frac) / 100) {
    return std::nullopt;  // the paise scaling itself would overflow
  }
  const std::int64_t paise = rupees * 100 + frac;
  return negative ? -paise : paise;
}

// Render integer paise as a rupee-decimal string ("145005" -> "1450.05"). The
// inverse of parse_decimal_paise for every value it can produce. No float.
[[nodiscard]] inline std::string paise_to_decimal(std::int64_t paise) {
  const bool negative = paise < 0;
  // Take the magnitude in unsigned space so INT64_MIN does not overflow on negation.
  const std::uint64_t magnitude =
      negative ? (~static_cast<std::uint64_t>(paise) + 1U) : static_cast<std::uint64_t>(paise);
  std::string out = std::to_string(magnitude / 100U);
  out.push_back('.');
  const std::uint64_t frac = magnitude % 100U;
  if (frac < 10U) {
    out.push_back('0');
  }
  out += std::to_string(frac);
  return negative ? ("-" + out) : out;
}

}  // namespace broker_exec::domain
