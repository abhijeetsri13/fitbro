#include "broker_exec/domain/money.hpp"

#include <cstdint>
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

std::string Money::to_string() const { return format_paise(paise_); }

std::string Price::to_string() const { return format_paise(paise_); }

Price Price::round_to_tick(Price tick) const noexcept {
  const std::int64_t t = tick.paise_;
  if (t <= 0) {
    return *this;  // Invalid tick; callers validate tick > 0 at the gate.
  }

  const std::int64_t value = paise_;

  // Round half-up to the nearest multiple of t using only integer math.
  // For non-negative values: floor((value + t/2) / t) * t.
  // For negative values, mirror through the magnitude so half rounds toward
  // +inf consistently (e.g. -25 with tick 10 -> -20, matching +25 -> +30).
  if (value >= 0) {
    const std::int64_t half = t / 2;
    const std::int64_t rounded = ((value + half) / t) * t;
    return Price::from_paise(rounded);
  }

  const std::int64_t mag = -value;
  const std::int64_t half = (t - 1) / 2;  // bias so the .5 boundary rounds up
  const std::int64_t rounded = ((mag + half) / t) * t;
  return Price::from_paise(-rounded);
}

std::string Quantity::to_string() const { return std::to_string(value_); }

}  // namespace broker_exec::domain
