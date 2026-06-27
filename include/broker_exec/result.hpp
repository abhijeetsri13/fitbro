#pragma once

// Project-wide fallible-call alias. Every internal call that can fail returns a
// `Result<T>` — an `expected<T, Error>`. Errors are values, never thrown across
// the strategy-facing boundary (Story 1.4, FR-25; see conventions "Errors").
//
// Usage:
//   broker_exec::Result<Order> place(const OrderIntent&);
//   if (auto r = place(intent); r) { use(r.value()); }
//   else { handle(r.error()); }            // r.error() is a typed errors::Error
//
// Construct the error state with broker_exec::unexpected(err):
//   return broker_exec::unexpected(broker_exec::errors::make_error(...));
//
// The alias deliberately lives in `namespace broker_exec` (NOT errors), so the
// shared contract name is `broker_exec::Result<T>`. `expected<>` is our own
// minimal C++20 type (see expected.hpp for the why); member names match
// std::expected so a future C++23 switch is source-compatible.

#include <utility>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/expected.hpp"

namespace broker_exec {

template <class T>
using Result = expected<T, ::broker_exec::errors::Error>;

// Convenience: wrap a typed Error as the error state of any Result<T>.
[[nodiscard]] inline unexpected<errors::Error> fail(errors::Error error) {
  return unexpected<errors::Error>(std::move(error));
}

}  // namespace broker_exec
