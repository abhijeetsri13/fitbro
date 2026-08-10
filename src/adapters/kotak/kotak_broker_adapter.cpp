#include "broker_exec/adapters/kotak/kotak_broker_adapter.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "broker_exec/adapters/square_off_exit.hpp"
#include "broker_exec/domain/decimal_paise.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/fillnorm/fill_normalizer.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::adapters::kotak {

using json = nlohmann::json;

namespace {

// ── No-throw JSON field readers ─────────────────────────────────────────────
// Kotak payloads are attacker-shaped from the adapter's perspective: every access
// is guarded (find + is_*), so a missing or oddly-typed field yields a benign
// empty value rather than an exception across the BrokerPort boundary. Kotak
// spells the same datum differently across endpoint generations, hence the
// `first_*` helpers that take a fallback list of candidate names.
//
// THE THREE-WAY DISTINCTION THAT MATTERS: for numeric fields these helpers
// separate PRESENT-AND-PARSED from ABSENT from PRESENT-BUT-GARBAGE. Collapsing
// the last two into "0" is how an absent order total silently turns a working
// order into a terminal Filled (see map_order_state).

[[nodiscard]] std::string json_str(const json& obj, const char* key) {
  if (!obj.is_object()) {
    return std::string{};
  }
  const auto it = obj.find(key);
  if (it == obj.end() || it->is_null()) {
    return std::string{};
  }
  if (it->is_string()) {
    return it->get<std::string>();
  }
  // Unsigned is checked FIRST: nlohmann's is_number_integer() is true for
  // unsigned values too, so testing it first would make this branch dead and
  // round a large uint64 through a signed read.
  if (it->is_number_unsigned()) {
    return std::to_string(it->get<std::uint64_t>());
  }
  if (it->is_number_integer()) {
    return std::to_string(it->get<std::int64_t>());
  }
  if (it->is_number_float()) {
    return it->dump();  // shortest round-trip TEXT; never parsed AS a float for money
  }
  return std::string{};
}

[[nodiscard]] std::string first_str(const json& obj, std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    std::string value = json_str(obj, key);
    if (!value.empty()) {
      return value;
    }
  }
  return std::string{};
}

// First candidate key that yields an EXACTLY parseable integer. A key that is
// present but unparseable is skipped (we fall through to the next spelling) and
// sets `malformed` — the caller fails that row closed rather than inventing a
// number for it. Absent everywhere -> nullopt with `malformed` untouched.
[[nodiscard]] std::optional<std::int64_t> first_number(const json& obj,
                                                       std::initializer_list<const char*> keys,
                                                       bool& malformed) {
  bool saw_garbage = false;
  for (const char* key : keys) {
    const std::string text = json_str(obj, key);
    if (text.empty()) {
      continue;  // absent (or an empty string, which we cannot distinguish)
    }
    if (const std::optional<std::int64_t> parsed = domain::parse_int64(text)) {
      return parsed;
    }
    saw_garbage = true;
  }
  malformed = malformed || saw_garbage;
  return std::nullopt;
}

// Same contract for a money field: EXACT decimal -> integer paise, or nothing.
// Money is never read best-effort (see domain/decimal_paise.hpp).
[[nodiscard]] std::optional<std::int64_t> first_paise(const json& obj,
                                                      std::initializer_list<const char*> keys,
                                                      bool& malformed) {
  bool saw_garbage = false;
  for (const char* key : keys) {
    const std::string text = json_str(obj, key);
    if (text.empty()) {
      continue;
    }
    if (const std::optional<std::int64_t> parsed = domain::parse_decimal_paise(text)) {
      return parsed;
    }
    saw_garbage = true;
  }
  malformed = malformed || saw_garbage;
  return std::nullopt;
}

// ── Kotak `ordSt` vocabulary ────────────────────────────────────────────────

[[nodiscard]] char lower_ascii(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Fold a broker status into a comparable form: ASCII-lowercase, '_' and '-'
// treated as spaces, runs of whitespace collapsed, ends trimmed. This is what
// lets ONE table cover the casings/spellings Kotak has been observed to emit
// ("TRIGGER PENDING", "Trigger_Pending", "trigger  pending").
[[nodiscard]] std::string fold_status(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  bool pending_space = false;
  for (const char c : raw) {
    const char lowered = lower_ascii(c);
    const bool is_sep = lowered == ' ' || lowered == '\t' || lowered == '_' || lowered == '-';
    if (is_sep) {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) {
      out.push_back(' ');
      pending_space = false;
    }
    out.push_back(lowered);
  }
  return out;
}

// The EXPLICIT Kotak status classification. `Unrecognized` is the fail-closed
// outcome and it is not a bug — it is the contract: a status we have not seen
// resolves to OrderState::Unknown so the engine reconciles instead of guessing.
enum class StatusClass { Unrecognized, Working, Complete, Rejected, Cancelled };

[[nodiscard]] StatusClass classify_kotak_status(std::string_view raw) {
  const std::string s = fold_status(raw);
  if (s.empty()) {
    return StatusClass::Unrecognized;
  }
  if (s == "complete") {
    return StatusClass::Complete;
  }
  if (s == "rejected") {
    return StatusClass::Rejected;
  }
  if (s == "cancelled" || s == "canceled" || s == "amo cancelled" || s == "amo canceled") {
    return StatusClass::Cancelled;
  }
  // LIVE / WORKING states. Note "not cancelled", "not modified" and "cancel
  // pending" belong HERE, not with the terminals: they mean a cancel/modify
  // REQUEST did not take effect (or has not yet), so the original order is still
  // working at the exchange. Reading them as Cancelled would tell the engine it is
  // flat while it is not — which is exactly why the raw string never reaches
  // fillnorm's substring matcher.
  static constexpr std::string_view kWorking[] = {
      "open",
      "open pending",
      "trigger pending",
      "validation pending",
      "modify pending",
      "modify validation pending",
      "modified",
      "not modified",
      "not cancelled",
      "not canceled",
      "cancel pending",
      "put order req received",
      "after market order req received",
      "amo submitted",
  };
  if (std::find(std::begin(kWorking), std::end(kWorking), std::string_view(s)) !=
      std::end(kWorking)) {
    return StatusClass::Working;
  }
  return StatusClass::Unrecognized;  // fail-closed
}

// Kotak status + quantities -> the canonical lifecycle state.
//
// THREE RULES, IN THIS ORDER:
//   1. FAIL CLOSED ON THE STATUS. An unrecognized `ordSt` is Unknown, full stop —
//      never Filled/Cancelled by inference.
//   2. ABSENT IS NOT ZERO. `total` is an optional on purpose. fillnorm derives
//      pending as max(0, total - filled), so feeding it a total of 0 that really
//      meant "we could not find the field" makes ANY fill look like
//      filled>0 && pending==0 — i.e. Filled, which the lifecycle FSM treats as
//      terminal-absorbing. A WORKING status with an unknown total therefore never
//      exceeds PartiallyFilled: a broker that says the order is live may not be
//      overruled into a terminal state by evidence we do not have.
//   3. WITH A KNOWN TOTAL, DRIVE OFF THE FILLED QUANTITY (fillnorm). The status is
//      handed over as a NEUTRAL token so only its quantity-first logic applies: a
//      "complete" reporting fldQty < qty comes back PartiallyFilled, and an "open"
//      reporting fldQty == qty comes back Filled.
[[nodiscard]] domain::OrderState map_order_state(std::string_view raw_status, std::int64_t filled,
                                                 std::optional<std::int64_t> total) {
  switch (classify_kotak_status(raw_status)) {
    case StatusClass::Unrecognized:
      return domain::OrderState::Unknown;
    case StatusClass::Rejected:
      return domain::OrderState::Rejected;
    case StatusClass::Cancelled:
      // A cancel can leave a partial fill behind; the state is terminal-cancelled
      // and the filled quantity travels alongside it on the Order.
      return domain::OrderState::Cancelled;
    case StatusClass::Complete:
      if (!total.has_value()) {
        // An explicit terminal claim plus an actual fill is enough to call it
        // done; a terminal claim with NO fill and no total is no evidence at all.
        return filled > 0 ? domain::OrderState::Filled : domain::OrderState::Unknown;
      }
      return fillnorm::normalize_fill("complete", filled, *total, /*from_reconcile=*/true)
          .canonical_state;
    case StatusClass::Working:
      break;
  }
  if (!total.has_value()) {
    // THE FIX THAT MATTERS: never terminal on an absent total (see rule 2).
    return filled > 0 ? domain::OrderState::PartiallyFilled : domain::OrderState::Acknowledged;
  }
  return fillnorm::normalize_fill("open", filled, *total, /*from_reconcile=*/true).canonical_state;
}

// ── domain <-> Kotak field mapping ──────────────────────────────────────────

[[nodiscard]] const char* side_code(domain::Side side) noexcept {
  return side == domain::Side::Sell ? "S" : "B";
}

// Normalize a broker-reported transaction type to the same two-letter vocabulary
// our own keys use ("B"/"S"). An unrecognized code is passed through LOWERCASED,
// which by construction can never collide with a "B"/"S" key — so a garbled side
// makes the correlation key un-matchable rather than matching the wrong intent.
[[nodiscard]] std::string fold_side_code(std::string_view raw) {
  if (!raw.empty()) {
    const char c = lower_ascii(raw.front());
    if (c == 'b') {
      return "B";
    }
    if (c == 's') {
      return "S";
    }
  }
  std::string out;
  out.reserve(raw.size());
  for (const char c : raw) {
    out.push_back(lower_ascii(c));
  }
  return out;
}

[[nodiscard]] const char* kotak_price_type(domain::OrderType type) noexcept {
  switch (type) {
    case domain::OrderType::Market:
      return "MKT";
    case domain::OrderType::Limit:
      return "L";
    case domain::OrderType::StopLoss:
      return "SL";
    case domain::OrderType::StopLossMarket:
      return "SL-M";
  }
  return "MKT";
}

[[nodiscard]] const char* kotak_product(domain::Product product) noexcept {
  switch (product) {
    case domain::Product::Intraday:
      return "MIS";
    case domain::Product::Delivery:
      return "CNC";
    case domain::Product::Margin:
    case domain::Product::Normal:
      return "NRML";
  }
  return "MIS";
}

// Best-effort exchange-segment inference from the trading symbol (OrderIntent
// carries no exchange). A deliberate MVP simplification with a documented
// mis-route risk — see KNOWN LIMITATIONS in the header. Production resolves the
// segment from the instrument master (Story 2.6).
[[nodiscard]] const char* infer_segment(const std::string& symbol) noexcept {
  const bool looks_derivative = symbol.find("FUT") != std::string::npos ||
                                symbol.find("CE") != std::string::npos ||
                                symbol.find("PE") != std::string::npos;
  return looks_derivative ? "nse_fo" : "nse_cm";
}

[[nodiscard]] bool is_priced(domain::OrderType type) noexcept {
  return type == domain::OrderType::Limit || type == domain::OrderType::StopLoss;
}

[[nodiscard]] bool is_triggered(domain::OrderType type) noexcept {
  return type == domain::OrderType::StopLoss || type == domain::OrderType::StopLossMarket;
}

// The `tp` (trigger price) value for an intent, as Kotak wants it: rupee-decimal
// TEXT, "0" meaning "no trigger". Reads the intent's REAL `trigger_price` (IMP-11)
// instead of duplicating `price`.
//
// A stop order that arrives with NO trigger sends "0", which Kotak rejects
// outright — a definitive broker verdict the dispatcher handles safely. The
// pre-IMP-11 fallback (send `price` as the trigger) would instead arm a live stop
// at the limit price, silently. `tp` is ALWAYS emitted because the key is part of
// the recorded jData shape; omitting a documented key is its own rejection risk.
[[nodiscard]] std::string trigger_field(const domain::OrderIntent& intent) {
  if (!is_triggered(intent.order_type) || !intent.trigger_price.has_value()) {
    return std::string("0");
  }
  return domain::paise_to_decimal(intent.trigger_price->paise());
}

// Map a Kotak `prcTp` price-type string BACK onto the domain enum, folded through
// the same casing/separator normalizer the status vocabulary uses (Kotak's
// spellings are not consistent across endpoint generations).
//
// An unrecognized value falls CLOSED to Market — and the caller then suppresses
// the trigger for that row (see fetch_orders), because "Market carrying a
// trigger" is a shape the validation gate refuses outright: publishing it would
// poison the order the next time anything touched it.
[[nodiscard]] domain::OrderType parse_price_type(std::string_view raw) {
  // Every arm is an EXACT match on the folded form (not a prefix test), which is
  // what keeps "SL" and "SL-M" from shadowing each other: the fold turns '-' into
  // a space, so the wire's "SL-M" arrives here as "sl m" and can never collide
  // with the "sl" arm.
  const std::string s = fold_status(raw);  // lowercased, '-'/'_' -> space, trimmed
  if (s == "l" || s == "lmt" || s == "limit") {
    return domain::OrderType::Limit;
  }
  if (s == "sl m" || s == "slm" || s == "sl mkt") {
    return domain::OrderType::StopLossMarket;
  }
  if (s == "sl" || s == "sll" || s == "stoploss") {
    return domain::OrderType::StopLoss;
  }
  return domain::OrderType::Market;  // MKT, absent, or unrecognized
}

// Restates `runtime::Dispatcher::is_reconcile_first` — an adapter may not link the
// runtime, and this decision has to agree with it or the two layers disagree about
// whether an order might be live. TRUE means "the outcome is ambiguous, the order
// may exist at the exchange"; FALSE means the broker gave a definitive verdict.
[[nodiscard]] bool is_ambiguous_outcome(const errors::Error& error) noexcept {
  return error.action == errors::SuggestedAction::ReconcileFirst ||
         error.category == errors::ErrorCategory::Timeout ||
         error.category == errors::ErrorCategory::Network ||
         error.category == errors::ErrorCategory::Unknown;
}

// The typed indeterminate answer a flatten gives when it cannot READ what it would
// have to act on (IMP-13, AC-1e). Never a second exit order, never a cheerful ok.
[[nodiscard]] errors::Error reconcile_first_error(std::string message, std::string code) {
  errors::Error error =
      errors::make_error(errors::ErrorCategory::Unknown, std::move(message), std::move(code));
  error.action = errors::SuggestedAction::ReconcileFirst;
  return error;
}

// "THE BROKER ANSWERED, AND WE COULD NOT READ ITS ANSWER" — the whole-read refusal
// on positions/funds, where there is no per-row escape hatch. THE TWIN OF THE KITE
// ADAPTER'S HELPER OF THE SAME NAME, and it must stay one: the error taxonomy is a
// cross-broker contract (CAP-13), so the same condition on both brokers has to
// resolve to the same category.
//
// NOT `Unknown`, deliberately. `reconcile::RecoveryCoordinator` reads any failed
// broker fetch as "the broker is UNREACHABLE" and, next to a local UNKNOWN order,
// escalates a DOUBLE FAULT — ManualInterventionRequired, Critical alert, terminal,
// no auto-square-off. Here the broker is reachable and answering; we refused one
// number inside a payload that arrived. `DataStale` is the honest existing
// data-quality enumerator (and what this adapter already returns for untrustworthy
// broker data); `Validation` would claim our REQUEST was bad, which it was not. The
// ACTION stays ReconcileFirst — what these reads already returned, and right for an
// idempotent read whose remedy is to go re-read broker truth. See
// `broker_answered()` in src/reconcile/recovery.cpp.
[[nodiscard]] errors::Error unreadable_payload_error(std::string message, std::string code) {
  errors::Error error =
      errors::make_error(errors::ErrorCategory::DataStale, std::move(message), std::move(code));
  error.action = errors::SuggestedAction::ReconcileFirst;
  return error;
}

// The Kotak quick-place `jData` object. EVERY value is a string — that is the
// wire contract, and it also keeps the money path textual (paise -> decimal text)
// with no float anywhere.
[[nodiscard]] json build_place_params(const domain::OrderIntent& intent) {
  json params = json::object();
  params["am"] = "NO";                          // after-market order
  params["dq"] = "0";                           // disclosed quantity
  params["es"] = infer_segment(intent.symbol);  // exchange segment
  params["mp"] = "0";                           // market protection
  params["pc"] = kotak_product(intent.product);
  params["pf"] = "N";
  params["pr"] = is_priced(intent.order_type) ? domain::paise_to_decimal(intent.price.paise())
                                              : std::string("0");
  params["pt"] = kotak_price_type(intent.order_type);
  params["qt"] = std::to_string(intent.quantity.value());
  params["rt"] = "DAY";  // retention / validity
  params["tp"] = trigger_field(intent);
  params["ts"] = intent.symbol;
  params["tt"] = side_code(intent.side);
  // NOTE THE ABSENCE: no client tag is sent. Kotak's echo of any such field is
  // UNVERIFIED, and an unknown jData key is itself a live-rejection risk on an
  // endpoint set that is still a tier-2 assumption. Correlation therefore runs on
  // the order-id map plus attribute corroboration (see the header).
  return params;
}

[[nodiscard]] json build_modify_params(const std::string& broker_order_id,
                                       const domain::OrderIntent& intent) {
  json params = json::object();
  params["am"] = "NO";
  params["dd"] = "NA";
  params["dq"] = "0";
  params["es"] = infer_segment(intent.symbol);
  params["mp"] = "0";
  params["no"] = broker_order_id;  // the order being amended
  params["pc"] = kotak_product(intent.product);
  params["pr"] = is_priced(intent.order_type) ? domain::paise_to_decimal(intent.price.paise())
                                              : std::string("0");
  params["pt"] = kotak_price_type(intent.order_type);
  params["qt"] = std::to_string(intent.quantity.value());
  params["tp"] = trigger_field(intent);
  params["ts"] = intent.symbol;
  params["vd"] = "DAY";
  return params;
}

[[nodiscard]] json build_cancel_params(const std::string& broker_order_id) {
  json params = json::object();
  params["am"] = "NO";
  params["on"] = broker_order_id;
  return params;
}

// The broker order id out of a mutation payload. `place`/`modify` answer with
// `nOrdNo`; `cancel` echoes the id under `result`.
[[nodiscard]] std::string extract_order_id(const json& payload) {
  if (!payload.is_object()) {
    return std::string{};
  }
  return first_str(payload, {"nOrdNo", "nstOrdNo", "ordNo", "orderId", "result"});
}

// The correlation key for the WEAK rung: the economic shape of the order. Built
// identically from an intent and from a broker order row so the comparison is a
// genuine wire round-trip, not an in-memory shortcut. A row whose quantity we
// could not read gets the "?" sentinel, which no intent-derived key can equal.
[[nodiscard]] std::string attribute_key(const std::string& symbol, std::string_view side,
                                        std::optional<std::int64_t> quantity) {
  std::string key = symbol;
  key.push_back('|');
  key.append(side);
  key.push_back('|');
  key += quantity.has_value() ? std::to_string(*quantity) : std::string("?");
  return key;
}

// One broker order row, parsed once so the correlation passes below can run over
// plain values instead of re-reading JSON.
struct RawOrder {
  std::string order_id;
  std::string symbol;
  std::string side;                        // folded to "B"/"S" (or an uncollidable fallback)
  std::optional<std::int64_t> quantity;    // NULLOPT means "the field was absent", not zero
  std::int64_t filled = 0;
  std::int64_t price_paise = 0;
  std::int64_t avg_paise = 0;
  // NULLOPT means "this row is not a stop order". Kept optional (unlike the other
  // money fields, which default to 0) because 0 and absent mean the SAME thing for
  // a trigger and BOTH must map to the domain's nullopt.
  std::optional<std::int64_t> trigger_paise;
  // Fails CLOSED to Market on an absent/unrecognized `prcTp`, which suppresses the
  // trigger for the row (a Market carrying a trigger is a refused shape).
  domain::OrderType order_type = domain::OrderType::Market;
  std::string status;
  // Echoed VERBATIM onto a square-off exit (IMP-13). Neither is parsed into a
  // domain enum: exiting an NRML position with an MIS order does not close it, it
  // opens a second one, and a segment we round-trip through our own inference is a
  // segment we may have guessed. Empty means "the report did not say"; the exit
  // then falls back to the same mapping a normal place uses.
  std::string product;  // `prod` / `pc` / `pCode` — the product the position is in
  std::string segment;  // `exSeg` / `es` — the exchange segment it actually sits on
  bool malformed = false;  // a field was PRESENT but unparseable -> fail this row closed
};

[[nodiscard]] RawOrder read_order_row(const json& row) {
  RawOrder raw;
  raw.order_id = first_str(row, {"nOrdNo", "nstOrdNo", "ordNo", "orderId"});
  raw.symbol = first_str(row, {"trdSym", "tsym", "sym", "trdSymbol"});
  raw.side = fold_side_code(first_str(row, {"trnsTp", "trnsTyp", "tt"}));
  raw.quantity = first_number(row, {"qty", "qt", "ordQty", "totQty"}, raw.malformed);
  raw.filled = first_number(row, {"fldQty", "flQty", "fillQty", "filledQty"}, raw.malformed)
                   .value_or(0);
  raw.price_paise = first_paise(row, {"prc", "pr", "ordPrc"}, raw.malformed).value_or(0);
  raw.avg_paise =
      first_paise(row, {"avgPrc", "avgPrice", "fldPrc", "flPrc"}, raw.malformed).value_or(0);
  // The TRIGGER, round-tripped back out of broker truth. Kotak is NOT symmetric
  // about this datum: the quick-place REQUEST spells it `tp`, the order REPORT
  // spells it `trgPrc`.
  //
  // `tp` IS DELIBERATELY NOT A READ CANDIDATE. It is the request-side spelling, so
  // on a report it is at best a coincidence and at worst something else entirely —
  // and `first_paise` flags a key that is PRESENT but unparseable as `malformed`,
  // which fails the whole row closed to Unknown. A live report carrying a
  // non-numeric `tp` would therefore turn EVERY row Unknown, and an all-Unknown
  // book freezes entries via the UNKNOWN-pause. Reading a field we were never
  // promised is not worth a self-inflicted trading halt.
  //
  // A NON-POSITIVE value is Kotak's "no trigger" encoding and collapses to
  // nullopt, so a plain limit order never comes back looking like a stop.
  raw.trigger_paise = first_paise(row, {"trgPrc", "trigPrc", "triggerPrice"}, raw.malformed);
  if (raw.trigger_paise.has_value() && *raw.trigger_paise <= 0) {
    raw.trigger_paise.reset();
  }
  raw.order_type = parse_price_type(first_str(row, {"prcTp", "prcType", "pt"}));
  raw.status = first_str(row, {"ordSt", "status", "ordStatus"});
  // Read as OPAQUE TEXT and never validated against a vocabulary: an unrecognized
  // product/segment string is still the broker's own answer about where this
  // position lives, and echoing it back is strictly safer than substituting a
  // value we derived. (They are also not money, so they cannot set `malformed`.)
  raw.product = first_str(row, {"prod", "pc", "pCode", "prcCode"});
  raw.segment = first_str(row, {"exSeg", "es", "exchSeg", "exch"});

  // A negative order quantity is nonsense, and treating it as real would let it
  // build a correlation key. Demote it to "unknown" and flag the row.
  if (raw.quantity.has_value() && *raw.quantity < 0) {
    raw.quantity.reset();
    raw.malformed = true;
  }
  if (raw.filled < 0) {
    raw.filled = 0;  // fillnorm clamps too; do it here so the reported qty agrees
  }
  // CLAMP AN IMPOSSIBLE OVER-FILL. A broker reporting fldQty > qty is reporting
  // garbage, and the dangerous direction is obvious: a fill of 500 against an
  // order of 50 would size a 10x exit. Cap it at what was actually ordered.
  if (raw.quantity.has_value() && *raw.quantity > 0 && raw.filled > *raw.quantity) {
    raw.filled = *raw.quantity;
  }
  return raw;
}

}  // namespace

KotakBrokerAdapter::KotakBrokerAdapter(KotakRestClient& rest) : rest_(rest) {}

void KotakBrokerAdapter::register_intent(const domain::OrderIntent& intent) {
  if (intent.client_ref.empty()) {
    return;  // nothing to correlate to; never register an empty anchor
  }
  if (anchored_refs_.find(intent.client_ref) != anchored_refs_.end()) {
    return;  // already has a strong id; the weak rung must not re-arm for it
  }
  const std::string key =
      attribute_key(intent.symbol, side_code(intent.side), intent.quantity.value());
  for (PendingIntent& p : pending_) {
    if (p.client_ref == intent.client_ref) {
      p.attr_key = key;  // a modify can change the economic shape; keep it current
      return;
    }
  }
  pending_.push_back(PendingIntent{intent.client_ref, key});
  // Bounded: the oldest entry is the least likely to still be in flight, and every
  // retained entry is look-alike bait for the weak rung.
  while (pending_.size() > kMaxPendingIntents) {
    pending_.erase(pending_.begin());
  }
}

void KotakBrokerAdapter::anchor(const std::string& broker_order_id,
                                const std::string& client_ref) {
  if (broker_order_id.empty() || client_ref.empty()) {
    return;
  }
  id_to_ref_[broker_order_id] = client_ref;
  anchored_refs_.insert(client_ref);
  // Retire the pending entry: this intent now has a strong id, so re-running the
  // weak rung for it could only ever produce a WEAKER answer.
  drop_pending(client_ref);
}

void KotakBrokerAdapter::drop_pending(const std::string& client_ref) {
  if (client_ref.empty()) {
    return;
  }
  pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                [&client_ref](const PendingIntent& p) {
                                  return p.client_ref == client_ref;
                                }),
                 pending_.end());
}

std::string KotakBrokerAdapter::ref_for_id(const std::string& broker_order_id) const {
  if (broker_order_id.empty()) {
    return std::string{};
  }
  const auto it = id_to_ref_.find(broker_order_id);
  return it == id_to_ref_.end() ? std::string{} : it->second;
}

Result<ports::BrokerAck> KotakBrokerAdapter::place(const domain::OrderIntent& intent) {
  // Registered BEFORE the broker is contacted: if the ack is lost, this is the
  // ONLY evidence that lets the orderbook read find the order again.
  register_intent(intent);

  auto payload = rest_.place_order(build_place_params(intent));
  if (!payload) {
    if (!is_ambiguous_outcome(payload.error())) {
      // A DEFINITIVE rejection: the broker gave a verdict and nothing is live.
      // Leaving this shape registered would let the weak rung later adopt an
      // unrelated look-alike (an operator's manual order of the same size) as if
      // it were ours. Retire it.
      drop_pending(intent.client_ref);
    }
    // Ambiguous outcomes keep their registration: the order may be live, and the
    // dispatcher will mark it UNKNOWN and reconcile rather than blindly retrying.
    return broker_exec::fail(payload.error());
  }

  const std::string order_id = extract_order_id(payload.value());
  if (order_id.empty()) {
    // Kotak said "Ok" but gave us no id. That is ambiguous, not successful, so the
    // registration STAYS: reconcile against broker truth rather than assuming
    // nothing happened.
    return broker_exec::fail(errors::make_error(
        errors::ErrorCategory::Unknown, "kotak: place acknowledged without an order number",
        "KOTAK-PLACE-NOID"));
  }
  anchor(order_id, intent.client_ref);
  return ports::BrokerAck{order_id, intent.client_ref};
}

Result<ports::BrokerAck> KotakBrokerAdapter::modify(const std::string& broker_order_id,
                                                    const domain::OrderIntent& intent) {
  register_intent(intent);

  auto payload = rest_.modify_order(build_modify_params(broker_order_id, intent));
  if (!payload) {
    if (!is_ambiguous_outcome(payload.error())) {
      drop_pending(intent.client_ref);  // definitive refusal; see place()
    }
    return broker_exec::fail(payload.error());
  }
  std::string order_id = extract_order_id(payload.value());
  if (order_id.empty()) {
    order_id = broker_order_id;  // Kotak modify echoes the same id; fall back to it
  }
  anchor(order_id, intent.client_ref);
  return ports::BrokerAck{order_id, intent.client_ref};
}

Result<ports::Ok> KotakBrokerAdapter::cancel(const std::string& broker_order_id) {
  auto payload = rest_.cancel_order(build_cancel_params(broker_order_id));
  if (!payload) {
    return broker_exec::fail(payload.error());
  }
  return ports::ok();
}

Result<ports::Ok> KotakBrokerAdapter::square_off(const std::string& broker_order_id) {
  return square_off_banded(broker_order_id, std::nullopt, std::nullopt);
}

Result<ports::Ok> KotakBrokerAdapter::square_off_banded(const std::string& broker_order_id,
                                                        std::optional<domain::Price> band_lower,
                                                        std::optional<domain::Price> band_upper) {
  // WHAT THIS REPLACED, so nobody restores it by accident: before IMP-13 this was
  // a typed NotSupported refusal, and before THAT it issued a cancel and returned
  // ok — fail-OPEN in precisely the situation square_off exists for (against a
  // FILLED position a cancel is a no-op, so the caller was told "flat" while the
  // position was still on). It is now a real flatten, protocol-identical to the
  // Kite adapter's; see that header for the step-by-step contract, and the KNOWN
  // LIMITATIONS in this one for where Kotak's evidence is weaker.
  if (broker_order_id.empty()) {
    return broker_exec::fail(errors::make_error(errors::ErrorCategory::Validation,
                                                "kotak: square_off requires a broker order id",
                                                "KOTAK-SQUAREOFF-NOID"));
  }

  // ── (a) BROKER TRUTH FIRST ────────────────────────────────────────────────
  // One read, two jobs: the parent's canonical fill, and the fetch-first check for
  // an exit that already exists (AC-2).
  auto payload = rest_.orders();
  if (!payload) {
    return broker_exec::fail(payload.error());  // an unread book cannot size an exit
  }
  if (!payload.value().is_array()) {
    return broker_exec::fail(reconcile_first_error(
        "kotak: the order book payload was not an array", "KOTAK-SQUAREOFF-BOOKSHAPE"));
  }

  std::vector<RawOrder> rows;
  rows.reserve(payload.value().size());
  for (const json& row : payload.value()) {
    if (!row.is_object()) {
      continue;
    }
    rows.push_back(read_order_row(row));
  }

  const RawOrder* parent = nullptr;
  for (const RawOrder& raw : rows) {
    if (!raw.order_id.empty() && raw.order_id == broker_order_id) {
      parent = &raw;
      break;
    }
  }

  // ── (e) NOTHING WE UNDERSTAND -> INDETERMINATE, never an exit ─────────────
  if (parent == nullptr) {
    return broker_exec::fail(reconcile_first_error(
        "kotak: square_off could not find the order in broker truth; reconcile before flattening",
        "KOTAK-SQUAREOFF-NOPARENT"));
  }
  if (parent->malformed) {
    return broker_exec::fail(reconcile_first_error(
        "kotak: square_off read an unparseable field on the order; reconcile before flattening",
        "KOTAK-SQUAREOFF-MALFORMED"));
  }

  // ── NEVER FLATTEN A FLATTEN ───────────────────────────────────────────────
  // The row we were pointed at is itself a square-off exit. Flattening it places
  // an order in the ORIGINAL direction — re-opening the position that exit was
  // sent to close, with nothing left to close it again. A panic walker that
  // squares off every row in the book hits this on its second pass and would
  // otherwise reverse every flatten it just made.
  //
  // THE HONEST LIMIT, because it differs from Kite's: this can only be decided
  // from `id_to_ref_`, which is IN-MEMORY. Kite recognizes its own exit from the
  // TAG the broker echoes back, so its guard survives a restart; ours cannot, and
  // after a crash a walker could still flatten an exit this process no longer
  // knows it placed. Closing that needs either a verified Kotak tag echo (tier-2,
  // TagCarry — smoke step 3a) or IntentLog rehydration of the correlation maps.
  // Until then this covers the common case (same process, repeated panic) and the
  // gap is stated rather than papered over.
  if (adapters::is_exit_ref(ref_for_id(broker_order_id))) {
    errors::Error error = errors::make_error(
        errors::ErrorCategory::Validation,
        "kotak: square_off was asked to flatten a square-off EXIT order; flattening an exit "
        "re-opens the position it closed, so nothing was sent",
        "KOTAK-SQUAREOFF-SELFEXIT");
    error.action = errors::SuggestedAction::DoNotRetry;
    return broker_exec::fail(error);
  }

  const domain::OrderState reported =
      map_order_state(parent->status, parent->filled, parent->quantity);
  if (reported == domain::OrderState::Unknown) {
    return broker_exec::fail(reconcile_first_error(
        "kotak: square_off read an unrecognized order status; reconcile before flattening",
        "KOTAK-SQUAREOFF-UNKNOWNSTATE"));
  }
  if (!parent->quantity.has_value()) {
    // ABSENT IS NOT ZERO. fillnorm derives pending as max(0, total - filled), so a
    // missing total makes every fill look complete — and here that would size an
    // exit off a number nobody reported.
    return broker_exec::fail(reconcile_first_error(
        "kotak: square_off could not read the order quantity; reconcile before flattening",
        "KOTAK-SQUAREOFF-NOQTY"));
  }

  // ── (a cont.) THE CANONICAL FILLED QUANTITY ───────────────────────────────
  // The Kotak vocabulary is classified EXPLICITLY first (never handed raw to
  // fillnorm — "not cancelled" and "cancel pending" are WORKING states that its
  // substring matcher would read as terminal), then fillnorm applies its
  // quantity-first rule to a neutral token. `from_reconcile` is true: this is an
  // authoritative read, which is the only basis on which an exit may be sized.
  const char* neutral_status = reported == domain::OrderState::Filled ? "complete" : "open";
  const fillnorm::FillSnapshot snap = fillnorm::normalize_fill(
      neutral_status, parent->filled, *parent->quantity, /*from_reconcile=*/true);
  const std::int64_t exit_qty = fillnorm::exit_qty_for(snap);

  // ── (b) CANCEL THE WORKING REMAINDER ──────────────────────────────────────
  const bool terminal = reported == domain::OrderState::Cancelled ||
                        reported == domain::OrderState::Rejected ||
                        snap.canonical_state == domain::OrderState::Filled;
  if (!terminal) {
    auto cancelled = rest_.cancel_order(build_cancel_params(broker_order_id));
    if (!cancelled && cancelled.error().category != errors::ErrorCategory::OrderNotFound) {
      // TOLERATED above: an already-terminal remainder is the state we wanted.
      // Anything else aborts BEFORE the exit — a live remainder that can still fill
      // turns an exit into a reversal, and an ambiguous cancel means the filled
      // quantity just measured may already be stale.
      return broker_exec::fail(cancelled.error());
    }
  }

  // ── (d) ZERO FILLED -> CANCEL-ONLY IS A COMPLETE SQUARE-OFF ───────────────
  if (exit_qty <= 0) {
    return ports::ok();
  }

  if (parent->side != "S" && parent->side != "B") {
    // fold_side_code() lower-cases anything it does not recognize, so this is a
    // side we could not read. A flatten whose DIRECTION is a guess is not a
    // flatten — it is a doubling-down.
    return broker_exec::fail(reconcile_first_error(
        "kotak: square_off could not read the order's transaction type; reconcile before "
        "flattening",
        "KOTAK-SQUAREOFF-NOSIDE"));
  }
  const domain::Side exit_side = parent->side == "S" ? domain::Side::Buy : domain::Side::Sell;
  const std::string parent_ref = ref_for_id(broker_order_id);
  const std::string exit_ref =
      adapters::exit_client_ref(parent_ref.empty() ? broker_order_id : parent_ref);

  // ── (c/AC-2) IS THE EXIT ALREADY AT THE BROKER? ───────────────────────────
  //
  // KOTAK'S EVIDENCE IS WEAKER THAN KITE'S AND THE DIFFERENCE IS STRUCTURAL, not
  // an oversight: Kite tags the exit with a value derived from the parent's broker
  // order id and echoes it back, so a replay IDENTIFIES its own exit exactly. Kotak
  // has no verified tag echo (see the header), so the same question is answered by
  // the adapter's existing two-rung ladder:
  //
  //   RUNG 1 — the strong id map: a row already bound to this exit's ref. Exact,
  //            but in-memory, so it answers only within the same process.
  //   RUNG 2 — attribute corroboration against the live book on (symbol, opposite
  //            side, a quantity no larger than the exit we would send), over rows
  //            NOT already bound to some other client_ref. This is what survives a
  //            restart, and it is the rung that carries the residual risk: a
  //            STRANGER'S order (one we never placed, so bound to nothing) of a
  //            plausible shape would be read as our exit and we would report ok
  //            without flattening. It is bounded three ways — bound-elsewhere rows
  //            are excluded outright, an over-large opposite order is somebody
  //            else's business, and the match must be UNAMBIGUOUS: two or more
  //            candidates is an indeterminate answer, not a coin flip.
  //
  // Only WORKING or COMPLETE rows count, ON BOTH RUNGS. A Rejected/Cancelled exit
  // is NOT an exit in force, and treating one as though it were would leave the
  // position open while reporting success — the exact fail-open this whole story
  // removed. Rung 1 used to apply NO status filter at all, contradicting this
  // paragraph two lines above it: a prior exit the broker had REJECTED was matched
  // by id and answered with a cheerful `ok`. It now classifies the row it found,
  // and — because a strong-id match PROVES the row is ours, exactly as Kite's tag
  // does — a dead one is escalated to an operator rather than merely refused.
  // (Rung 2 cannot make that claim: there a dead look-alike proves only that
  // somebody's order was rejected, so it is simply not a candidate.)
  //
  // AND FINDING AN EXIT IS NOT ENOUGH — IT MUST ALSO BE BIG ENOUGH. Both rungs
  // therefore carry the matched exit's QUANTITY back out; the size comparison
  // below is what stops a 30-lot exit from being reported as covering a position
  // that has since filled to 50. `-1` means "no exit found".
  std::int64_t existing_exit_qty = -1;

  bool exit_found_by_id = false;
  for (const RawOrder& raw : rows) {  // RUNG 1 — the strong id map
    // `exit_ref` cannot be empty here: `broker_order_id` was rejected empty at the
    // top of this function, so the anchor handed to exit_client_ref is non-empty
    // whichever branch supplied it. The old `|| exit_ref.empty()` guard was dead.
    if (raw.order_id.empty() || raw.order_id == broker_order_id) {
      continue;
    }
    if (ref_for_id(raw.order_id) != exit_ref) {
      continue;
    }
    const StatusClass cls = classify_kotak_status(raw.status);
    if (cls == StatusClass::Rejected || cls == StatusClass::Cancelled) {
      // THE TWIN OF KITE-SQUAREOFF-EXITREJECTED. This row is ours by broker id,
      // and the broker either refused it or it was cancelled afterwards — so the
      // position is fully on and nothing is going to close it. Never `ok`, and
      // never a silent re-fire.
      errors::Error error = errors::make_error(
          errors::ErrorCategory::BrokerRejected,
          cls == StatusClass::Rejected
              ? "kotak: a prior square-off exit for this order was REJECTED by the broker; the "
                "position is still open and needs an operator"
              : "kotak: a prior square-off exit for this order was CANCELLED; the position is "
                "still open and needs an operator",
          cls == StatusClass::Rejected ? "KOTAK-SQUAREOFF-EXITREJECTED"
                                       : "KOTAK-SQUAREOFF-EXITCANCELLED");
      error.action = errors::SuggestedAction::RaiseAlert;
      return broker_exec::fail(error);
    }
    if (cls == StatusClass::Unrecognized || raw.malformed) {
      return broker_exec::fail(reconcile_first_error(
          "kotak: square_off could not read the state of the exit already at the broker; "
          "reconcile before flattening",
          "KOTAK-SQUAREOFF-EXITUNKNOWNSTATE"));
    }
    if (!raw.quantity.has_value()) {
      return broker_exec::fail(reconcile_first_error(
          "kotak: square_off could not read the quantity of the exit already at the broker; "
          "reconcile before flattening",
          "KOTAK-SQUAREOFF-EXITNOQTY"));
    }
    exit_found_by_id = true;
    existing_exit_qty = *raw.quantity;
    break;
  }

  std::size_t exit_candidates = 0;
  if (!exit_found_by_id) {  // RUNG 2 — attribute corroboration
    for (const RawOrder& raw : rows) {
      if (raw.order_id.empty() || raw.order_id == broker_order_id || raw.malformed) {
        continue;
      }
      // A ROW WE HAVE ALREADY BOUND TO A DIFFERENT SIGNAL IS NOT A CANDIDATE, and
      // this is the difference between a flatten and a no-op. Attribute
      // corroboration alone cannot tell our exit from a LEGITIMATE opposite-side
      // order of the same shape — the second leg of a hedge pair, a reversal, or
      // another strategy's position under 6-4 isolation. A SELL 50 parent sitting
      // next to a genuine BUY 50 therefore used to make square_off adopt the BUY
      // as "our exit", place NOTHING, and return ok while the short stayed fully
      // on. But rung 1 already KNOWS that BUY belongs to someone else: it is bound
      // to a different client_ref. Skipping bound-elsewhere rows costs nothing
      // (they were never corroboration candidates in the first place) and removes
      // the entire class.
      const std::string bound = ref_for_id(raw.order_id);
      if (!bound.empty() && bound != exit_ref) {
        continue;
      }
      if (raw.symbol != parent->symbol || raw.side != side_code(exit_side)) {
        continue;
      }
      // SIZE IS A FILTER, NOT AN EQUALITY TEST ANYMORE. It used to demand
      // `*raw.quantity == exit_qty`, which made a legitimately-SHORT prior exit
      // (the parent kept filling after that exit was sized) invisible to this
      // rung — so the flatten saw "no exit" and placed a SECOND, FULL-SIZE one.
      // 30 + 50 against a 50-lot position is a 30-lot naked reversal: the single
      // worst outcome this call can produce. Any working opposite-side order up to
      // the size we would send is now a candidate, and the size comparison below
      // decides what to do about it.
      //
      // The upper bound is kept because a fill can only GROW: an exit larger than
      // the position could never have been sized off this parent, so a bigger
      // opposite order is somebody else's business and must not suppress our exit.
      if (!raw.quantity.has_value() || *raw.quantity <= 0 || *raw.quantity > exit_qty) {
        continue;
      }
      const StatusClass cls = classify_kotak_status(raw.status);
      if (cls == StatusClass::Working || cls == StatusClass::Complete) {
        ++exit_candidates;
        existing_exit_qty = *raw.quantity;
      }
    }
  }

  if (exit_candidates > 1) {
    return broker_exec::fail(reconcile_first_error(
        "kotak: square_off found more than one order matching this exit's shape; reconcile before "
        "flattening",
        "KOTAK-SQUAREOFF-AMBIGUOUS"));
  }
  if (exit_found_by_id || exit_candidates == 1) {
    // ── DOES THE EXIT ACTUALLY COVER THE POSITION? ──────────────────────────
    // See the Kite twin for the full argument; the decision is identical and
    // deliberately so (AC-3 parity): an exit SMALLER than the position now
    // reported filled is an operator condition, never a top-up order and never a
    // cheerful ok. A top-up would be sized off an exit whose own fill state we
    // have not established, acting on evidence that just proved itself stale —
    // and here it would additionally create a SECOND look-alike, which makes rung
    // 2 ambiguous and turns every later replay into a refusal anyway.
    if (existing_exit_qty < exit_qty) {
      errors::Error error = errors::make_error(
          errors::ErrorCategory::DataStale,
          "kotak: the square-off exit already at the broker is SMALLER than the position now "
          "reported filled; the remainder is still open and needs an operator",
          "KOTAK-SQUAREOFF-EXITSHORT");
      error.action = errors::SuggestedAction::RaiseAlert;
      return broker_exec::fail(error);
    }
    return ports::ok();  // covered — never a second exit
  }

  // ── (c) THE EXIT ORDER ────────────────────────────────────────────────────
  domain::OrderIntent exit;
  exit.client_ref = exit_ref;
  exit.symbol = parent->symbol;
  exit.side = exit_side;
  exit.quantity = domain::Quantity::of(exit_qty);  // EXACTLY what filled
  exit.order_type = domain::OrderType::Market;
  exit.product = domain::Product::Intraday;  // overridden from broker truth below
  exit.strategy = "square_off";

  // A band-clamped LIMIT when a WELL-FORMED band was supplied; MARKET otherwise.
  // The clamp is to the edge the exit must cross, so the order stays marketable
  // while remaining inside the range the exchange accepts. A half-supplied or
  // inverted band is treated as no band at all.
  if (band_lower.has_value() && band_upper.has_value() &&
      band_lower->paise() <= band_upper->paise()) {
    exit.order_type = domain::OrderType::Limit;
    exit.price = exit_side == domain::Side::Buy ? *band_upper : *band_lower;
  }

  json params = build_place_params(exit);
  // Product and segment are ECHOED from broker truth, not re-derived: exiting an
  // NRML position with an MIS order opens a second position instead of closing the
  // first, and the segment inference is the very heuristic this story is retiring.
  if (!parent->product.empty()) {
    params["pc"] = parent->product;
  }
  if (!parent->segment.empty()) {
    params["es"] = parent->segment;
  }

  // Registered BEFORE the wire call, exactly as place() does: if the ack is lost
  // the exit may still be live, and this registration is the only thing that lets
  // rung 2 recognize it on the next read.
  register_intent(exit);

  auto placed = rest_.place_order(params);
  if (!placed) {
    if (!is_ambiguous_outcome(placed.error())) {
      drop_pending(exit.client_ref);  // definitive refusal; nothing is live (see place())
    }
    return broker_exec::fail(placed.error());
  }
  if (const std::string exit_id = extract_order_id(placed.value()); !exit_id.empty()) {
    anchor(exit_id, exit.client_ref);
  }
  return ports::ok();
}

Result<std::vector<domain::Order>> KotakBrokerAdapter::fetch_orders() {
  auto payload = rest_.orders();
  if (!payload) {
    return broker_exec::fail(payload.error());  // a read failure is the caller's to handle
  }

  std::vector<domain::Order> out;
  if (!payload.value().is_array()) {
    return out;
  }

  // ── Pass 1: parse the rows once ──────────────────────────────────────────
  std::vector<RawOrder> rows;
  rows.reserve(payload.value().size());
  for (const json& row : payload.value()) {
    if (!row.is_object()) {
      continue;
    }
    rows.push_back(read_order_row(row));
  }

  // ── Pass 2: RUNG 1, broker order id (strong, unambiguous) ────────────────
  std::vector<std::string> refs(rows.size());
  std::unordered_map<std::string, std::size_t> unmatched_rows_per_key;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    refs[i] = ref_for_id(rows[i].order_id);
    if (refs[i].empty()) {
      ++unmatched_rows_per_key[attribute_key(rows[i].symbol, rows[i].side, rows[i].quantity)];
    }
  }

  // ── Pass 3: RUNG 2, attribute corroboration (weak, ack-lost only) ────────
  // Candidates are the intents still in flight — anchored ones were retired, and
  // definitively-rejected ones were dropped, so nothing here is bait for a shape
  // that is not genuinely outstanding. The pairing is admitted ONLY when it is
  // unambiguous in BOTH directions: exactly one such intent AND exactly one
  // still-unmatched broker row carry the key. A colliding manual order, or two
  // identical lots of our own, therefore resolves to NOTHING rather than to a
  // coin flip.
  std::unordered_map<std::string, std::size_t> candidate_refs_per_key;
  std::unordered_map<std::string, std::string> sole_ref_for_key;
  for (const PendingIntent& p : pending_) {
    if (p.attr_key.empty()) {
      continue;
    }
    ++candidate_refs_per_key[p.attr_key];
    sole_ref_for_key[p.attr_key] = p.client_ref;
  }

  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (!refs[i].empty()) {
      continue;
    }
    if (rows[i].malformed) {
      continue;  // we do not understand this row; do not attach a signal to it
    }
    const std::string key = attribute_key(rows[i].symbol, rows[i].side, rows[i].quantity);
    if (unmatched_rows_per_key[key] != 1) {
      continue;  // ambiguous at the broker end -> claim nothing (fail-closed)
    }
    const auto candidates = candidate_refs_per_key.find(key);
    if (candidates == candidate_refs_per_key.end() || candidates->second != 1) {
      continue;  // ambiguous (or absent) at our end -> claim nothing (fail-closed)
    }
    refs[i] = sole_ref_for_key[key];
    // Promote to the strong rung immediately: the weak rung is consulted at most
    // once per intent, and later reads (including trades) key off the id.
    anchor(rows[i].order_id, refs[i]);
  }

  // ── Pass 4: build the domain view ────────────────────────────────────────
  out.reserve(rows.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const RawOrder& raw = rows[i];
    const bool correlated = !refs[i].empty();

    domain::Order order;
    order.broker_order_id = raw.order_id;
    order.intent.client_ref = refs[i];  // EMPTY when neither rung matched (fail-closed)
    order.intent.symbol = raw.symbol;
    // A row whose fields we could not parse is a row we do not understand: fail it
    // closed to Unknown so the engine reconciles instead of trusting our reading.
    order.state = raw.malformed ? domain::OrderState::Unknown
                                : map_order_state(raw.status, raw.filled, raw.quantity);
    order.filled_qty = domain::Quantity::of(raw.filled);
    order.avg_price = domain::Price::from_paise(raw.avg_paise);

    // THE CORRELATION TUPLE IS PUBLISHED ONLY FOR ROWS WE POSITIVELY CORRELATED.
    // `runtime::UnknownResolver`'s rung 3 re-runs attribute corroboration on
    // exactly (symbol, side, quantity, price) — first-match-wins, with NO
    // ambiguity check. Handing it these fields for a row we deliberately REFUSED
    // to attribute would let it overturn that refusal one layer up and adopt a
    // colliding manual order. Withholding them makes the refusal stick at the
    // stack level. Everything reconciliation actually needs (id, symbol, state,
    // fill progress) is still published. See the header for the full rationale.
    if (correlated) {
      order.intent.side = raw.side == "S" ? domain::Side::Sell : domain::Side::Buy;
      if (raw.quantity.has_value()) {
        order.intent.quantity = domain::Quantity::of(*raw.quantity);
      }
      order.intent.price = domain::Price::from_paise(raw.price_paise);
      // The order TYPE and the trigger travel with the rest of the correlation
      // tuple, and for the same reason: they are published only for a row we
      // POSITIVELY correlated. The type is set FIRST because it gates the trigger.
      order.intent.order_type = raw.order_type;
      // A trigger is published ONLY on a row that is actually a stop. An
      // absent/unrecognized `prcTp` fell closed to Market above and therefore
      // suppresses it — a row we did not understand is never published as an armed
      // stop, and "Market carrying a trigger" (a shape the validation gate refuses)
      // can never be synthesized here.
      const bool is_stop = raw.order_type == domain::OrderType::StopLoss ||
                           raw.order_type == domain::OrderType::StopLossMarket;
      if (is_stop && raw.trigger_paise.has_value()) {
        order.intent.trigger_price = domain::Price::from_paise(*raw.trigger_paise);
      }
    }
    out.push_back(std::move(order));
  }
  return out;
}

Result<std::vector<domain::Trade>> KotakBrokerAdapter::fetch_trades() {
  auto payload = rest_.trades();
  if (!payload) {
    return broker_exec::fail(payload.error());
  }

  std::vector<domain::Trade> out;
  if (!payload.value().is_array()) {
    return out;
  }
  out.reserve(payload.value().size());
  for (const json& row : payload.value()) {
    if (!row.is_object()) {
      continue;
    }
    bool malformed = false;
    domain::Trade trade;
    trade.trade_id = first_str(row, {"trdNo", "tradeId", "flId", "exTrdNo", "nOrdNo"});
    trade.broker_order_id = first_str(row, {"nOrdNo", "nstOrdNo", "ordNo", "orderId"});
    // Trades correlate by broker order id ONLY. A trade row carries no
    // independent corroboration surface (its quantity is the FILL, not the order
    // size), so inventing an attribute rung here would be a guess. It is also
    // unnecessary: fetch_orders() anchors an ack-lost order's id on the first
    // reconcile, after which this lookup succeeds.
    trade.client_ref = ref_for_id(trade.broker_order_id);
    const std::int64_t qty =
        first_number(row, {"fldQty", "flQty", "qty", "trdQty"}, malformed).value_or(0);
    trade.quantity = domain::Quantity::of(qty > 0 ? qty : 0);
    trade.price = domain::Price::from_paise(
        first_paise(row, {"avgPrc", "flPrc", "prc", "trdPrc"}, malformed).value_or(0));
    if (malformed) {
      // The row is still REPORTED — never hide an execution — but it is not
      // ATTRIBUTED. A fill whose quantity or price we could not read must not be
      // folded into one of our signals as though we understood it; leaving the
      // client_ref empty routes it to the operator instead.
      trade.client_ref.clear();
    }
    out.push_back(std::move(trade));
  }
  return out;
}

Result<std::vector<domain::Position>> KotakBrokerAdapter::fetch_positions() {
  auto payload = rest_.positions();
  if (!payload) {
    return broker_exec::fail(payload.error());
  }

  std::vector<domain::Position> out;
  if (!payload.value().is_array()) {
    return out;
  }
  // A position quantity is what an exit is sized off. A WRONG net (an unparseable
  // leg silently read as zero) under-reports a live short, so the whole snapshot
  // fails closed rather than being published with a hole in it — same policy as
  // fetch_funds(). This is an idempotent read; the caller retries or escalates.
  bool malformed = false;
  out.reserve(payload.value().size());
  for (const json& row : payload.value()) {
    if (!row.is_object()) {
      continue;
    }
    domain::Position pos;
    pos.symbol = first_str(row, {"trdSym", "tsym", "sym"});
    // Kotak reports positions as four buckets rather than a signed net: day
    // (fl*) plus carry-forward (cf*). Net = all buys - all sells, integer only.
    const std::int64_t buys = first_number(row, {"flBuyQty", "buyQty"}, malformed).value_or(0) +
                              first_number(row, {"cfBuyQty"}, malformed).value_or(0);
    const std::int64_t sells = first_number(row, {"flSellQty", "sellQty"}, malformed).value_or(0) +
                               first_number(row, {"cfSellQty"}, malformed).value_or(0);
    pos.net_qty = domain::Quantity::of(buys - sells);
    // avg_price stays ZERO unless the payload states one outright: deriving it
    // from buyAmt/sellAmt needs an unverified contract multiplier, and a wrong
    // average is worse than an absent one (see KNOWN LIMITATIONS).
    pos.avg_price =
        domain::Price::from_paise(first_paise(row, {"avgPrc", "avgPrice"}, malformed).value_or(0));
    out.push_back(std::move(pos));
  }
  if (malformed) {
    return broker_exec::fail(unreadable_payload_error(
        "kotak: positions payload carried an unparseable quantity or amount",
        "KOTAK-POSITIONS-MALFORMED"));
  }
  return out;
}

Result<ports::FundsSnapshot> KotakBrokerAdapter::fetch_funds() {
  // Kotak takes the limits filter as a jData body (not a path segment as Kite
  // does). THE SELECTOR IS HARDCODED and that is a KNOWN LIMITATION: a
  // derivatives-only or BSE deployment would read the wrong segment's margin.
  // It must become configuration before live use.
  json params = json::object();
  params["seg"] = "CASH";
  params["exch"] = "NSE";
  params["prod"] = "ALL";

  auto payload = rest_.margins(params);
  if (!payload) {
    return broker_exec::fail(payload.error());
  }

  ports::FundsSnapshot funds;
  if (!payload.value().is_object()) {
    return funds;
  }
  const json& body = payload.value();
  bool malformed = false;
  const std::optional<std::int64_t> available =
      first_paise(body, {"Net", "net", "availableMargin", "AvailableMargin"}, malformed);
  const std::optional<std::int64_t> used =
      first_paise(body, {"MarginUsed", "marginUsed", "UtilizedMargin", "utilizedMargin"}, malformed);
  if (malformed) {
    // A funds figure we cannot parse EXACTLY must not be reported as a number —
    // the freshness/margin gates would size real risk off it. Fail the read.
    return broker_exec::fail(unreadable_payload_error(
        "kotak: funds payload carried an unparseable amount", "KOTAK-FUNDS-MALFORMED"));
  }
  funds.available_margin = domain::Money::from_paise(available.value_or(0));
  funds.used_margin = domain::Money::from_paise(used.value_or(0));
  return funds;
}

}  // namespace broker_exec::adapters::kotak
