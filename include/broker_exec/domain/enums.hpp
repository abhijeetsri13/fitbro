#pragma once

#include <string_view>

namespace broker_exec::domain {

// Direction of an order. Buy increases (or covers a short), Sell decreases.
enum class Side { Buy, Sell };

// Order pricing / triggering style. The four MVP types the validation gate and
// adapters must support; broker-specific variants map onto these.
enum class OrderType { Market, Limit, StopLoss, StopLossMarket };

// Position product / margin bucket. Intraday auto-squares off; Delivery (CNC)
// is overnight; Margin and Normal cover leveraged carry products.
enum class Product { Intraday, Delivery, Margin, Normal };

// Order lifecycle states owned by the lifecycle state machine (Story 1.8).
//
// Forward-progressing happy path:
//   Created -> Validated -> PendingSend -> Sent -> Acknowledged ->
//   PartiallyFilled -> Filled
// Terminal-absorbing states: Filled, Rejected, Cancelled.
//
// Resilience states (Epics 1/3):
//   Unknown                     - send result is uncertain; reconcile, never
//                                 blindly retry.
//   Reconciled                  - state confirmed against broker truth.
//   PartiallyPlaced             - a sliced parent whose children are not all
//                                 placed yet (and may carry an UNKNOWN child).
//   ManualInterventionRequired  - a double fault that needs a human; no
//                                 automatic square-off.
enum class OrderState {
  Created,
  Validated,
  PendingSend,
  Sent,
  Acknowledged,
  PartiallyFilled,
  Filled,
  Rejected,
  Cancelled,
  Unknown,
  Reconciled,
  PartiallyPlaced,
  ManualInterventionRequired
};

// Stable, log/serialization-friendly names. These strings are part of the
// observability contract (NFR-8): renames are breaking changes.
[[nodiscard]] std::string_view to_string(OrderState state) noexcept;
[[nodiscard]] std::string_view to_string(Side side) noexcept;
[[nodiscard]] std::string_view to_string(OrderType type) noexcept;
[[nodiscard]] std::string_view to_string(Product product) noexcept;

}  // namespace broker_exec::domain
