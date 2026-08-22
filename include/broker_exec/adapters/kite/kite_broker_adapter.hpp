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
// THE FLATTEN (IMP-13): `square_off` is a REAL position flatten, not a cancel —
// see the contract on the method below. It is the emergency exit, so it is built
// to survive being called twice.
//
// MONEY: integer paise only; Kite's rupee-decimal prices are parsed with no-float
// decimal-string arithmetic. SECRETS: credentials live only inside the REST
// client's auth header; no secret can reach an Error here (errors come pre-scrubbed
// from `map_http_error`).
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "broker_exec/adapters/kite/kite_rest_client.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::adapters::kite {

// ── The exchange-resolution seam (IMP-13, AC-4) ─────────────────────────────
//
// `domain::OrderIntent` carries no exchange, so something has to supply one. The
// instrument master (Story 2.6) already KNOWS it — `refdata::InstrumentMaster`
// indexes on (exchange, tradingsymbol) and returns a `domain::Instrument` whose
// `exchange` field is the authority. This alias is the seam that lets the
// composition root hand that knowledge to the adapter WITHOUT the adapter linking
// refdata (which would drag ClockPort + the cache lifecycle into a transport
// layer for one string).
//
// CONTRACT, and it is deliberately strict:
//   * Wired  -> AUTHORITATIVE. Its answer is used for order build AND for the
//               square-off exit, and its ERROR IS FAIL-CLOSED: the order is NOT
//               sent. Falling back to the heuristic after the authority said "I
//               do not know this symbol" would route a real order on a guess made
//               by the layer we just overrode — the exact failure the seam exists
//               to end.
//   * Absent -> the symbol-shape heuristic below is used, and its known
//               mis-routing risk stands (see KNOWN LIMITATIONS).
// Wire it ONCE at composition, before any order is placed.
using ExchangeResolver = std::function<Result<std::string>(const std::string& tradingsymbol)>;

// KNOWN LIMITATIONS (must resolve before live scale — tier-2)
// ───────────────────────────────────────────────────────────
// This adapter is certified at tier-1 (recorded-conformance, Story 2.14): it
// passes the broker-agnostic kit against an in-memory recorded Kite endpoint with
// NO network and NO live credentials. The following gaps are real and intentional
// at this tier; production / the live min-qty smoke (TO-6) MUST close them:
//
//   * CORRELATION STATE IS IN-MEMORY ONLY (`tag_to_ref_` / `id_to_ref_`). After a
//     process restart neither map can name a previously placed order, so such a
//     row comes back with an EMPTY client_ref until it is placed again. This is
//     safe, not silent — the intent is durable in the IntentLog and the
//     reconcile-first posture holds the order under an operator alert — but it is
//     also why square_off's duplicate guard does NOT rely on these maps (below).
//
//   * THE EXCHANGE SEAM IS UNWIRED ON THE ONLY PRODUCTION ASSEMBLY, so the
//     heuristic below is UNCONDITIONALLY IN FORCE on the live path. This is the
//     open half of the gap, and it cannot be closed from inside this adapter:
//     `composition::make_broker` builds its Kite broker with the ONE-ARGUMENT
//     constructor, `set_exchange_resolver` and the two-argument constructor have no
//     production caller, and `has_exchange_resolver()` — named below as the way a
//     composition root asserts the guess is not in force — has no production caller
//     either, because the owning type is file-local and `BrokerAssembly::broker()`
//     hands back a `ports::BrokerPort&`, which has no such method. TO CLOSE IT:
//     build an ExchangeResolver from `refdata::InstrumentMaster::resolve` in the
//     broker factory, pass it to the two-argument constructor, and make safe-start
//     refuse to run LIVE while `has_exchange_resolver()` is false. Note the
//     exchange is already known one layer up — the entry context carries a resolved
//     `domain::Instrument` whose `exchange` the validation gate checks — and is
//     then dropped, because `OrderIntent` has no exchange field; the gate cannot
//     catch a mis-route because it validates the TRUE exchange, never the guess.
//
//   * The heuristic itself (the fallback) routes to NFO only when the symbol ENDS
//     in "FUT"/"CE"/"PE" AND carries digits, else NSE. It was a SUBSTRING match
//     until IMP-26, which mis-routed every cash equity whose name merely contains
//     those letters — CESC, CEATLTD, PETRONET, PERSISTENT, PEL, CENTRALBK — into a
//     permanent Validation/DoNotRetry rejection at the broker. The narrowed form
//     keeps every real derivative on NFO and takes the equities off it, but it is
//     STILL A GUESS: NOTHING marks the wire when it is used — the params a guess
//     produces are byte-identical to the params a resolved exchange produces, so a
//     live account cannot be audited after the fact for which one was in force.
//     Wire the resolver.
//
// RESOLVED (IMP-13): square_off is a REAL FLATTEN — it no longer cancels and
// reports success against a filled position (which told the caller "you are flat"
// while the position was still on, whereupon the caller stopped managing it). It
// now reads broker truth, cancels any working remainder, and places ONE
// deterministically-named opposite order for exactly the CANONICAL filled quantity.
// See the square_off contract below for the duplicate-safety argument.
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

  // Construct with the exchange-resolution seam already wired (AC-4). Preferred in
  // production: it makes "resolver-less" a visible choice at the composition root
  // rather than a default nobody notices.
  KiteBrokerAdapter(KiteRestClient& rest, ExchangeResolver exchange_resolver);

  // Wire (or replace) the exchange resolver. Intended to be called ONCE, at
  // composition, before any order is placed — swapping it mid-session would change
  // where a live position's exit routes.
  void set_exchange_resolver(ExchangeResolver exchange_resolver);

  // True iff an exchange resolver is wired (i.e. the heuristic is NOT in force).
  [[nodiscard]] bool has_exchange_resolver() const noexcept;

  // ── Mutations ──
  [[nodiscard]] Result<ports::BrokerAck> place(const domain::OrderIntent& intent) override;
  [[nodiscard]] Result<ports::BrokerAck> modify(const std::string& broker_order_id,
                                                const domain::OrderIntent& intent) override;
  [[nodiscard]] Result<ports::Ok> cancel(const std::string& broker_order_id) override;

  // FLATTEN the position opened by `broker_order_id` (IMP-13, AC-1/AC-2).
  //
  // THE PROTOCOL, in order, and every step is load-bearing:
  //   (a) FETCH BROKER TRUTH FIRST. The parent order is re-read from /orders — not
  //       from any local cache and not from a push — and its filled quantity is
  //       normalized through `fillnorm` (quantity-first: a broker that says
  //       "COMPLETE" while reporting a short fill is a PARTIAL, and only an
  //       AUTHORITATIVE read may size an exit).
  //   (b) CANCEL THE WORKING REMAINDER, if the order is not already terminal. An
  //       `OrderNotFound` outcome is TOLERATED — it means the remainder went
  //       terminal underneath us, which is the state we were trying to reach. Any
  //       other cancel failure aborts BEFORE the exit: leaving a live remainder
  //       while adding an opposite order is how a flatten turns into a reversal.
  //   (c) PLACE ONE opposite-side order for EXACTLY the canonical filled quantity,
  //       named `<parent>#X` (adapters/square_off_exit.hpp) and tagged with a
  //       PARENT-BROKER-ID-DERIVED tag, so a replay finds it (see AC-2 below).
  //   (d) ZERO FILLED is a COMPLETE square-off: the cancel alone flattened it, and
  //       this returns ok WITHOUT placing anything. Placing a "0-quantity exit" or
  //       an exit for the ORDERED size would open a fresh naked position.
  //   (e) ANY UNKNOWN LEG -> a typed `ReconcileFirst` error and NO exit order. An
  //       unrecognized status, an order that is not in broker truth at all, or a
  //       read failure all land here. We do not flatten what we cannot read.
  //
  // ORDERING RULE: every refusal that can be decided from the READ is decided
  // BEFORE the cancel — the exit's side, the exchange (see exit_exchange), and the
  // "is this row itself an exit?" check all run first. A refusal that fired after
  // the cancel would leave the working remainder pulled and the already-filled
  // quantity NAKED, i.e. a failed square_off that made the position harder to
  // manage than not calling it at all.
  //
  // AC-2, THE DUPLICATE-SAFETY ARGUMENT: the exit's Kite `tag` is derived from the
  // PARENT BROKER ORDER ID, which is broker truth and survives a process restart —
  // unlike this adapter's correlation maps, and unlike the parent's client_ref.
  // Step (a)'s fetch therefore also scans for an order already carrying that tag,
  // and if one is there the flatten reports ok WITHOUT re-placing. That closes both
  // dangerous replays: a double invocation, and a crash between (b) and (c).
  //
  // FINDING AN EXIT IS NOT ENOUGH, AND BOTH HALVES BITE:
  //   * ITS STATE. A REJECTED or CANCELLED exit is not an exit in force — the
  //     position is fully on and the broker is not going to close it. Both are a
  //     `RaiseAlert` error; a status we cannot read is `ReconcileFirst`. Only a
  //     working/filled exit answers ok.
  //   * ITS SIZE. A parent that kept filling after its exit was sized leaves an
  //     exit that is TOO SMALL. That is answered with a `RaiseAlert` error
  //     (`KITE-SQUAREOFF-EXITSHORT`) and NOT with a top-up order — see the
  //     decision recorded in the implementation and in square_off_exit.hpp.
  //
  // NEVER FLATTEN A FLATTEN: if the row named by `broker_order_id` carries an exit
  // tag, this refuses (`DoNotRetry`). A panic loop that squares off every row in
  // the book would otherwise reach the exit it just placed and re-open the
  // position in the original direction.
  //
  // GATE POSTURE: an exit reaching the broker through this adapter is by
  // construction exempt from the strategy ENTRY gates — it is not an entry. The
  // intended final chokepoint remains `runtime::dispatch()` (record -> fsync ->
  // send -> record); routing the flatten through it is tracked as tier-2 for when
  // the runtime loop lands. Until then a caller that needs the intent journalled
  // uses `Dispatcher::square_off`, which wraps this call.
  [[nodiscard]] Result<ports::Ok> square_off(const std::string& broker_order_id) override;

  // square_off with an optional CIRCUIT/LPP PRICE BAND (IMP-13, AC-1 tail).
  //
  // With no band the exit is a MARKET order — the only order type that cannot be
  // priced out of a fill. When BOTH bounds are supplied the exit becomes a LIMIT
  // CLAMPED TO THE BAND EDGE IT MUST CROSS (a BUY exit at the UPPER bound, a SELL
  // exit at the LOWER bound): still marketable, but priced where the exchange will
  // accept it instead of earning a "price out of LPP range" rejection on exactly
  // the fast move that made the flatten necessary.
  //
  // WHY A PAIR OF Prices AND NOT `priceband::PriceBand`: this is the smaller API.
  // The band type would put `broker_exec_priceband` on this adapter's PUBLIC link
  // line and in its public header for two integers. The clamp rule here is the
  // same one `priceband::check_price_band(..., is_exit=true)` encodes — an exit is
  // CLAMPED, never BLOCKED — and a caller that already holds a `PriceBand` passes
  // `{band.lower, band.upper}`. A HALF-supplied band (one bound only) or an
  // INVERTED band (lower > upper) is treated as NO band: a malformed band cannot
  // price anything, and a MARKET exit is the safe reading of "I do not know".
  [[nodiscard]] Result<ports::Ok> square_off_banded(const std::string& broker_order_id,
                                                    std::optional<domain::Price> band_lower,
                                                    std::optional<domain::Price> band_upper);

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

  // The exchange for a symbol: the wired resolver (AUTHORITATIVE, and its error
  // propagates — no fallback) or, with no resolver, the symbol-shape heuristic.
  [[nodiscard]] Result<std::string> resolve_exchange(const std::string& symbol) const;

  // The exchange a square-off EXIT must route to, given the parent order row's own
  // reported exchange. Differs from resolve_exchange in two ways, both because a
  // flatten must land where the position ALREADY IS:
  //   * with NO resolver wired, the parent row's exchange (broker truth) is used
  //     in preference to the symbol-shape guess;
  //   * with a resolver wired, a DISAGREEMENT between it and the parent row is a
  //     typed refusal, not a preference — routing an "exit" to the other venue
  //     opens a second position instead of closing the first.
  // Called BEFORE any side effect, so a refusal cannot strand a cancelled
  // remainder next to a naked fill.
  [[nodiscard]] Result<std::string> exit_exchange(const std::string& symbol,
                                                  const std::string& parent_exchange) const;

  KiteRestClient& rest_;

  // Empty until wired. See ExchangeResolver for the fail-closed contract.
  ExchangeResolver exchange_resolver_;

  // Correlation maps populated on the place path (see the class comment). Both are
  // monotonically grown; a scenario/session keeps one adapter instance so the maps
  // survive across the place -> reconcile sequence.
  std::unordered_map<std::string, std::string> tag_to_ref_;  // tag           -> client_ref
  std::unordered_map<std::string, std::string> id_to_ref_;   // broker_order_id -> client_ref
};

}  // namespace broker_exec::adapters::kite
