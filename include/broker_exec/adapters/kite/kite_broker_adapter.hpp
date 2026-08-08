#pragma once

// broker_exec::adapters::kite::KiteBrokerAdapter — the Kite Connect implementation
// of `ports::BrokerPort` (Story 2.14, FR-25/FR-37, NFR-1/NFR-3).
//
// WHAT THIS IS: the broker adapter that sits ABOVE the Kite REST transport
// (KiteRestClient over an HttpClient seam) and BELOW the broker-neutral execution
// core. It translates a domain `OrderIntent` into Kite order params, parses Kite's
// JSON `{status,data}` payloads back into domain types, and maps Kite's order
// `status` strings onto the lifecycle `OrderState`. No Kite/HTTP/JSON type ever
// leaks above this line; every method returns a typed `Result<T>` and NOTHING
// throws across the BrokerPort boundary.
//
// THE ZERO-DUPLICATE ANCHOR (the headline invariant): an ack-lost place — the
// caller observes a transport failure but the order IS live at the broker — must
// be reconcilable, never blindly retried. To make that structurally true the
// adapter registers a short correlation `tag -> client_ref` mapping BEFORE it ever
// calls the broker, and echoes that `tag` in the Kite place params. On reconcile,
// `fetch_orders()` recovers the originating `client_ref` for each broker order
// from (broker_order_id -> client_ref) when the ack returned, else from
// (tag -> client_ref) — so the UnknownResolver's match-key ladder
// (order_id > correlation token) finds the single placed order instead of firing
// a duplicate. An unrecoverable order is left with an EMPTY client_ref (fail-safe:
// it simply matches no signal, never the WRONG one).
//
// MONEY: integer paise only; Kite's rupee-decimal prices are parsed with no-float
// decimal-string arithmetic. SECRETS: credentials live only inside the REST
// client's auth header; no secret can reach an Error here (errors come pre-scrubbed
// from `map_http_error`).
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <string>
#include <unordered_map>
#include <vector>

#include "broker_exec/adapters/kite/kite_rest_client.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::adapters::kite {

// KNOWN LIMITATIONS (must resolve before live scale — tier-2)
// ───────────────────────────────────────────────────────────
// This adapter is certified at tier-1 (recorded-conformance, Story 2.14): it
// passes the broker-agnostic kit against an in-memory recorded Kite endpoint with
// NO network and NO live credentials. The following gaps are real and intentional
// at this tier; production / the live min-qty smoke (TO-6) MUST close them:
//
//   * square_off(broker_order_id) currently CANCELS a working order (it issues the
//     same Kite cancel as cancel()). Flattening a FILLED position — a MARKET exit
//     sized and side-flipped from the live net position — is NOT yet implemented.
//     The live smoke's square-off step depends on that real flatten; cancelling a
//     filled order is a no-op against an open position.
//
//   * The exchange is INFERRED from the tradingsymbol by a substring heuristic
//     (a "FUT"/"CE"/"PE" match routes to NFO, else NSE) because OrderIntent carries
//     no exchange. This can MIS-ROUTE a cash equity whose symbol merely contains
//     those letters (e.g. "PETRONET" matches "PE" -> wrongly NFO). Production MUST
//     resolve the exchange from the instrument master (Story 2.6), not the heuristic.
//
// RESOLVED (IMP-11): a StopLoss (SL) order no longer sends the same number as both
// the limit `price` and the `trigger_price`. OrderIntent carries a distinct
// `std::optional<Price> trigger_price`, so SL sends the real pair (`price` +
// `trigger_price`) and SL-M sends the trigger alone. A stop intent that reaches
// this adapter with NO trigger emits no `trigger_price` field at all — Kite
// rejects it definitively, which is strictly safer than arming a live stop at the
// limit price. fetch_orders() parses `order_type` (MARKET/LIMIT/SL/SL-M) and
// `trigger_price` back into the domain; the trigger is published ONLY on a row
// that is actually a stop and carries a positive trigger (Kite reports 0 for
// non-stop orders -> nullopt). An unrecognized `order_type` falls CLOSED to Market
// AND suppresses the trigger — a Market carrying a trigger is a shape the
// validation gate refuses outright, so a row we do not understand is never
// published as an armed stop.

class KiteBrokerAdapter final : public ports::BrokerPort {
 public:
  // Construct over a Kite REST client (which itself wraps the HttpClient transport
  // and the SecretProvider). The client is held by reference and MUST outlive this
  // adapter, matching the codebase's reference-injection convention.
  explicit KiteBrokerAdapter(KiteRestClient& rest);

  // ── Mutations ──
  [[nodiscard]] Result<ports::BrokerAck> place(const domain::OrderIntent& intent) override;
  [[nodiscard]] Result<ports::BrokerAck> modify(const std::string& broker_order_id,
                                                const domain::OrderIntent& intent) override;
  [[nodiscard]] Result<ports::Ok> cancel(const std::string& broker_order_id) override;
  [[nodiscard]] Result<ports::Ok> square_off(const std::string& broker_order_id) override;

  // ── Reads (idempotent) ──
  [[nodiscard]] Result<std::vector<domain::Order>> fetch_orders() override;
  [[nodiscard]] Result<std::vector<domain::Trade>> fetch_trades() override;
  [[nodiscard]] Result<std::vector<domain::Position>> fetch_positions() override;
  [[nodiscard]] Result<ports::FundsSnapshot> fetch_funds() override;

 private:
  // Derive a short, deterministic Kite `tag` (<= 20 chars, the Kite limit) from a
  // client_ref AND register `tag -> client_ref` so an ack-lost order is recoverable
  // from the orderbook. Idempotent for a given client_ref.
  [[nodiscard]] std::string register_tag(const std::string& client_ref);

  // Recover the originating client_ref for a Kite order object: broker_order_id
  // first (set only on a returned ack), then the echoed tag; empty if neither maps
  // (fail-safe — matches no signal rather than the wrong one).
  [[nodiscard]] std::string recover_client_ref(const std::string& broker_order_id,
                                               const std::string& tag) const;

  KiteRestClient& rest_;

  // Correlation maps populated on the place path (see the class comment). Both are
  // monotonically grown; a scenario/session keeps one adapter instance so the maps
  // survive across the place -> reconcile sequence.
  std::unordered_map<std::string, std::string> tag_to_ref_;  // tag           -> client_ref
  std::unordered_map<std::string, std::string> id_to_ref_;   // broker_order_id -> client_ref
};

}  // namespace broker_exec::adapters::kite
