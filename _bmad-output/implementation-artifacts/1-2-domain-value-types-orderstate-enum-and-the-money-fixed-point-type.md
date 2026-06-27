# Story 1.2: Domain value types, OrderState enum, and the Money fixed-point type

Status: review
Epic: 1 — Order-Safety Substrate

## Story

As a strategy author,
I want immutable domain types and an exact Money type,
So that prices and quantities are never represented as floating point.

## Acceptance Criteria

1. Given the domain library, when I construct `Money`, `Price`, `Quantity`, `OrderIntent`, `Order`, `Trade`, `Position`, `Instrument`, then `Money`/`Price` store integer paise (`int64`) and expose `round_to_tick()`; no `double`/`float` appears in any money path (lint-enforced).
2. `OrderState` enumerates CREATED…UNKNOWN, RECONCILED, PARTIALLY_PLACED, MANUAL_INTERVENTION_REQUIRED.
3. Value types are immutable and unit-tested for equality/serialization.

## Tasks

- [x] `include/broker_exec/domain/enums.hpp` — `Side`, `OrderType`, `Product`, `OrderState` (13 states incl. Reconciled, PartiallyPlaced, ManualInterventionRequired) + free `to_string()` overloads for serialization/logging.
- [x] `include/broker_exec/domain/money.hpp` — `Money` (int64 paise: `from_paise`/`from_rupees`/`paise()`/`+`/`-`/unary-`-`/comparisons/`to_string()`), `Price` (int64 paise + `round_to_tick(Price) const`), `Quantity` (int64: `of`/`value()`/`+`/`-`/comparisons). All `constexpr`, immutable, value semantics. No `double`/`float`.
- [x] `include/broker_exec/domain/types.hpp` — `Instrument`, `OrderIntent`, `Order`, `Trade`, `Position` aggregates with defaulted `operator==` and `to_string()`.
- [x] `include/broker_exec/domain/domain.hpp` — umbrella include.
- [x] `src/domain/enums.cpp` — stable `to_string()` name tables (`switch` with no `default`, so a new enumerator is a `-Wswitch` compile error).
- [x] `src/domain/money.cpp` — all-integer `format_paise` (sign-correct, two-digit paise, INT64_MIN-safe via magnitude), half-up `round_to_tick` (magnitude-symmetric for negatives, no-op on tick ≤ 0).
- [x] `src/domain/types.cpp` — aggregate `to_string()` serialization.
- [x] `src/domain/value_types_test.cpp` — Catch2: paise arithmetic, equality/comparisons, `round_to_tick` (on-tick, below/at/above half, exact-half-up, negative, invalid-tick no-op), enum serialization names, aggregate value-equality, `to_string` serialize surface.
- [x] `src/domain/CMakeLists.txt` — added the three new sources; added `broker_exec_domain_tests` under `if(BROKER_EXEC_BUILD_TESTS)` with `add_test`, mirroring `src/platform/CMakeLists.txt`. Kept `version.cpp`, the STATIC target, and PRIVATE `broker_exec_warnings` + `broker_exec_sanitizers`.

## Dev Notes

- **No floating point anywhere.** `Money`/`Price` are int64 paise; `Quantity` is int64 count. `to_string()` and `round_to_tick()` are pure integer math — no `printf("%f")`, no `std::stod`, no `double`.
- **Immutability / value semantics.** Each value type has a private raw-int constructor and named factories (`from_paise`/`from_rupees`/`of`); fields are non-`const` (so the type stays assignable/container-friendly per Google style) but there are no mutating members — instances are effectively immutable, copyable, and compared by value via defaulted `==`/`<=>`.
- **`round_to_tick` semantics.** Round half-up to the nearest integer multiple of the tick. `tick.paise() ≤ 0` is treated as a no-op (returns `*this`); the validation gate enforces `tick > 0` upstream. Negative prices round magnitude-symmetrically so the half boundary is consistent with positives.
- **Enum serialization is a contract (NFR-8).** `to_string(OrderState/Side/OrderType/Product)` returns stable UPPER_SNAKE names; renames are breaking. `switch` statements omit `default` so adding an enumerator without a name is a compile error under `-Werror`/`/WX`.
- **`OrderState` exact set:** Created, Validated, PendingSend, Sent, Acknowledged, PartiallyFilled, Filled, Rejected, Cancelled, Unknown, Reconciled, PartiallyPlaced, ManualInterventionRequired.
- **Strict-warning hygiene.** Signed/unsigned handled explicitly (formatting works in a `std::uint64_t` magnitude with an INT64_MIN guard); no implicit narrowing; `[[nodiscard]]` on factories/accessors/operators.
- **Cross-platform.** C++20 stdlib only (`<cstdint>`, `<string>`, `<string_view>`); no OS APIs, no `#ifdef`, no `<cstdio>`.

## Completion Record

Files created:

- `include/broker_exec/domain/enums.hpp`
- `include/broker_exec/domain/money.hpp`
- `include/broker_exec/domain/types.hpp`
- `include/broker_exec/domain/domain.hpp`
- `src/domain/enums.cpp`
- `src/domain/money.cpp`
- `src/domain/types.cpp`
- `src/domain/value_types_test.cpp`

Files modified:

- `src/domain/CMakeLists.txt` (added sources + `broker_exec_domain_tests`; `version.cpp` and existing target/link config preserved).

Files preserved (untouched): `include/broker_exec/domain/version.hpp`, `src/domain/version.cpp`.

Build: per story constraints, no cmake/conan/build run locally (the orchestrator builds centrally). Code is written to compile clean under MSVC `/W4 /permissive- /WX` and gcc/clang `-Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion`.
