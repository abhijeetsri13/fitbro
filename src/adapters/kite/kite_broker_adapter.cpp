#include "broker_exec/adapters/kite/kite_broker_adapter.hpp"

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
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

// FNV-1a (64-bit) — a tiny, deterministic, dependency-free hash for the short
// correlation tag. The tag need not be reversible (we look it up in a map); it
// only needs to be stable and collision-resistant for a client_ref.
[[nodiscard]] std::string short_tag(const std::string& client_ref) {
  std::uint64_t h = 1469598103934665603ULL;
  for (const unsigned char c : client_ref) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string tag = "be";  // "broker_exec" marker; keeps the tag human-recognizable
  for (int shift = 60; shift >= 0; shift -= 4) {
    tag.push_back(kHex[(h >> static_cast<unsigned>(shift)) & 0xFU]);
  }
  return tag;  // 18 chars, comfortably <= Kite's 20-char tag limit
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

// Best-effort exchange inference from the trading symbol (the broker-neutral
// OrderIntent carries no exchange; a derivative symbol -> NFO, else NSE). This is
// a deliberate MVP simplification: a production deployment resolves the exchange
// from the instrument master (Story 2.6) injected as ref-data.
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

// Build the Kite place params object from an intent + correlation tag.
[[nodiscard]] json build_place_params(const domain::OrderIntent& intent, const std::string& tag) {
  json params = json::object();
  params["tradingsymbol"] = intent.symbol;
  params["exchange"] = infer_exchange(intent.symbol);
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

}  // namespace

KiteBrokerAdapter::KiteBrokerAdapter(KiteRestClient& rest) : rest_(rest) {}

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
  const std::string tag = register_tag(intent.client_ref);
  const json params = build_place_params(intent, tag);

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
  const std::string tag = register_tag(intent.client_ref);
  const json params = build_place_params(intent, tag);

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
  // MVP flatten: cancel the working order at the broker. A full position-flattening
  // market exit needs the live position/side (resolved via ref-data + positions in
  // a production deployment); here we keep the contract simple and consistent — a
  // single broker call that returns ok/Error and never blindly retries.
  auto data = rest_.cancel_order(broker_order_id, json::object());
  if (!data) {
    return fail(data.error());
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
