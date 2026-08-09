#include "broker_exec/adapters/kite/kite_broker_adapter.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "broker_exec/adapters/square_off_exit.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/fillnorm/fill_normalizer.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::adapters::kite {

using json = nlohmann::json;

namespace {

// ── No-throw JSON field readers ─────────────────────────────────────────────
// Kite payloads are attacker-shaped from the adapter's perspective: every access
// is guarded (find + is_*), so a missing/oddly-typed field yields a benign empty
// value rather than an exception across the BrokerPort boundary.

[[nodiscard]] std::string json_str(const json& obj, const char* key) {
  const auto it = obj.find(key);
  if (it == obj.end() || it->is_null()) {
    return std::string{};
  }
  if (it->is_string()) {
    return it->get<std::string>();
  }
  if (it->is_number_integer()) {
    return std::to_string(it->get<std::int64_t>());
  }
  if (it->is_number_unsigned()) {
    return std::to_string(it->get<std::uint64_t>());
  }
  if (it->is_number_float()) {
    return it->dump();  // shortest round-trip text; never parsed AS a float for money
  }
  return std::string{};
}

[[nodiscard]] std::int64_t parse_int(std::string_view s) noexcept {
  std::int64_t value = 0;
  bool neg = false;
  std::size_t i = 0;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
    neg = s[i] == '-';
    ++i;
  }
  for (; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (std::isdigit(c) == 0) {
      break;
    }
    value = value * 10 + static_cast<std::int64_t>(c - '0');
  }
  return neg ? -value : value;
}

[[nodiscard]] std::int64_t json_int(const json& obj, const char* key) {
  const auto it = obj.find(key);
  if (it == obj.end() || it->is_null()) {
    return 0;
  }
  if (it->is_number_integer()) {
    return it->get<std::int64_t>();
  }
  if (it->is_number_unsigned()) {
    return static_cast<std::int64_t>(it->get<std::uint64_t>());
  }
  if (it->is_string()) {
    return parse_int(it->get<std::string>());
  }
  if (it->is_number_float()) {
    // Quantities are integral; take the integer part of the textual form, no float.
    return parse_int(it->dump());
  }
  return 0;
}

// Parse a rupee-decimal string ("123.5", "123.50", "-7.25") into integer paise
// with NO floating point: integer rupees * 100 + the first two fractional digits
// (padded). Excess fractional digits are truncated (Kite prices are 2-decimal).
[[nodiscard]] std::int64_t decimal_to_paise(std::string_view s) noexcept {
  bool neg = false;
  std::size_t i = 0;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
    neg = s[i] == '-';
    ++i;
  }
  std::int64_t rupees = 0;
  for (; i < s.size() && s[i] != '.'; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (std::isdigit(c) == 0) {
      break;
    }
    rupees = rupees * 10 + static_cast<std::int64_t>(c - '0');
  }
  std::int64_t frac = 0;
  int frac_digits = 0;
  if (i < s.size() && s[i] == '.') {
    ++i;
    for (; i < s.size() && frac_digits < 2; ++i) {
      const unsigned char c = static_cast<unsigned char>(s[i]);
      if (std::isdigit(c) == 0) {
        break;
      }
      frac = frac * 10 + static_cast<std::int64_t>(c - '0');
      ++frac_digits;
    }
  }
  while (frac_digits < 2) {  // "123.5" -> 50 paise, not 5
    frac *= 10;
    ++frac_digits;
  }
  const std::int64_t paise = rupees * 100 + frac;
  return neg ? -paise : paise;
}

// Read a money field (string decimal OR JSON number) as integer paise, no float.
[[nodiscard]] std::int64_t json_paise(const json& obj, const char* key) {
  const auto it = obj.find(key);
  if (it == obj.end() || it->is_null()) {
    return 0;
  }
  if (it->is_string()) {
    return decimal_to_paise(it->get<std::string>());
  }
  if (it->is_number_integer()) {
    return it->get<std::int64_t>() * 100;
  }
  if (it->is_number_unsigned()) {
    return static_cast<std::int64_t>(it->get<std::uint64_t>()) * 100;
  }
  if (it->is_number_float()) {
    return decimal_to_paise(it->dump());  // textual form, then exact decimal parse
  }
  return 0;
}

// A money field read as an OPTIONAL, so ABSENT is distinguishable from ZERO.
// `json_paise` collapses both to 0, which is right for an average price but wrong
// for a TRIGGER: absent (and Kite's `trigger_price: 0` on a non-stop order) mean
// "this order is not a stop", while a real armed trigger is always > 0. Anything
// non-positive therefore comes back as nullopt — the domain's "not a stop order".
[[nodiscard]] std::optional<std::int64_t> json_trigger_paise(const json& obj, const char* key) {
  const auto it = obj.find(key);
  if (it == obj.end() || it->is_null()) {
    return std::nullopt;
  }
  const std::int64_t paise = json_paise(obj, key);
  return paise > 0 ? std::optional<std::int64_t>{paise} : std::nullopt;
}

// Render integer paise as a Kite rupee-decimal string ("12350" -> "123.50"),
// no float. Used for the place price param.
[[nodiscard]] std::string paise_to_decimal(std::int64_t paise) {
  const bool neg = paise < 0;
  const std::int64_t mag = neg ? -paise : paise;
  std::string out = std::to_string(mag / 100);
  out.push_back('.');
  const std::int64_t frac = mag % 100;
  if (frac < 10) {
    out.push_back('0');
  }
  out += std::to_string(frac);
  return neg ? ("-" + out) : out;
}

// THE TAG NAMESPACE. Two DISJOINT prefixes over the same hash space: "be" marks
// an ordinary broker_exec order, "bx" a square-off EXIT. Because they are
// disjoint, a tag alone answers "is this row an exit?" — which is what lets a
// flatten refuse to flatten one (see is_exit_tag).
inline constexpr std::string_view kOrderTagPrefix = "be";
inline constexpr std::string_view kExitTagPrefix = "bx";
// 2 prefix chars + 16 hex digits = 18, comfortably inside Kite's 20-char limit.
inline constexpr std::size_t kHashedTagLength = 18;

// FNV-1a (64-bit) — a tiny, deterministic, dependency-free hash for the short
// correlation tag. The tag need not be reversible (we look it up in a map); it
// only needs to be stable and collision-resistant.
[[nodiscard]] std::string hashed_tag(std::string_view prefix, std::string_view material) {
  std::uint64_t h = 1469598103934665603ULL;
  for (const char ch : material) {
    h ^= static_cast<unsigned char>(ch);
    h *= 1099511628211ULL;
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string tag(prefix);
  for (int shift = 60; shift >= 0; shift -= 4) {
    tag.push_back(kHex[(h >> static_cast<unsigned>(shift)) & 0xFU]);
  }
  return tag;  // 18 chars, comfortably <= Kite's 20-char tag limit
}

// The correlation tag for a normal order: derived from its client_ref.
[[nodiscard]] std::string short_tag(const std::string& client_ref) {
  // "broker_exec" marker; human-recognizable.
  return hashed_tag(kOrderTagPrefix, client_ref);
}

// THE SQUARE-OFF EXIT TAG — the wire-level duplicate anchor (IMP-13, AC-2).
//
// Derived from the PARENT BROKER ORDER ID, deliberately NOT from the parent's
// client_ref. The correlation maps are in-memory: after a restart this adapter
// cannot name the parent's client_ref, so a ref-derived tag would come out
// DIFFERENT on the replay — and a different tag is a tag the fetch-first check
// cannot find, which is exactly how a crash between cancel and place ends with two
// opposite positions. The broker order id is broker truth and is the argument we
// were handed, so it is identical on every invocation, in every process.
//
// The "bx" prefix (vs "be") means a parent tag and an exit tag can never collide
// even if the hashes did: they occupy different prefixes of the tag space.
[[nodiscard]] std::string exit_tag_for(const std::string& parent_broker_order_id) {
  return hashed_tag(kExitTagPrefix, "square-off-exit:" + parent_broker_order_id);
}

// True iff `tag` was minted by exit_tag_for — i.e. THIS ROW IS ITSELF A FLATTEN.
//
// WHY square_off REFUSES SUCH A ROW (IMP-13 hardening). `square_off` is reached
// from panic paths that WALK a book: "flatten everything". Left unguarded, such a
// walk reaches the exit a previous flatten just placed, treats it as a parent,
// and flattens IT — which re-opens the position in the ORIGINAL direction, with
// nothing left to close it. Every extra pass reverses the account again. The
// refusal is cheap and absolute: an exit is never a thing to flatten.
//
// The PREFIX alone is the test, deliberately: we cannot recompute an exit tag
// without knowing which parent it belongs to, and we do not need to. A value
// inside the "bx" space is an exit of SOME parent, and that is already enough to
// know it must not be flattened. Unlike the Kotak twin (which can only consult an
// in-memory map) this survives a process restart, because the broker echoes the
// tag back on every read.
[[nodiscard]] bool is_exit_tag(std::string_view tag) noexcept {
  return tag.size() == kHashedTagLength && tag.substr(0, kExitTagPrefix.size()) == kExitTagPrefix;
}

// The typed indeterminate answer a flatten gives when it cannot READ what it would
// have to act on (AC-1e). Never a second exit order, never a cheerful ok.
[[nodiscard]] errors::Error reconcile_first_error(std::string message, std::string code) {
  errors::Error error =
      errors::make_error(errors::ErrorCategory::Unknown, std::move(message), std::move(code));
  error.action = errors::SuggestedAction::ReconcileFirst;
  return error;
}

// ── domain <-> Kite enum mapping ────────────────────────────────────────────

[[nodiscard]] const char* kite_transaction_type(domain::Side side) noexcept {
  return side == domain::Side::Sell ? "SELL" : "BUY";
}

[[nodiscard]] const char* kite_order_type(domain::OrderType type) noexcept {
  switch (type) {
    case domain::OrderType::Market:
      return "MARKET";
    case domain::OrderType::Limit:
      return "LIMIT";
    case domain::OrderType::StopLoss:
      return "SL";
    case domain::OrderType::StopLossMarket:
      return "SL-M";
  }
  return "MARKET";
}

// Map a Kite `order_type` string BACK onto the domain enum. An unrecognized value
// falls CLOSED to Market — and the caller then suppresses the trigger for that
// row (see fetch_orders), because "Market carrying a trigger" is a shape the
// validation gate refuses outright: publishing it would poison the order the next
// time anything touched it.
[[nodiscard]] domain::OrderType parse_order_type(const std::string& text) noexcept {
  if (text == "LIMIT") {
    return domain::OrderType::Limit;
  }
  if (text == "SL") {
    return domain::OrderType::StopLoss;
  }
  if (text == "SL-M") {
    return domain::OrderType::StopLossMarket;
  }
  return domain::OrderType::Market;  // MARKET, absent, or unrecognized
}

[[nodiscard]] const char* kite_product(domain::Product product) noexcept {
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

// THE FALLBACK ONLY (IMP-13, AC-4). Best-effort exchange inference from the
// trading symbol (the broker-neutral OrderIntent carries no exchange; a derivative
// symbol -> NFO, else NSE). It is used ONLY when no `ExchangeResolver` is wired —
// production wires one, built from `refdata::InstrumentMaster::resolve`, and its
// answer then overrides this outright.
//
// NOTHING MARKS THE WIRE when this fires. That was considered and rejected: a
// synthetic marker in the order params is an unknown key on a live endpoint (its
// own rejection risk) and would leak our internal posture into the broker's
// records. The consequence is stated rather than encoded — params built from a
// guess are byte-identical to params built from the master, so an operator cannot
// tell them apart after the fact. `has_exchange_resolver()` is how a composition
// root asserts, at start-up, that the guess is not in force.
[[nodiscard]] const char* infer_exchange(const std::string& symbol) noexcept {
  const bool looks_derivative = symbol.find("FUT") != std::string::npos ||
                                symbol.find("CE") != std::string::npos ||
                                symbol.find("PE") != std::string::npos;
  return looks_derivative ? "NFO" : "NSE";
}

// Map a Kite order `status` string onto the lifecycle OrderState. An unmapped /
// unknown status falls CLOSED to OrderState::Unknown (never a wrong state).
[[nodiscard]] domain::OrderState map_status(const std::string& status) noexcept {
  if (status == "COMPLETE") {
    return domain::OrderState::Filled;
  }
  if (status == "REJECTED") {
    return domain::OrderState::Rejected;
  }
  if (status == "CANCELLED" || status == "CANCELLED AMO") {
    return domain::OrderState::Cancelled;
  }
  // Live / working states at the broker — modeled as the Sent-equivalent live
  // state; fill progress arrives via later reconcile/push views.
  if (status == "OPEN" || status == "TRIGGER PENDING" || status == "OPEN PENDING" ||
      status == "VALIDATION PENDING" || status == "PUT ORDER REQ RECEIVED" ||
      status == "MODIFY PENDING" || status == "MODIFY VALIDATION PENDING") {
    return domain::OrderState::Sent;
  }
  return domain::OrderState::Unknown;  // fail-closed on an unrecognized status
}

// Build the Kite place params object from an intent + correlation tag + the
// ALREADY-RESOLVED exchange (see KiteBrokerAdapter::resolve_exchange — the
// resolution can fail, and a params builder that cannot fail must not own it).
[[nodiscard]] json build_place_params(const domain::OrderIntent& intent, const std::string& tag,
                                      const std::string& exchange) {
  json params = json::object();
  params["tradingsymbol"] = intent.symbol;
  params["exchange"] = exchange;
  params["transaction_type"] = kite_transaction_type(intent.side);
  params["order_type"] = kite_order_type(intent.order_type);
  params["product"] = kite_product(intent.product);
  params["quantity"] = intent.quantity.value();
  params["validity"] = "DAY";
  params["tag"] = tag;
  // Price is meaningful only for priced order types; send it as a rupee-decimal
  // string so no float ever touches the money path.
  if (intent.order_type == domain::OrderType::Limit ||
      intent.order_type == domain::OrderType::StopLoss) {
    params["price"] = paise_to_decimal(intent.price.paise());
  }
  // The TRIGGER is its own number now (IMP-11), read from the intent's
  // `trigger_price` rather than duplicated from `price`. A stop order whose
  // trigger is ABSENT emits NO `trigger_price` field at all: Kite then rejects the
  // SL/SL-M outright, which is a definitive broker verdict the dispatcher handles
  // safely. Falling back to `price` (the pre-IMP-11 behavior) would instead arm a
  // real stop at the wrong level, silently. The gate refuses this shape upstream;
  // this is the adapter's own fail-closed backstop.
  if (intent.trigger_price.has_value() &&
      (intent.order_type == domain::OrderType::StopLoss ||
       intent.order_type == domain::OrderType::StopLossMarket)) {
    params["trigger_price"] = paise_to_decimal(intent.trigger_price->paise());
  }
  return params;
}

// Pull a Kite `order_id` out of a `data` payload, tolerating string or numeric ids.
[[nodiscard]] std::string extract_order_id(const json& data) {
  if (!data.is_object()) {
    return std::string{};
  }
  return json_str(data, "order_id");
}

// ── Broker truth, as the FLATTEN needs to read it ───────────────────────────
//
// `fetch_orders()` publishes the reconciler's view and deliberately does not carry
// the ORDER TOTAL, the SIDE, the PRODUCT or the EXCHANGE. A flatten needs all four
// (the total to normalize the fill, the rest to build an exit that lands on the
// same position), so square_off parses the raw orderbook itself rather than
// widening the published view for one caller.
struct BrokerRow {
  std::string order_id;
  std::string tag;
  std::string symbol;
  std::string side;      // Kite `transaction_type`: "BUY" / "SELL"
  std::string status;
  std::string product;   // Kite `product`: MIS / CNC / NRML — echoed onto the exit
  std::string exchange;  // Kite `exchange`: where the position ACTUALLY is
  // NULLOPT means the field was ABSENT, which is NOT zero: fillnorm derives
  // pending as max(0, total - filled), so a total defaulted to 0 makes any fill
  // look terminal. A row with no readable total is refused, not guessed.
  std::optional<std::int64_t> quantity;
  std::int64_t filled = 0;
};

[[nodiscard]] std::optional<std::int64_t> json_qty(const json& obj, const char* key) {
  const auto it = obj.find(key);
  if (it == obj.end() || it->is_null()) {
    return std::nullopt;
  }
  if (it->is_string() && it->get<std::string>().empty()) {
    return std::nullopt;
  }
  return json_int(obj, key);
}

[[nodiscard]] BrokerRow read_broker_row(const json& row) {
  BrokerRow raw;
  raw.order_id = json_str(row, "order_id");
  raw.tag = json_str(row, "tag");
  raw.symbol = json_str(row, "tradingsymbol");
  raw.side = json_str(row, "transaction_type");
  raw.status = json_str(row, "status");
  raw.product = json_str(row, "product");
  raw.exchange = json_str(row, "exchange");
  raw.quantity = json_qty(row, "quantity");
  raw.filled = json_int(row, "filled_quantity");
  if (raw.filled < 0) {
    raw.filled = 0;
  }
  // CLAMP AN IMPOSSIBLE OVER-FILL. A broker reporting filled > ordered is
  // reporting garbage, and the dangerous direction is obvious: a fill of 500
  // against an order of 50 would size a 10x exit — a flatten that opens a
  // position. Cap it at what was actually ordered. (The Kotak adapter clamps
  // identically; a flatten must not depend on which broker mis-reported.)
  if (raw.quantity.has_value() && *raw.quantity > 0 && raw.filled > *raw.quantity) {
    raw.filled = *raw.quantity;
  }
  return raw;
}

// The exit's side: the opposite of the parent's. An unrecognized transaction_type
// yields nullopt — a flatten whose DIRECTION we are guessing at is not a flatten.
[[nodiscard]] std::optional<domain::Side> exit_side_for(std::string_view parent_side) noexcept {
  if (parent_side == "SELL") {
    return domain::Side::Buy;
  }
  if (parent_side == "BUY") {
    return domain::Side::Sell;
  }
  return std::nullopt;
}

}  // namespace

KiteBrokerAdapter::KiteBrokerAdapter(KiteRestClient& rest) : rest_(rest) {}

KiteBrokerAdapter::KiteBrokerAdapter(KiteRestClient& rest, ExchangeResolver exchange_resolver)
    : rest_(rest), exchange_resolver_(std::move(exchange_resolver)) {}

void KiteBrokerAdapter::set_exchange_resolver(ExchangeResolver exchange_resolver) {
  exchange_resolver_ = std::move(exchange_resolver);
}

bool KiteBrokerAdapter::has_exchange_resolver() const noexcept {
  return static_cast<bool>(exchange_resolver_);
}

Result<std::string> KiteBrokerAdapter::resolve_exchange(const std::string& symbol) const {
  if (!exchange_resolver_) {
    return std::string(infer_exchange(symbol));  // documented fallback (AC-4)
  }
  auto resolved = exchange_resolver_(symbol);
  if (!resolved) {
    // FAIL CLOSED. The authority was asked and could not answer, so we do NOT
    // silently drop back to the heuristic: sending a real order to an exchange we
    // guessed at, right after the instrument master said it does not know this
    // symbol, is the precise failure this seam exists to end. The typed error
    // propagates and nothing reaches the wire.
    return fail(resolved.error());
  }
  if (resolved.value().empty()) {
    return fail(errors::make_error(
        errors::ErrorCategory::Validation,
        "kite: the exchange resolver returned an empty exchange for the symbol",
        "KITE-EXCHANGE-EMPTY"));
  }
  return resolved.value();
}

Result<std::string> KiteBrokerAdapter::exit_exchange(const std::string& symbol,
                                                     const std::string& parent_exchange) const {
  if (!has_exchange_resolver()) {
    // No authority wired. The PARENT ROW'S OWN exchange outranks the symbol-shape
    // guess here: it is broker truth about where this position actually sits,
    // which is strictly better evidence than a substring match. The heuristic is
    // only the last resort, for a row that did not report one.
    return parent_exchange.empty() ? std::string(infer_exchange(symbol)) : parent_exchange;
  }
  auto resolved = resolve_exchange(symbol);
  if (!resolved) {
    return fail(resolved.error());  // fail-closed (AC-4); nothing reaches the wire
  }

  // ── THE DISAGREEMENT REFUSAL, AND WHY IT IS A REFUSAL AND NOT A PREFERENCE ──
  //
  // On the PLACE path the resolver is authoritative outright: there is no
  // position yet, so the instrument master is the only party that knows where the
  // contract trades, and overriding the heuristic is the whole point of the seam.
  //
  // A FLATTEN IS NOT A PLACE. It must land on the exchange where the position
  // ALREADY IS, and the parent order row is first-hand evidence of that: the
  // broker filled it there. If the resolver names a different venue, one of two
  // things is true — the instrument master is stale/mis-keyed, or the parent was
  // routed somewhere we did not expect — and BOTH readings make the same action
  // wrong. Sending the "exit" to the resolver's venue does not close anything; it
  // OPENS a fresh naked position on a second exchange while the original one stays
  // on. Preferring the parent row instead would be safer than that, but it would
  // also silently ignore an authority we were explicitly told to trust, on the one
  // call where being wrong is most expensive.
  //
  // So neither side wins: we refuse, place nothing, cancel nothing, and hand an
  // operator two exchange names that must be reconciled. This is checked BEFORE
  // any side effect for exactly that reason.
  if (!parent_exchange.empty() && resolved.value() != parent_exchange) {
    errors::Error error = errors::make_error(
        errors::ErrorCategory::DataStale,
        "kite: the exchange resolver and the parent order row disagree about where this position "
        "is; a flatten must route where the position actually is, so nothing was sent",
        "KITE-SQUAREOFF-EXCHANGEMISMATCH");
    error.action = errors::SuggestedAction::RaiseAlert;
    return fail(error);
  }
  return resolved.value();
}

std::string KiteBrokerAdapter::register_tag(const std::string& client_ref) {
  const std::string tag = short_tag(client_ref);
  // Register BEFORE the broker is contacted so an ack-lost order is recoverable.
  tag_to_ref_[tag] = client_ref;
  return tag;
}

std::string KiteBrokerAdapter::recover_client_ref(const std::string& broker_order_id,
                                                  const std::string& tag) const {
  if (!broker_order_id.empty()) {
    const auto it = id_to_ref_.find(broker_order_id);
    if (it != id_to_ref_.end()) {
      return it->second;
    }
  }
  if (!tag.empty()) {
    const auto it = tag_to_ref_.find(tag);
    if (it != tag_to_ref_.end()) {
      return it->second;
    }
  }
  return std::string{};  // fail-safe: no wrong match
}

Result<ports::BrokerAck> KiteBrokerAdapter::place(const domain::OrderIntent& intent) {
  // The exchange is resolved BEFORE the tag is registered: a fail-closed resolver
  // error must leave no trace of an order that was never attempted (a registered
  // tag is bait for the ack-lost recovery path).
  auto exchange = resolve_exchange(intent.symbol);
  if (!exchange) {
    return fail(exchange.error());
  }
  const std::string tag = register_tag(intent.client_ref);
  const json params = build_place_params(intent, tag, exchange.value());

  auto data = rest_.place_order(params);
  if (!data) {
    // Typed, already-scrubbed taxonomy Error -> the dispatcher marks the order
    // UNKNOWN (reconcile-first) without any blind retry. The order may be live;
    // its tag is already registered, so fetch_orders() will recover it.
    return fail(data.error());
  }

  const std::string order_id = extract_order_id(data.value());
  if (order_id.empty()) {
    return fail(errors::make_error(errors::ErrorCategory::Unknown,
                                   "kite: place succeeded but response carried no order_id",
                                   "KITE-PLACE-NOID"));
  }
  id_to_ref_[order_id] = intent.client_ref;
  return ports::BrokerAck{order_id, intent.client_ref};
}

Result<ports::BrokerAck> KiteBrokerAdapter::modify(const std::string& broker_order_id,
                                                   const domain::OrderIntent& intent) {
  auto exchange = resolve_exchange(intent.symbol);
  if (!exchange) {
    return fail(exchange.error());
  }
  const std::string tag = register_tag(intent.client_ref);
  const json params = build_place_params(intent, tag, exchange.value());

  auto data = rest_.modify_order(broker_order_id, params);
  if (!data) {
    return fail(data.error());
  }
  std::string order_id = extract_order_id(data.value());
  if (order_id.empty()) {
    order_id = broker_order_id;  // Kite modify echoes the same id; fall back to it
  }
  id_to_ref_[order_id] = intent.client_ref;
  return ports::BrokerAck{order_id, intent.client_ref};
}

Result<ports::Ok> KiteBrokerAdapter::cancel(const std::string& broker_order_id) {
  auto data = rest_.cancel_order(broker_order_id, json::object());
  if (!data) {
    return fail(data.error());
  }
  return ports::ok();
}

Result<ports::Ok> KiteBrokerAdapter::square_off(const std::string& broker_order_id) {
  return square_off_banded(broker_order_id, std::nullopt, std::nullopt);
}

Result<ports::Ok> KiteBrokerAdapter::square_off_banded(const std::string& broker_order_id,
                                                       std::optional<domain::Price> band_lower,
                                                       std::optional<domain::Price> band_upper) {
  if (broker_order_id.empty()) {
    return fail(errors::make_error(errors::ErrorCategory::Validation,
                                   "kite: square_off requires a broker order id",
                                   "KITE-SQUAREOFF-NOID"));
  }
  const std::string exit_tag = exit_tag_for(broker_order_id);

  // ── (a) BROKER TRUTH FIRST ────────────────────────────────────────────────
  // Not the local cache, not a push. This ONE read serves both jobs: it resolves
  // the parent's canonical fill, and it is the fetch-first duplicate check (AC-2).
  auto book = rest_.orders();
  if (!book) {
    // A read failure leaves the parent's fill unknown. Sizing an exit off a number
    // we could not read is how a flatten becomes a reversal.
    return fail(book.error());
  }
  if (!book.value().is_array()) {
    return fail(reconcile_first_error("kite: the orderbook payload was not an array",
                                      "KITE-SQUAREOFF-BOOKSHAPE"));
  }

  std::optional<BrokerRow> parent;
  std::optional<BrokerRow> existing_exit;
  for (const json& row : book.value()) {
    if (!row.is_object()) {
      continue;
    }
    BrokerRow raw = read_broker_row(row);
    if (!raw.order_id.empty() && raw.order_id == broker_order_id) {
      parent = raw;
      continue;
    }
    if (!raw.tag.empty() && raw.tag == exit_tag) {
      existing_exit = raw;
    }
  }

  // ── (e) NOTHING TO ACT ON -> INDETERMINATE, never an exit ─────────────────
  if (!parent.has_value()) {
    return fail(reconcile_first_error(
        "kite: square_off could not find the order in broker truth; reconcile before flattening",
        "KITE-SQUAREOFF-NOPARENT"));
  }

  // ── NEVER FLATTEN A FLATTEN ───────────────────────────────────────────────
  // The row we were pointed at is itself a square-off exit (its tag lives in the
  // exit namespace). Flattening it would place an order in the ORIGINAL direction
  // — re-opening the position this exit was sent to close, with nothing left to
  // close it again. A panic walker that squares off every row in the book hits
  // this on its second pass and would otherwise reverse every flatten it just
  // made. Refused definitively: no read of the market changes the answer.
  if (is_exit_tag(parent->tag)) {
    errors::Error error = errors::make_error(
        errors::ErrorCategory::Validation,
        "kite: square_off was asked to flatten a square-off EXIT order; flattening an exit "
        "re-opens the position it closed, so nothing was sent",
        "KITE-SQUAREOFF-SELFEXIT");
    error.action = errors::SuggestedAction::DoNotRetry;
    return fail(error);
  }

  const domain::OrderState reported = map_status(parent->status);
  if (reported == domain::OrderState::Unknown) {
    // An `status` we do not recognize. We do not know whether this order is
    // working, filled or dead — so we neither cancel it nor size an exit off it.
    return fail(reconcile_first_error(
        "kite: square_off read an unrecognized order status; reconcile before flattening",
        "KITE-SQUAREOFF-UNKNOWNSTATE"));
  }
  if (!parent->quantity.has_value()) {
    // ABSENT IS NOT ZERO (the Kotak lesson, applied here): fillnorm derives pending
    // as max(0, total - filled), so a missing total silently makes every fill look
    // complete. Refuse the row instead.
    return fail(reconcile_first_error(
        "kite: square_off could not read the order quantity; reconcile before flattening",
        "KITE-SQUAREOFF-NOQTY"));
  }

  // ── (a cont.) THE CANONICAL FILLED QUANTITY ───────────────────────────────
  // The status is classified EXPLICITLY first (map_status is fail-closed) and
  // fillnorm is then handed a NEUTRAL token, so only its quantity-first logic
  // applies: a broker saying "COMPLETE" while reporting a short fill is read as a
  // PARTIAL, and the exit is sized off what actually executed. `from_reconcile` is
  // true because this IS an authoritative read — the one basis on which fillnorm
  // permits an exit to be sized at all.
  const char* neutral_status = reported == domain::OrderState::Filled ? "complete" : "open";
  const fillnorm::FillSnapshot snap = fillnorm::normalize_fill(
      neutral_status, parent->filled, *parent->quantity, /*from_reconcile=*/true);
  const std::int64_t exit_qty = fillnorm::exit_qty_for(snap);

  // ── EVERY REFUSAL THAT CAN BE DECIDED FROM A READ IS DECIDED *HERE* ───────
  //
  // BEFORE the cancel, i.e. before this call has changed anything at the broker.
  // The ordering is the fix, not a tidy-up: the side lookup and the exchange
  // resolution used to sit AFTER the cancel and immediately before the place, so
  // a resolver that returned an error left the working remainder CANCELLED and
  // the already-filled quantity NAKED — a square_off that made the position
  // strictly harder to manage and then reported failure. `place()` has always
  // resolved first for the same reason; the flatten now matches it.
  //
  // Anything that still cannot be decided here (whether an exit already exists,
  // whether it covers the position) genuinely needs the cancel to have happened
  // first, and stays below.
  std::optional<domain::Side> side;
  std::string exchange;
  if (exit_qty > 0) {
    side = exit_side_for(parent->side);
    if (!side.has_value()) {
      return fail(reconcile_first_error(
          "kite: square_off could not read the order's transaction type; reconcile before "
          "flattening",
          "KITE-SQUAREOFF-NOSIDE"));
    }
    auto resolved = exit_exchange(parent->symbol, parent->exchange);
    if (!resolved) {
      return fail(resolved.error());
    }
    exchange = std::move(resolved.value());
  }

  // ── (b) CANCEL THE WORKING REMAINDER ──────────────────────────────────────
  // Skipped when the order is already terminal (nothing to cancel). Note this is
  // driven off the CANONICAL state, not the broker's word: a "COMPLETE" that only
  // filled half still has a live remainder to pull.
  const bool terminal = reported == domain::OrderState::Cancelled ||
                        reported == domain::OrderState::Rejected ||
                        snap.canonical_state == domain::OrderState::Filled;
  if (!terminal) {
    auto cancelled = rest_.cancel_order(broker_order_id, json::object());
    if (!cancelled) {
      // TOLERATED: the remainder went terminal underneath us. That is the state we
      // were trying to reach, so it is not an error (AC-1b).
      if (cancelled.error().category != errors::ErrorCategory::OrderNotFound) {
        // Anything else aborts BEFORE the exit. Adding an opposite order while a
        // live remainder can still fill does not flatten the position, it reverses
        // it — and an ambiguous cancel additionally means the filled quantity we
        // just measured may already be stale.
        return fail(cancelled.error());
      }
    }
  }

  // ── (d) ZERO FILLED -> CANCEL-ONLY IS A COMPLETE SQUARE-OFF ───────────────
  if (exit_qty <= 0) {
    return ports::ok();
  }

  // ── (c/AC-2) IS THE EXIT ALREADY AT THE BROKER? ───────────────────────────
  // Checked from the snapshot read above, BEFORE anything is placed. This is what
  // makes a double invocation — and a crash between the cancel and the place —
  // safe: the exit's tag is a pure function of the parent broker order id, so the
  // replay recognizes an exit this process may never have seen.
  if (existing_exit.has_value()) {
    // AN EXIT EXISTING IS NOT THE SAME AS AN EXIT WORKING. The guard used to
    // special-case only REJECTED and let everything else fall into `return ok()`,
    // which meant a CANCELLED exit — or one carrying a status from a vocabulary we
    // do not know — reported the position FLAT while it was still fully on. Drive
    // the answer off the CANONICAL state instead, so every value map_status can
    // produce has a deliberate arm.
    const domain::OrderState exit_state = map_status(existing_exit->status);

    // DEAD EXITS: rejected, or cancelled after the fact (an operator, the broker's
    // RMS, or an exchange-side purge). Both mean the same thing — the broker is
    // not going to close this position — and neither may be answered with `ok`,
    // nor with a silent re-fire. This is the one outcome that must reach a human.
    if (exit_state == domain::OrderState::Rejected ||
        exit_state == domain::OrderState::Cancelled) {
      errors::Error error = errors::make_error(
          errors::ErrorCategory::BrokerRejected,
          exit_state == domain::OrderState::Rejected
              ? "kite: a prior square-off exit for this order was REJECTED by the broker; the "
                "position is still open and needs an operator"
              : "kite: a prior square-off exit for this order was CANCELLED; the position is "
                "still open and needs an operator",
          exit_state == domain::OrderState::Rejected ? "KITE-SQUAREOFF-EXITREJECTED"
                                                     : "KITE-SQUAREOFF-EXITCANCELLED");
      error.action = errors::SuggestedAction::RaiseAlert;
      return fail(error);
    }
    // AN UNREADABLE EXIT STATE is the indeterminate leg of AC-1e, applied to the
    // exit rather than the parent: we cannot tell whether this order is going to
    // close the position, so we neither claim it did nor fire a second one.
    if (exit_state == domain::OrderState::Unknown) {
      return fail(reconcile_first_error(
          "kite: square_off read an unrecognized status on the exit already at the broker; "
          "reconcile before flattening",
          "KITE-SQUAREOFF-EXITUNKNOWNSTATE"));
    }

    // ── DOES THE EXIT ACTUALLY COVER THE POSITION? ──────────────────────────
    //
    // The guard above answers "is there an exit?"; this answers "is it BIG
    // ENOUGH?", and the difference is a real, ordinary race:
    //
    //   parent 50, filled 30 -> square_off cancels the remainder and exits 30;
    //   the last 20 fills in the window before the cancel lands; a replay now
    //   measures a 50-lot position, finds the 30-lot exit, and — checking only
    //   EXISTENCE — reported `ok`. 20 lots stayed open while the caller was told
    //   it was flat, which is precisely the fail-open this whole story removed,
    //   reintroduced one level down.
    //
    // A quantity we cannot read is not a size we may compare against.
    if (!existing_exit->quantity.has_value()) {
      return fail(reconcile_first_error(
          "kite: square_off could not read the quantity of the exit already at the broker; "
          "reconcile before flattening",
          "KITE-SQUAREOFF-EXITNOQTY"));
    }
    if (*existing_exit->quantity < exit_qty) {
      // WHY REFUSE RATHER THAN TOP UP — the decision, recorded so it is not
      // quietly reversed. A top-up (a second, smaller exit under its own `#X2`
      // ref) is superficially the more helpful answer and is the more dangerous
      // one:
      //   * it must be sized `required - existing`, where `existing` is the
      //     ORDERED size of an exit whose own fill state we have NOT established.
      //     If that exit is itself partially filled, or is rejected a moment
      //     later, the top-up is wrong in the direction that opens a position;
      //   * the evidence we would be acting on is the evidence that JUST proved
      //     itself stale — the parent moved under us once already in this same
      //     call, which is the only reason we are here;
      //   * it is unbounded: each replay can discover a further fill and add a
      //     further order, so a flapping parent yields a queue of small opposite
      //     legs nobody is reconciling; and
      //   * the Kotak twin cannot even tell two of its own exits apart (no tag
      //     echo), so a second one makes its attribute rung AMBIGUOUS and turns
      //     the NEXT replay into a refusal anyway — after two orders are live.
      // Refusing places nothing, cancels nothing further, and names the exact
      // shortfall to an operator, who can flatten the remainder by hand. The
      // position is under-covered either way; only one of the two options can
      // also over-sell it.
      errors::Error error = errors::make_error(
          errors::ErrorCategory::DataStale,
          "kite: the square-off exit already at the broker is SMALLER than the position now "
          "reported filled; the remainder is still open and needs an operator",
          "KITE-SQUAREOFF-EXITSHORT");
      error.action = errors::SuggestedAction::RaiseAlert;
      return fail(error);
    }
    // Covered (equal, or larger than we would send — a fill can only grow, so a
    // larger exit means somebody sized one off a later read than ours). Never a
    // second exit.
    return ports::ok();
  }

  // ── (c) THE EXIT ORDER ────────────────────────────────────────────────────
  // The ref anchors on the parent's client_ref when this process can still name it
  // (so the FSM's parent/child fold sees a child of the right parent), and on the
  // broker order id when it cannot — see adapters/square_off_exit.hpp. NOTE what
  // this ref is NOT: it never reaches the store, so no UNIQUE(client_ref)
  // constraint and no idempotency key is in force here. The duplicate guard is the
  // wire TAG plus the fetch-first check above, and nothing else.
  const std::string parent_ref = recover_client_ref(broker_order_id, parent->tag);
  const std::string exit_ref =
      adapters::exit_client_ref(parent_ref.empty() ? broker_order_id : parent_ref);

  domain::OrderIntent exit;
  exit.client_ref = exit_ref;
  exit.symbol = parent->symbol;
  exit.side = *side;
  exit.quantity = domain::Quantity::of(exit_qty);  // EXACTLY what filled; never the ordered size
  exit.order_type = domain::OrderType::Market;
  exit.product = domain::Product::Intraday;  // overridden on the wire below
  exit.strategy = "square_off";

  // A BAND-CLAMPED LIMIT, when (and only when) a WELL-FORMED band was supplied. The
  // clamp is to the edge the exit must cross, which keeps the order marketable
  // while staying inside the range the exchange will accept. A half-supplied or
  // inverted band is treated as no band at all — a malformed band cannot price
  // anything, and MARKET is the safe reading of "I do not know".
  if (band_lower.has_value() && band_upper.has_value() &&
      band_lower->paise() <= band_upper->paise()) {
    exit.order_type = domain::OrderType::Limit;
    exit.price = *side == domain::Side::Buy ? *band_upper : *band_lower;
  }

  // The exchange was resolved BEFORE the cancel (see exit_exchange and the
  // pre-cancel block above) — a resolver failure must not be able to leave a
  // cancelled remainder and a naked fill behind it.
  json params = build_place_params(exit, exit_tag, exchange);
  // The PRODUCT is echoed from broker truth rather than mapped through the domain
  // enum: exiting an NRML position with an MIS order does not close it, it opens a
  // second one. An absent product falls back to the intent's MIS mapping already in
  // the params (Kite has always required the field, so this is belt-and-braces).
  if (!parent->product.empty()) {
    params["product"] = parent->product;
  }

  // Register the exit's correlation BEFORE the wire call, exactly as place() does:
  // if the ack is lost, the exit is still live and this is what lets the next read
  // (and the next square_off) recognize it.
  tag_to_ref_[exit_tag] = exit_ref;

  auto placed = rest_.place_order(params);
  if (!placed) {
    // No blind retry. An ambiguous failure means the exit MAY be live — the tag is
    // already registered and, more importantly, the tag is re-derivable from the
    // parent id, so a replay finds it at the broker instead of firing a second one.
    return fail(placed.error());
  }
  if (const std::string exit_id = extract_order_id(placed.value()); !exit_id.empty()) {
    id_to_ref_[exit_id] = exit_ref;
  }
  return ports::ok();
}

Result<std::vector<domain::Order>> KiteBrokerAdapter::fetch_orders() {
  auto data = rest_.orders();
  if (!data) {
    return fail(data.error());  // a read failure is handled by the caller/reconciler
  }

  std::vector<domain::Order> out;
  if (!data.value().is_array()) {
    return out;
  }
  out.reserve(data.value().size());
  for (const json& ko : data.value()) {
    if (!ko.is_object()) {
      continue;
    }
    domain::Order order;
    order.broker_order_id = json_str(ko, "order_id");
    const std::string tag = json_str(ko, "tag");
    order.intent.client_ref = recover_client_ref(order.broker_order_id, tag);
    order.intent.symbol = json_str(ko, "tradingsymbol");
    order.state = map_status(json_str(ko, "status"));
    order.filled_qty = domain::Quantity::of(json_int(ko, "filled_quantity"));
    order.avg_price = domain::Price::from_paise(json_paise(ko, "average_price"));
    // The ORDER TYPE has to come back too, and it has to come back BEFORE the
    // trigger is published: a reconciled stop that reported as Market while
    // carrying a trigger would be a shape the validation gate refuses (see the
    // matrix in risk/validation_gate.cpp), so the next touch of that order would
    // fail closed on data we invented.
    order.intent.order_type = parse_order_type(json_str(ko, "order_type"));
    // Round-trip the trigger back into the domain WHERE THE PAYLOAD CARRIES ONE
    // *AND* the row is actually a stop. Kite reports `trigger_price: 0` for every
    // non-stop order, so the optional stays nullopt unless a real (positive)
    // trigger is present; and an unrecognized order_type fell closed to Market
    // above, which suppresses the trigger here — a row we did not understand
    // never gets published as an armed stop.
    const bool is_stop = order.intent.order_type == domain::OrderType::StopLoss ||
                         order.intent.order_type == domain::OrderType::StopLossMarket;
    if (is_stop) {
      if (const auto trigger = json_trigger_paise(ko, "trigger_price")) {
        order.intent.trigger_price = domain::Price::from_paise(*trigger);
      }
    }
    out.push_back(std::move(order));
  }
  return out;
}

Result<std::vector<domain::Trade>> KiteBrokerAdapter::fetch_trades() {
  auto data = rest_.trades();
  if (!data) {
    return fail(data.error());
  }

  std::vector<domain::Trade> out;
  if (!data.value().is_array()) {
    return out;
  }
  out.reserve(data.value().size());
  for (const json& kt : data.value()) {
    if (!kt.is_object()) {
      continue;
    }
    domain::Trade trade;
    trade.trade_id = json_str(kt, "trade_id");
    trade.broker_order_id = json_str(kt, "order_id");
    trade.client_ref = recover_client_ref(trade.broker_order_id, json_str(kt, "tag"));
    trade.quantity = domain::Quantity::of(json_int(kt, "quantity"));
    trade.price = domain::Price::from_paise(json_paise(kt, "average_price"));
    out.push_back(std::move(trade));
  }
  return out;
}

Result<std::vector<domain::Position>> KiteBrokerAdapter::fetch_positions() {
  auto data = rest_.positions();
  if (!data) {
    return fail(data.error());
  }

  std::vector<domain::Position> out;
  // Kite returns `{ "net": [...], "day": [...] }`; the net book is broker truth.
  if (!data.value().is_object()) {
    return out;
  }
  const auto net = data.value().find("net");
  if (net == data.value().end() || !net->is_array()) {
    return out;
  }
  out.reserve(net->size());
  for (const json& kp : *net) {
    if (!kp.is_object()) {
      continue;
    }
    domain::Position pos;
    pos.symbol = json_str(kp, "tradingsymbol");
    pos.net_qty = domain::Quantity::of(json_int(kp, "quantity"));
    pos.avg_price = domain::Price::from_paise(json_paise(kp, "average_price"));
    out.push_back(std::move(pos));
  }
  return out;
}

Result<ports::FundsSnapshot> KiteBrokerAdapter::fetch_funds() {
  auto data = rest_.margins("equity");
  if (!data) {
    return fail(data.error());
  }

  ports::FundsSnapshot funds;
  if (!data.value().is_object()) {
    return funds;
  }
  const json& seg = data.value();
  // Kite margins: `{ "available": { "live_balance": .. }, "utilised": { "debits": .. }, "net": .. }`.
  std::int64_t available_paise = 0;
  if (const auto av = seg.find("available"); av != seg.end() && av->is_object()) {
    available_paise = json_paise(*av, "live_balance");
  }
  if (available_paise == 0) {
    available_paise = json_paise(seg, "net");
  }
  std::int64_t used_paise = 0;
  if (const auto ut = seg.find("utilised"); ut != seg.end() && ut->is_object()) {
    used_paise = json_paise(*ut, "debits");
  }
  funds.available_margin = domain::Money::from_paise(available_paise);
  funds.used_margin = domain::Money::from_paise(used_paise);
  return funds;
}

}  // namespace broker_exec::adapters::kite
