#pragma once

// broker_exec::ports::BrokerPort — the abstract broker mutation/query surface.
//
// The single broker-neutral seam between the execution core and a specific
// broker (Kite, Kotak Neo, ...). The core places/modifies/cancels/squares-off
// and fetches state through THIS interface only; concrete adapters live in
// `adapters` and translate to/from the broker's SDK + error vocabulary. No
// broker SDK type ever leaks above this line — inputs/outputs are domain types
// and a typed `Result<T>` (Story 1.4), never raw broker responses.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <string>
#include <vector>

#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::ports {

// Broker acknowledgement of a place/modify: the broker-assigned id plus the
// client_ref we sent, so the caller can correlate the ack to its intent.
struct BrokerAck {
  std::string broker_order_id;  // Broker-assigned order id.
  std::string client_ref;       // Echo of the intent's idempotency key.

  [[nodiscard]] bool operator==(const BrokerAck&) const = default;
};

// A point-in-time view of margin/funds, used by the funds-freshness gate.
struct FundsSnapshot {
  domain::Money available_margin;  // Free margin available to deploy.
  domain::Money used_margin;       // Margin currently blocked.

  [[nodiscard]] bool operator==(const FundsSnapshot&) const = default;
};

// Abstract broker access. Mutations are dangerous (a Timeout may mean the order
// reached the exchange) — callers reconcile rather than blindly retry; that
// policy lives in the runtime, not here. Header-only, pure-virtual.
class BrokerPort {
 public:
  virtual ~BrokerPort() = default;

  // ── Mutations ──
  [[nodiscard]] virtual Result<BrokerAck> place(const domain::OrderIntent& intent) = 0;
  [[nodiscard]] virtual Result<BrokerAck> modify(const std::string& broker_order_id,
                                                 const domain::OrderIntent& intent) = 0;
  [[nodiscard]] virtual Result<Ok> cancel(const std::string& broker_order_id) = 0;
  [[nodiscard]] virtual Result<Ok> square_off(const std::string& broker_order_id) = 0;

  // ── Queries (idempotent reads; safe to retry) ──
  [[nodiscard]] virtual Result<std::vector<domain::Order>> fetch_orders() = 0;
  [[nodiscard]] virtual Result<std::vector<domain::Trade>> fetch_trades() = 0;
  [[nodiscard]] virtual Result<std::vector<domain::Position>> fetch_positions() = 0;
  [[nodiscard]] virtual Result<FundsSnapshot> fetch_funds() = 0;
};

}  // namespace broker_exec::ports
