#pragma once

#include <cstdint>
#include <string>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"

namespace broker_exec::domain {

// Tradable instrument reference data, resolved from the broker's instrument
// master (Story 2.6). Symbol<->token plus the derived lot/tick/freeze/expiry
// the validation gate needs. Value type with full value-equality.
struct Instrument {
  std::string symbol;       // Trading symbol, e.g. "NIFTY24JUN24000CE".
  std::int64_t token{0};    // Broker instrument token.
  std::string exchange;     // e.g. "NFO", "NSE".
  Quantity lot_size;        // Minimum tradable / lot-aligned unit.
  Price tick_size;          // Minimum price increment.
  Quantity freeze_qty;      // Per-order exchange freeze ceiling (over => slice).
  std::string expiry;       // ISO date "YYYY-MM-DD"; empty for cash equities.

  [[nodiscard]] bool operator==(const Instrument&) const = default;

  // Stable single-line serialization for logs / round-trip tests.
  [[nodiscard]] std::string to_string() const;
};

// A strategy's request to trade, before any broker contact. Carries the
// client-side reference (Story 1.7) that makes the intent idempotent.
struct OrderIntent {
  std::string client_ref;  // "<strategy>-<sig8>-<uuid>" (or "<parent>#<k>").
  std::string symbol;
  Side side{Side::Buy};
  Quantity quantity;
  Price price;  // Limit/trigger price; ignored for Market orders.
  OrderType order_type{OrderType::Market};
  Product product{Product::Intraday};
  std::string strategy;  // Owning strategy id (multi-strategy isolation).

  [[nodiscard]] bool operator==(const OrderIntent&) const = default;

  [[nodiscard]] std::string to_string() const;
};

// An order tracked through its lifecycle: the originating intent plus the
// broker-assigned id and current fill progress. Owned/mutated only by the
// lifecycle state machine on the main loop (Story 1.8); a plain value here.
struct Order {
  OrderIntent intent;
  OrderState state{OrderState::Created};
  std::string broker_order_id;  // Empty until acknowledged.
  Quantity filled_qty;
  Price avg_price;  // Volume-weighted average fill price.

  [[nodiscard]] bool operator==(const Order&) const = default;

  [[nodiscard]] std::string to_string() const;
};

// A single execution reported by the broker. Linked back to the originating
// order via client_ref and/or broker_order_id.
struct Trade {
  std::string trade_id;
  std::string client_ref;
  std::string broker_order_id;
  Quantity quantity;
  Price price;

  [[nodiscard]] bool operator==(const Trade&) const = default;

  [[nodiscard]] std::string to_string() const;
};

// Net position in a symbol: signed quantity (negative = short) at an average
// price. Reconciled against broker truth (Epic 3).
struct Position {
  std::string symbol;
  Quantity net_qty;  // Signed: positive long, negative short.
  Price avg_price;

  [[nodiscard]] bool operator==(const Position&) const = default;

  [[nodiscard]] std::string to_string() const;
};

}  // namespace broker_exec::domain
