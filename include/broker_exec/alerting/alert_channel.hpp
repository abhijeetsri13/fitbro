#pragma once

// broker_exec::alerting::AlertChannel — one operator-notification destination,
// plus the injected POST seam the sink delivers through (Story 4.3, FR-28).
//
// A channel is pure configuration: which transport (Telegram bot / generic
// webhook), the endpoint url, and (Telegram only) the chat_id. Delivery itself
// goes through `PostFn`, a `std::function` seam: tests inject a capturing
// recorder so CI needs NO Telegram/webhook network; production wires a thin cpr
// POST adapter behind the same seam (follow-up, mirroring the kite HttpClient
// seam). Keeping the network out of this module preserves the inward-only
// dependency rule (ports/errors/domain + nlohmann, no adapters/SDKs).
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <functional>
#include <string>
#include <string_view>

#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::alerting {

// A single delivery destination. `chat_id` is used only for Telegram bodies.
struct AlertChannel {
  enum class Kind { Telegram, Webhook };

  Kind kind = Kind::Webhook;
  std::string url;
  std::string chat_id;
};

// The injected delivery seam: POST `body` to `url`, returning ok()/Error. The
// implementation never throws across this boundary — a throwing seam is caught
// and treated as a per-channel failure (see MultiChannelAlertSink::send).
using PostFn = std::function<Result<ports::Ok>(std::string_view url, std::string_view body)>;

}  // namespace broker_exec::alerting
