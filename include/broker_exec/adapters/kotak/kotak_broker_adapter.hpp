#pragma once

// broker_exec::adapters::kotak::KotakBrokerAdapter — the Kotak Neo implementation
// of `ports::BrokerPort` (Story 6.2, FR-1/FR-2/FR-37, NFR-1/NFR-3, CAP-13).
//
// WHAT THIS IS: the broker adapter that sits ABOVE the Kotak Neo REST transport
// (`KotakRestClient` over the `HttpClient` seam) and BELOW the broker-neutral
// execution core. It translates a domain `OrderIntent` into Kotak `jData` order
// fields, parses Kotak's `{"stat":...}` payloads back into domain types, and maps
// Kotak's `ordSt` strings onto the lifecycle `OrderState`. No Kotak/HTTP/JSON type
// ever leaks above this line; every method returns a typed `Result<T>` and NOTHING
// throws across the BrokerPort boundary.
//
// ── THE CORRELATION PROBLEM, AND WHY KOTAK IS NOT KITE ──────────────────────
//
// The Kite adapter anchors an ack-lost order with a short `tag` that Kite echoes
// back on the orderbook — a genuine correlation token (match-key rung 2). **KOTAK
// HAS NO SUCH VERIFIED ECHO.** The Neo quick-order endpoints document no
// round-tripped client tag that we have ever seen come back from a real endpoint,
// and this module refuses to build a safety property on an unverified assumption:
//
//   * we do NOT send a speculative tag field. An unknown `jData` key is a live
//     rejection risk on an endpoint set that is itself still a tier-2 assumption
//     (see kotak_transport.hpp), and a token we cannot trust to return is not an
//     anchor — it is a comforting lie.
//   * we therefore run a TWO-RUNG ladder, in strict NFR-1 precedence order:
//
//       RUNG 1 — BROKER ORDER ID (strong).  A successful place/modify ack carries
//           `nOrdNo`; we bind `nOrdNo -> client_ref` and every later read matches
//           on that id. Unambiguous: the broker minted the id for THIS order.
//
//       RUNG 2 — ATTRIBUTE CORROBORATION (weak, ack-lost only).  When the ack was
//           LOST there is no id to bind, so a broker order is corroborated against
//           the intent we registered BEFORE the wire call, on (symbol, side,
//           quantity). It is admitted ONLY when the pairing is unambiguous in BOTH
//           directions: exactly one un-anchored intent carries that attribute key
//           AND exactly one un-anchored broker order matches it. Any collision —
//           two identical lots of ours, or a MANUAL order placed by the operator
//           with the same shape — makes the pairing ambiguous and we claim NOTHING.
//
//       FAIL CLOSED — neither rung matched: the order comes back with an EMPTY
//           client_ref, AND the adapter withholds that row's correlation tuple
//           (see "WHAT AN UNCORRELATED ROW PUBLISHES" below). That is the intended
//           outcome: an unresolved order held under an operator alert is strictly
//           safer than an order attributed to the WRONG signal.
//
// ANCHOR-ON-CORROBORATION: when rung 2 does resolve unambiguously we immediately
// bind `nOrdNo -> client_ref`, so the weak rung is consulted AT MOST ONCE per
// intent and every subsequent read (orders and trades alike) uses the strong id.
//
// PENDING INTENTS ARE DROPPED ON A DEFINITIVE VERDICT. A registered intent stays
// eligible for rung 2 only while its fate is genuinely ambiguous. If the broker
// returns a DEFINITIVE rejection (a `DoNotRetry`-class error such as the HTTP-200
// `stat:"Not_Ok"` margin reject) then nothing is live, and leaving the intent
// registered would let rung 2 later adopt an operator's unrelated look-alike
// order. The registration is therefore erased unless the error is
// reconcile-first-ish (`ReconcileFirst`, or Timeout/Network/Unknown) — the same
// test `runtime::Dispatcher` uses to decide UNKNOWN vs Rejected, restated here
// because an adapter may not link the runtime. The pending set is additionally
// SIZE-BOUNDED so a long-lived session cannot accumulate stale look-alike bait.
//
// ── WHAT AN UNCORRELATED ROW PUBLISHES (and why) ────────────────────────────
// `fetch_orders()` reports EVERY broker row — an order the engine cannot see is
// far worse than one it cannot name. But for a row it could NOT correlate, the
// adapter deliberately leaves `intent.side`, `intent.quantity` and `intent.price`
// at their defaults, publishing only `broker_order_id`, `intent.symbol`, `state`,
// `filled_qty` and `avg_price`.
//
// THE REASON IS SPECIFIC. `runtime::UnknownResolver`'s rung 3 does its OWN
// attribute corroboration on exactly `(symbol, side, quantity, price)`, and it is
// FIRST-MATCH-WINS with no ambiguity check. For Kotak that rung can never do
// better than the adapter's own — which tests the same attributes but requires
// the pairing to be unambiguous in both directions — and it can do materially
// worse: handed a colliding manual order it would silently adopt whichever row
// came first. Publishing the tuple only for rows we positively correlated makes
// the adapter's refusal actually stick at the STACK level instead of being
// re-decided, more weakly, one layer up. Verified single-consumer: nothing else
// in the codebase reads those three fields off a broker-truth row.
//
// KNOWN RISK, STATED PLAINLY: rung 2 can still adopt a stranger's order that
// happens to share our (symbol, side, qty) while OUR fate is genuinely ambiguous
// and no second look-alike exists to make the pairing ambiguous. It cannot fire a
// duplicate — the read path is read-only — and it errs conservatively: we treat a
// look-alike as possibly-ours and reconcile rather than firing a second order.
//
// HONEST LABELLING CAVEAT: when rung 2 succeeds the adapter stamps the recovered
// client_ref onto the row, so `UnknownResolver` matches it at ITS rung 2 and logs
// `MatchKind::CorrelationToken`. That label overstates the evidence — no broker
// echoed anything; this was attribute corroboration. The observability contract
// has nowhere on `domain::Order` to signal "weak match", and inventing one would
// ripple through ports/store/lifecycle for a logging nuance, so it is documented
// here and in docs/kotak-min-qty-smoke.md rather than encoded. Read a
// CORRELATION_TOKEN match on Kotak as "order id OR corroborated attributes"
// until TagCarry is resolved live.
//
// ── STATE MAPPING IS FAIL-CLOSED, AND DRIVEN OFF THE FILLED QUANTITY ────────
// A Kotak `ordSt` we do not RECOGNIZE maps to `OrderState::Unknown`. Never a
// guess, never a default-to-terminal. For a recognized status the canonical state
// is then derived by `fillnorm::normalize_fill` — the quantity-first rule — so a
// broker that says "complete" while reporting `fldQty < qty` is read as
// PartiallyFilled, not Filled.
//
// ABSENT IS NOT ZERO. The order TOTAL is read through a list of candidate Kotak
// field spellings, and "none of them was present" is tracked distinctly from
// "the total is 0". It has to be: `fillnorm` derives pending as
// `max(0, total - filled)`, so a total defaulted to 0 makes ANY fill look like
// `filled > 0 && pending == 0` — i.e. FILLED, which is terminal-absorbing in the
// lifecycle FSM. A working order 30-filled-of-50 seen through one unexpected
// field name would become permanently, wrongly terminal. So: when the total is
// UNKNOWN, a WORKING status can never exceed PartiallyFilled (or Acknowledged
// with no fill) — a status that says the order is live is never allowed to
// produce a terminal state on absent evidence.
//
// A field that is PRESENT but unparseable is worse than absent (see
// `domain::parse_decimal_paise` — money is never read best-effort). The
// fail-closed response is graded by what the read is USED for:
//
//   fetch_orders()    -> that ROW's state becomes Unknown, and it is not
//                        attributed. Orders must stay ENUMERABLE, so the row is
//                        still reported; the engine just reconciles it.
//   fetch_trades()    -> the row is reported but its client_ref is cleared. A
//                        fill we cannot read is never folded into one of our
//                        signals as though we understood it.
//   fetch_positions() -> the WHOLE read fails. A net quantity is what an exit is
//                        sized off, and a leg silently read as zero under-reports
//                        a live short.
//   fetch_funds()     -> the WHOLE read fails, for the same reason: the margin
//                        gates would size real risk off the number.
//
// And a broker reporting `fldQty > qty` has its fill CLAMPED to the ordered
// quantity, so a nonsense over-fill can never size an exit above what was ordered.
//
// WHY THE RAW KOTAK STRING IS NEVER HANDED TO fillnorm: `fillnorm` classifies
// terminal-by-status with substring matching, and Kotak's vocabulary contains
// "not cancelled" and "cancel pending" — a live, still-working order in both
// cases. A substring match on "cancel" would declare it Cancelled and tell the
// engine it is flat while the order is working at the exchange. The adapter
// therefore classifies the Kotak vocabulary EXPLICITLY first, then hands fillnorm
// a neutral, already-classified token so only its quantity logic applies.
//
// MONEY: integer paise only; Kotak's decimal-string prices are parsed with the
// same no-float decimal→paise arithmetic the instrument master uses.
// SECRETS: credentials live only inside the REST client's per-call headers; every
// outward string is already scrubbed by `map_kotak_error`.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "broker_exec/adapters/kotak/kotak_rest_client.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::adapters::kotak {

// KNOWN LIMITATIONS (tier-2 gate — see docs/kotak-min-qty-smoke.md)
// ─────────────────────────────────────────────────────────────────
// This adapter is certified at TIER-1 only: it passes the broker-agnostic
// conformance kit against an in-memory recorded Kotak endpoint with NO network
// and NO live credentials. `kotak_capabilities()` therefore stays entirely
// Unknown — a fixture we authored cannot certify an endpoint we never contacted.
// The gaps below are real and intentional at this tier:
//
//   * ENDPOINT PATHS AND jData FIELD NAMES ARE A RECORDED ASSUMPTION taken from
//     the public Neo documentation (see kotak_transport.hpp). Tier-2 verifies
//     them live; until then a 404 is deliberately mapped ReconcileFirst, not
//     "no such order".
//
//   * square_off(broker_order_id) is NOT IMPLEMENTED and returns a typed
//     `NotSupported` / `DoNotRetry` error. It previously issued a cancel and
//     returned ok, which is FAIL-OPEN in the one situation the call exists for:
//     against a FILLED position a cancel is a no-op, so the caller was told "you
//     are flat" while the position was still on. A real flatten is a market exit
//     sized and side-flipped off the live net position; until that exists, an
//     explicit refusal is the only safe answer. `SquareOff` therefore cannot be
//     promoted past `Unknown` (docs/kotak-min-qty-smoke.md step 6).
//
//   * CORRELATION STATE IS IN-MEMORY ONLY. `id_to_ref_` / the pending set do not
//     survive a process restart, so after a crash NEITHER rung can name a
//     previously placed order — every such row comes back with an empty
//     client_ref. This is safe, not silent: the intent is already durable in the
//     IntentLog (fsync-before-send), so the order is enumerable and the
//     reconcile-first UNKNOWN posture holds it under an operator alert. What is
//     NOT available after a restart is automatic re-attribution. Rehydrating the
//     correlation maps from the IntentLog on startup is a tracked follow-up.
//
//   * The exchange SEGMENT is INFERRED from the trading symbol by a substring
//     heuristic ("FUT"/"CE"/"PE" -> `nse_fo`, else `nse_cm`) because OrderIntent
//     carries no exchange. This can MIS-ROUTE a cash equity whose symbol merely
//     contains those letters (e.g. "PETRONET" matches "PE"). Production MUST
//     resolve the segment from the instrument master (Story 2.6).
//
//   * RESOLVED (IMP-11), with ONE spelling caveat. A StopLoss (SL) order no
//     longer sends the same number as both the limit price and the trigger:
//     OrderIntent carries a distinct `std::optional<Price> trigger_price`, so
//     `pr` is the limit and `tp` is the real trigger ("0" when there is none).
//     THE CAVEAT: Kotak is NOT symmetric about this field's name. The quick-place
//     REQUEST spells it `tp` (the recorded jData vocabulary this adapter sends,
//     pinned by the committed fixtures) while the ORDER REPORT spells it `trgPrc`.
//     fetch_orders() therefore reads `trgPrc` / `trigPrc` / `triggerPrice` and
//     DELIBERATELY NOT `tp`: a money key that is present but unparseable fails the
//     whole row closed to Unknown, so reading a request-side spelling we were
//     never promised on a report would let one odd `tp` value turn the ENTIRE
//     order book Unknown — and an all-Unknown book freezes entries via the
//     UNKNOWN-pause. Both spellings remain a tier-2 recorded ASSUMPTION like every
//     other field name here; the live smoke verifies them.
//
//   * fetch_orders() also parses the price type back (`prcTp` / `prcType` / `pt`,
//     values MKT/L/SL/SL-M). An unrecognized value falls CLOSED to Market AND
//     suppresses the trigger for that row — publishing a Market that carries a
//     trigger would be a shape the validation gate refuses outright, so a row we
//     do not understand is never published as an armed stop.
//
//   * fetch_positions() reports net quantity but leaves `avg_price` ZERO unless
//     the payload carries an explicit average-price field: Kotak reports buy/sell
//     AMOUNTS, and deriving an average from them requires a contract multiplier
//     we have not verified. A derived-and-wrong average is worse than none.
//
//   * fetch_funds() hardcodes the limits filter to `{seg:"CASH", exch:"NSE",
//     prod:"ALL"}`. A derivatives-only or BSE deployment will read the WRONG
//     segment's margin, and the funds-freshness gate would then pass on a number
//     that does not describe the account being traded. The selector must become
//     configuration (or be derived from the intent's segment) before live use.
//
//   * The stack-level fail-closed guarantee for a colliding manual order rests on
//     the adapter withholding the correlation tuple (above). It is pinned by a
//     test that runs the adapter together with `runtime::UnknownResolver`; if
//     that suppression is ever removed, the resolver's first-match-wins rung 3
//     will adopt the collider.

class KotakBrokerAdapter final : public ports::BrokerPort {
 public:
  // Construct over a Kotak REST client (which owns the transport seam and the
  // per-call session bundle resolution). The client is held by reference and MUST
  // outlive this adapter, matching the codebase's reference-injection convention.
  explicit KotakBrokerAdapter(KotakRestClient& rest);

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

  // The most in-flight, un-anchored intents rung 2 will track. Each one is
  // look-alike BAIT — a shape a stranger's order could accidentally match — so the
  // set is deliberately bounded rather than allowed to grow for a whole session.
  // Overflow evicts the OLDEST entry, which is the one least likely to still be
  // genuinely in flight. (A time-based window would be sharper, but the adapter is
  // not given a ClockPort; adding one is a tracked follow-up. The bound is the
  // safety net, not the primary mechanism — the primary mechanism is that entries
  // are erased on both a definitive rejection and a successful anchor, so in
  // normal operation this set holds only orders whose fate is truly unknown.)
  static constexpr std::size_t kMaxPendingIntents = 256;

 private:
  // One intent we have ATTEMPTED to send, registered BEFORE the wire call so an
  // ack-lost order is still corroborable. Entries live ONLY while the intent's
  // fate is ambiguous: `anchor()` erases on a strong id, `drop_pending()` erases
  // on a definitive broker verdict.
  struct PendingIntent {
    std::string client_ref;
    std::string attr_key;  // "<symbol>|<B|S>|<qty>" — see attribute_key() in the .cpp
  };

  // Register (or refresh) the attribute key for an intent. Idempotent per
  // client_ref, a no-op for an already-anchored ref, and bounded by
  // kMaxPendingIntents. Called BEFORE the broker is contacted on place/modify.
  void register_intent(const domain::OrderIntent& intent);

  // Bind `broker_order_id -> client_ref` (rung 1) and retire the pending entry —
  // an anchored intent must never be re-corroborated by the weak rung. No-op for
  // an empty id or ref.
  void anchor(const std::string& broker_order_id, const std::string& client_ref);

  // Retire a pending entry without anchoring it: the broker gave a DEFINITIVE
  // verdict, so nothing is live and this shape must stop attracting look-alikes.
  void drop_pending(const std::string& client_ref);

  // Rung 1 lookup only: the client_ref bound to a broker order id, or empty.
  [[nodiscard]] std::string ref_for_id(const std::string& broker_order_id) const;

  KotakRestClient& rest_;

  // Correlation state. One adapter instance spans a session, so these survive the
  // place -> reconcile sequence — but NOT a process restart (see KNOWN LIMITATIONS).
  std::unordered_map<std::string, std::string> id_to_ref_;  // nOrdNo -> client_ref
  std::unordered_set<std::string> anchored_refs_;           // refs with a known id
  std::vector<PendingIntent> pending_;  // in-flight only; insertion-ordered, bounded
};

}  // namespace broker_exec::adapters::kotak
