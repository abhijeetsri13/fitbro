#include "broker_exec/domain/money.hpp"

#include <cstdint>
#include <limits>
#include <string>

namespace broker_exec::domain {

namespace {

// Format a signed paise count as "<rupees>.<pp>" with no floating point.
// Always two fractional digits; a single leading '-' for negative amounts.
std::string format_paise(std::int64_t paise) {
  const bool negative = paise < 0;

  // Work in the magnitude to keep the integer division/modulo well-defined and
  // sign-stable across platforms. Guard the INT64_MIN edge so negation is safe.
  const std::uint64_t magnitude =
      negative ? (~static_cast<std::uint64_t>(paise) + 1U) : static_cast<std::uint64_t>(paise);

  const std::uint64_t rupees = magnitude / 100U;
  const std::uint64_t frac = magnitude % 100U;

  std::string out;
  if (negative) {
    out.push_back('-');
  }
  out += std::to_string(rupees);
  out.push_back('.');
  if (frac < 10U) {
    out.push_back('0');
  }
  out += std::to_string(frac);
  return out;
}

}  // namespace

std::string Money::to_string() const {
  return format_paise(paise_);
}

std::string Price::to_string() const {
  return format_paise(paise_);
}

Price Price::round_to_tick(Price tick) const noexcept {
  const std::int64_t t = tick.paise_;
  if (t <= 0) {
    return *this;  // Invalid tick; callers validate tick > 0 at the gate.
  }

  const std::int64_t value = paise_;
  const bool negative = value < 0;

  // Round half-up to the nearest multiple of t using only integer math, done in
  // UNSIGNED magnitude space — the same treatment format_paise gives the same
  // edge, 30 lines above. Every signed form of this arithmetic is UB at the
  // extremes: `-value` is undefined at INT64_MIN (the magnitude is not
  // representable), and both `value + t/2` and `magnitude + bias` overflow
  // within t/2 of the int64 ends. Price::from_paise is a public constructor with
  // no range restriction and unclamped int64s really do reach it (broker JSON,
  // store columns), so neither extreme is unreachable by construction.
  const std::uint64_t ut = static_cast<std::uint64_t>(t);
  const std::uint64_t mag =
      negative ? (~static_cast<std::uint64_t>(value) + 1U) : static_cast<std::uint64_t>(value);

  // Bias so an exact .5 boundary always rounds toward +inf: away from zero on
  // the positive side, toward zero on the negative side (+25 -> +30 and
  // -25 -> -20 at tick 10). mag <= 2^63 and half < 2^62, so the add below
  // cannot wrap uint64 and the multiply cannot exceed the pre-division value.
  const std::uint64_t half = negative ? (ut - 1U) / 2U : ut / 2U;
  const std::uint64_t rounded = ((mag + half) / ut) * ut;

  // Fail CLOSED when the rounded magnitude leaves int64 range: hand back the
  // price UNCHANGED rather than a wrapped or clamped one. An un-rounded price is
  // still caught by the tick-alignment gate downstream; a wrapped one would be
  // perfectly tick-aligned and would sail straight through it.
  constexpr std::uint64_t kMaxMagnitude =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  // The negative side reaches exactly one further: |INT64_MIN| is kMaxMagnitude + 1.
  const std::uint64_t limit = negative ? kMaxMagnitude + 1U : kMaxMagnitude;
  if (rounded > limit) {
    return *this;
  }

  // Negate in unsigned space as well, so a magnitude of exactly 2^63 round-trips
  // to INT64_MIN instead of overflowing on the way back.
  return Price::from_paise(negative ? static_cast<std::int64_t>(~rounded + 1U)
                                    : static_cast<std::int64_t>(rounded));
}

std::string Quantity::to_string() const {
  return std::to_string(value_);
}

}  // namespace broker_exec::domain
