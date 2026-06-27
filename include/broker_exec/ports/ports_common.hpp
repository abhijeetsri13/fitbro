#pragma once

// broker_exec::ports — shared vocabulary for the abstract port interfaces.
//
// The core depends ONLY on these abstractions (ClockPort, BrokerPort, StorePort,
// AlertSink, SecretProvider, RefDataPort) — never on a concrete broker SDK,
// database, or OS API. Concrete impls live in `adapters`/`platform`/later
// stories and are injected at composition time. This file holds the one piece
// of vocabulary every port shares.
//
// ── The Ok / void decision ────────────────────────────────────────────────
// The project's `broker_exec::expected<T, E>` (see expected.hpp) is a minimal
// C++20 vocabulary type that does NOT specialize for `void`: it stores a `T`
// in a union and `value()` returns `T&`, so `expected<void, E>` will not
// compile. Therefore ports that logically return "success or Error" (no value)
// use `Result<Ok>` where `Ok = std::monostate` — a regular, trivially
// constructible stand-in for "no value". Impls write `return ports::ok();` on
// success and `return broker_exec::fail(err);` on failure.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <variant>

#include "broker_exec/result.hpp"

namespace broker_exec::ports {

// Void-substitute: the value carried by a fallible call that returns no data,
// only success-or-Error. See the decision note above.
using Ok = std::monostate;

// Convenience success value for `Result<Ok>`-returning ports. Lets impls write
// `return ports::ok();` instead of constructing a monostate by hand.
[[nodiscard]] inline Result<Ok> ok() noexcept { return Ok{}; }

}  // namespace broker_exec::ports
