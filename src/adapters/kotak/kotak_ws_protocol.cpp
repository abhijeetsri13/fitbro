#include "broker_exec/adapters/kotak/kotak_ws_protocol.hpp"

#include <cstddef>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::adapters::kotak {

using json = nlohmann::json;

namespace {

// The Kotak feed task codes: `mws` subscribes a market-watch set, `mwu`
// unsubscribes it. Recorded from the public Neo docs and pinned by fixtures.
constexpr std::string_view kSubscribeTask = "mws";
constexpr std::string_view kUnsubscribeTask = "mwu";

// `segment|token` pairs joined by '&' — the exact `scrips` payload shape.
[[nodiscard]] std::string join_scrips(std::span<const KotakSubscription> subscriptions) {
  std::string scrips;
  for (const auto& subscription : subscriptions) {
    if (!scrips.empty()) {
      scrips.push_back('&');
    }
    scrips += subscription.exchange_segment;
    scrips.push_back('|');
    scrips += subscription.instrument_token;
  }
  return scrips;
}

[[nodiscard]] std::string build_frame(std::string_view task,
                                      std::span<const KotakSubscription> subscriptions,
                                      std::string_view channel) {
  if (subscriptions.empty()) {
    return std::string{};
  }
  json frame;
  frame["type"] = std::string(task);
  frame["task"] = std::string(task);
  frame["channel"] = std::string(channel);
  frame["scrips"] = join_scrips(subscriptions);
  // `error_handler_t::replace` is required for the no-throw contract: the default
  // handler THROWS on a non-UTF-8 byte, and segment/token strings can originate
  // from a scrip-master file we did not author.
  return frame.dump(-1, ' ', /*ensure_ascii=*/false, json::error_handler_t::replace);
}

[[nodiscard]] bool has_key(const json& object, const char* key) {
  return object.find(key) != object.end();
}

[[nodiscard]] std::string string_field(const json& object, const char* key) {
  const auto it = object.find(key);
  if (it != object.end() && it->is_string()) {
    return it->get<std::string>();
  }
  return std::string{};
}

// Classify a single record object. Order-update detection comes FIRST: an order
// frame that also carried a price-like key must never be mistaken for a tick.
[[nodiscard]] KotakMessageKind classify_record(const json& record) {
  const std::string type = string_field(record, "type");

  if (type == "hb" || type == "heartbeat" || has_key(record, "hb")) {
    return KotakMessageKind::Heartbeat;
  }
  if (type == "order" || type == "ord" || has_key(record, "nOrdNo") || has_key(record, "ordSt") ||
      has_key(record, "orderId")) {
    return KotakMessageKind::OrderUpdate;
  }
  if (type == "stk" || type == "if" || type == "dp" || type == "tick" ||
      (has_key(record, "tk") && (has_key(record, "lp") || has_key(record, "ltp") ||
                                 has_key(record, "ltt") || has_key(record, "bp")))) {
    return KotakMessageKind::Tick;
  }
  // FAIL CLOSED: an unrecognized frame is never assumed to be data.
  return KotakMessageKind::Unknown;
}

}  // namespace

std::string_view to_string(KotakMessageKind kind) noexcept {
  switch (kind) {
    case KotakMessageKind::OrderUpdate:
      return "order_update";
    case KotakMessageKind::Tick:
      return "tick";
    case KotakMessageKind::Heartbeat:
      return "heartbeat";
    case KotakMessageKind::Unknown:
      return "unknown";
  }
  return "unknown";
}

std::string build_subscribe_frame(std::span<const KotakSubscription> subscriptions,
                                  std::string_view channel) {
  return build_frame(kSubscribeTask, subscriptions, channel);
}

std::string build_unsubscribe_frame(std::span<const KotakSubscription> subscriptions,
                                    std::string_view channel) {
  return build_frame(kUnsubscribeTask, subscriptions, channel);
}

Result<std::vector<KotakSubscription>> parse_subscription_frame(std::string_view frame) {
  const json parsed = json::parse(frame.begin(), frame.end(), nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return broker_exec::fail(errors::make_error(errors::ErrorCategory::Validation,
                                                "kotak: subscription frame is not a JSON object"));
  }
  const std::string task = string_field(parsed, "task");
  if (task != kSubscribeTask && task != kUnsubscribeTask) {
    return broker_exec::fail(errors::make_error(errors::ErrorCategory::Validation,
                                                "kotak: subscription frame has no known task"));
  }
  const std::string scrips = string_field(parsed, "scrips");
  if (scrips.empty()) {
    return broker_exec::fail(errors::make_error(errors::ErrorCategory::Validation,
                                                "kotak: subscription frame carried no scrips"));
  }

  std::vector<KotakSubscription> subscriptions;
  std::size_t begin = 0;
  while (begin <= scrips.size()) {
    const std::size_t end = scrips.find('&', begin);
    const std::string_view entry = std::string_view(scrips).substr(
        begin, end == std::string::npos ? std::string::npos : end - begin);
    const std::size_t bar = entry.find('|');
    if (bar == std::string_view::npos || bar == 0 || bar + 1 == entry.size()) {
      return broker_exec::fail(errors::make_error(
          errors::ErrorCategory::Validation, "kotak: subscription entry is not segment|token"));
    }
    KotakSubscription subscription;
    subscription.exchange_segment = std::string(entry.substr(0, bar));
    subscription.instrument_token = std::string(entry.substr(bar + 1));
    subscriptions.push_back(std::move(subscription));
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return subscriptions;
}

KotakMessageKind classify_message(std::string_view message) {
  const json parsed =
      json::parse(message.begin(), message.end(), nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded()) {
    // Some Kotak deployments send a bare "hb" text keep-alive rather than JSON.
    return message == "hb" ? KotakMessageKind::Heartbeat : KotakMessageKind::Unknown;
  }
  if (parsed.is_object()) {
    return classify_record(parsed);
  }
  if (parsed.is_array() && !parsed.empty() && parsed.front().is_object()) {
    // The feed batches records of ONE kind per frame; the first record decides.
    return classify_record(parsed.front());
  }
  return KotakMessageKind::Unknown;
}

}  // namespace broker_exec::adapters::kotak
