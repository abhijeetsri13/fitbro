#pragma once

#include <cstdint>
#include <string>

namespace broker_exec::domain {

// Exact monetary amount stored as integer paise (1 rupee = 100 paise).
//
// NO floating point appears anywhere in the money/price path (binding
// convention): doubles cannot represent decimal currency exactly, and rounding
// drift is unacceptable in an execution core. All arithmetic is int64 paise.
//
// Immutable value type: const-correct, copyable, with value-equality semantics.
class Money {
 public:
  // Default-constructs to zero so Money is a regular, container-friendly value.
  constexpr Money() noexcept = default;

  // Build from a raw paise count.
  [[nodiscard]] static constexpr Money from_paise(std::int64_t paise) noexcept {
    return Money{paise};
  }

  // Build from a (rupees, paise) pair. paise is added with the sign of rupees
  // for negative amounts (e.g. from_rupees(-5, 50) == -550 paise), so callers
  // pass the magnitude of the fractional part.
  [[nodiscard]] static constexpr Money from_rupees(std::int64_t rupees,
                                                   std::int64_t paise = 0) noexcept {
    const std::int64_t signed_paise = rupees < 0 ? -paise : paise;
    return Money{rupees * 100 + signed_paise};
  }

  // Raw paise accessor.
  [[nodiscard]] constexpr std::int64_t paise() const noexcept { return paise_; }

  // Arithmetic over exact paise. Same-currency assumption (single currency at
  // MVP); overflow is the caller's responsibility (amounts are bounded by
  // risk limits well below int64 range).
  [[nodiscard]] constexpr Money operator+(const Money& rhs) const noexcept {
    return Money{paise_ + rhs.paise_};
  }
  [[nodiscard]] constexpr Money operator-(const Money& rhs) const noexcept {
    return Money{paise_ - rhs.paise_};
  }
  [[nodiscard]] constexpr Money operator-() const noexcept { return Money{-paise_}; }

  // Value comparisons (C++20 synthesizes >, <=, >= and != from these).
  [[nodiscard]] constexpr bool operator==(const Money& rhs) const noexcept = default;
  [[nodiscard]] constexpr auto operator<=>(const Money& rhs) const noexcept = default;

  // Human-readable "<rupees>.<paise>" form for logs/reports (e.g. "-5.50").
  [[nodiscard]] std::string to_string() const;

 private:
  explicit constexpr Money(std::int64_t paise) noexcept : paise_{paise} {}

  std::int64_t paise_{0};
};

// Exact price stored as integer paise, distinct from Money so prices and
// amounts never get implicitly mixed. Same no-float guarantee.
class Price {
 public:
  constexpr Price() noexcept = default;

  [[nodiscard]] static constexpr Price from_paise(std::int64_t paise) noexcept {
    return Price{paise};
  }
  [[nodiscard]] static constexpr Price from_rupees(std::int64_t rupees,
                                                   std::int64_t paise = 0) noexcept {
    const std::int64_t signed_paise = rupees < 0 ? -paise : paise;
    return Price{rupees * 100 + signed_paise};
  }

  [[nodiscard]] constexpr std::int64_t paise() const noexcept { return paise_; }

  // Round this price to the nearest integer multiple of `tick` (round half-up).
  // `tick` must be > 0; a non-positive tick returns *this unchanged (callers
  // validate tick > 0 upstream at the gate). All-integer math, no float.
  //
  // Total over the whole int64 range — there is no precondition on the VALUE.
  // A price whose rounded form would not fit in an int64 (INT64_MIN at most
  // ticks, INT64_MAX where it would round up past the end) is likewise returned
  // unchanged, never wrapped or clamped: an un-rounded price is still rejected
  // by the tick-alignment gate, whereas a wrapped one would be tick-aligned and
  // would pass it.
  [[nodiscard]] Price round_to_tick(Price tick) const noexcept;

  [[nodiscard]] constexpr Price operator+(const Price& rhs) const noexcept {
    return Price{paise_ + rhs.paise_};
  }
  [[nodiscard]] constexpr Price operator-(const Price& rhs) const noexcept {
    return Price{paise_ - rhs.paise_};
  }

  [[nodiscard]] constexpr bool operator==(const Price& rhs) const noexcept = default;
  [[nodiscard]] constexpr auto operator<=>(const Price& rhs) const noexcept = default;

  [[nodiscard]] std::string to_string() const;

 private:
  explicit constexpr Price(std::int64_t paise) noexcept : paise_{paise} {}

  std::int64_t paise_{0};
};

// Integer share/contract/lot count. A distinct strong type so quantities are
// never confused with prices or raw ints.
class Quantity {
 public:
  constexpr Quantity() noexcept = default;

  [[nodiscard]] static constexpr Quantity of(std::int64_t value) noexcept {
    return Quantity{value};
  }

  [[nodiscard]] constexpr std::int64_t value() const noexcept { return value_; }

  [[nodiscard]] constexpr Quantity operator+(const Quantity& rhs) const noexcept {
    return Quantity{value_ + rhs.value_};
  }
  [[nodiscard]] constexpr Quantity operator-(const Quantity& rhs) const noexcept {
    return Quantity{value_ - rhs.value_};
  }

  [[nodiscard]] constexpr bool operator==(const Quantity& rhs) const noexcept = default;
  [[nodiscard]] constexpr auto operator<=>(const Quantity& rhs) const noexcept = default;

  [[nodiscard]] std::string to_string() const;

 private:
  explicit constexpr Quantity(std::int64_t value) noexcept : value_{value} {}

  std::int64_t value_{0};
};

}  // namespace broker_exec::domain
