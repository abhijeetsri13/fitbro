#pragma once

// A tiny, self-contained `expected<T, E>` for C++20.
//
// WHY THIS EXISTS (decision record):
//   The project targets C++20. `std::expected` is a C++23 library feature and
//   is NOT guaranteed present on every C++20 toolchain we build on (MSVC/gcc/
//   clang in the CI matrix). Rather than gate behind a fragile feature-test
//   macro, we ship this minimal vocabulary type so every supported compiler
//   builds identically. The surface is intentionally a small subset of
//   `std::expected`: `has_value()`, `operator bool`, `value()`, `error()`,
//   construction from a value or from `unexpected(E)`. If/when the project
//   moves to C++23 this can be aliased to `std::expected` with no caller change
//   (the member names match).
//
// Cross-platform: C++20 standard library only (`<utility>`, `<type_traits>`,
// `<new>`). No OS APIs, no `#ifdef`. Header-only.

#include <new>
#include <type_traits>
#include <utility>

namespace broker_exec {

// Wraps an error value so an `expected<T, E>` can be unambiguously constructed
// in its error state, even when T and E are otherwise convertible.
template <class E>
class unexpected {
 public:
  explicit constexpr unexpected(E error) : error_(std::move(error)) {}

  [[nodiscard]] constexpr const E& error() const& noexcept { return error_; }
  [[nodiscard]] constexpr E& error() & noexcept { return error_; }
  [[nodiscard]] constexpr E&& error() && noexcept { return std::move(error_); }

 private:
  E error_;
};

template <class E>
unexpected(E) -> unexpected<E>;

// Minimal expected<T, E>. Holds either a value of type T or an error of type E,
// never both. Non-throwing accessors are caller-checked (check has_value() /
// operator bool first); this matches our no-throw-across-the-strategy-boundary
// policy — fallible calls return an expected, they do not throw.
template <class T, class E>
class expected {
 public:
  using value_type = T;
  using error_type = E;

  // Value-state constructors.
  constexpr expected(const T& value) : has_value_(true) {  // NOLINT(*-explicit-*)
    ::new (static_cast<void*>(std::addressof(storage_.value))) T(value);
  }
  constexpr expected(T&& value) : has_value_(true) {  // NOLINT(*-explicit-*)
    ::new (static_cast<void*>(std::addressof(storage_.value))) T(std::move(value));
  }

  // Error-state constructors (from unexpected<E>).
  constexpr expected(const unexpected<E>& unex) : has_value_(false) {  // NOLINT(*-explicit-*)
    ::new (static_cast<void*>(std::addressof(storage_.error))) E(unex.error());
  }
  constexpr expected(unexpected<E>&& unex) : has_value_(false) {  // NOLINT(*-explicit-*)
    ::new (static_cast<void*>(std::addressof(storage_.error))) E(std::move(unex).error());
  }

  expected(const expected& other) : has_value_(other.has_value_) {
    if (has_value_) {
      ::new (static_cast<void*>(std::addressof(storage_.value))) T(other.storage_.value);
    } else {
      ::new (static_cast<void*>(std::addressof(storage_.error))) E(other.storage_.error);
    }
  }

  expected(expected&& other) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                      std::is_nothrow_move_constructible_v<E>)
      : has_value_(other.has_value_) {
    if (has_value_) {
      ::new (static_cast<void*>(std::addressof(storage_.value)))
          T(std::move(other.storage_.value));
    } else {
      ::new (static_cast<void*>(std::addressof(storage_.error)))
          E(std::move(other.storage_.error));
    }
  }

  expected& operator=(const expected& other) {
    if (this != &other) {
      destroy();
      has_value_ = other.has_value_;
      if (has_value_) {
        ::new (static_cast<void*>(std::addressof(storage_.value))) T(other.storage_.value);
      } else {
        ::new (static_cast<void*>(std::addressof(storage_.error))) E(other.storage_.error);
      }
    }
    return *this;
  }

  expected& operator=(expected&& other) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                                 std::is_nothrow_move_constructible_v<E>) {
    if (this != &other) {
      destroy();
      has_value_ = other.has_value_;
      if (has_value_) {
        ::new (static_cast<void*>(std::addressof(storage_.value)))
            T(std::move(other.storage_.value));
      } else {
        ::new (static_cast<void*>(std::addressof(storage_.error)))
            E(std::move(other.storage_.error));
      }
    }
    return *this;
  }

  ~expected() { destroy(); }

  [[nodiscard]] constexpr bool has_value() const noexcept { return has_value_; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value_; }

  // Value accessors. Precondition: has_value() == true (caller-checked).
  [[nodiscard]] constexpr const T& value() const& noexcept { return storage_.value; }
  [[nodiscard]] constexpr T& value() & noexcept { return storage_.value; }
  [[nodiscard]] constexpr T&& value() && noexcept { return std::move(storage_.value); }

  [[nodiscard]] constexpr const T& operator*() const& noexcept { return storage_.value; }
  [[nodiscard]] constexpr T& operator*() & noexcept { return storage_.value; }

  // Error accessors. Precondition: has_value() == false (caller-checked).
  [[nodiscard]] constexpr const E& error() const& noexcept { return storage_.error; }
  [[nodiscard]] constexpr E& error() & noexcept { return storage_.error; }
  [[nodiscard]] constexpr E&& error() && noexcept { return std::move(storage_.error); }

 private:
  void destroy() noexcept {
    if (has_value_) {
      storage_.value.~T();
    } else {
      storage_.error.~E();
    }
  }

  // Manually-managed union: exactly one member is alive, tracked by has_value_.
  union Storage {
    Storage() {}   // NOLINT(*-member-init) — members are placement-new'd
    ~Storage() {}  // destruction is driven by expected::destroy()
    T value;
    E error;
  } storage_;
  bool has_value_;
};

}  // namespace broker_exec
