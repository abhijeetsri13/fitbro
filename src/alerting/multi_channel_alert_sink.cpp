#include "broker_exec/alerting/multi_channel_alert_sink.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <utility>

#include "broker_exec/domain/redaction.hpp"
#include "broker_exec/errors/error.hpp"

namespace broker_exec::alerting {

namespace {

using json = nlohmann::json;
using errors::ErrorCategory;
using errors::make_error;

// Stable, log-friendly level names for the webhook body. Anonymous-namespace
// helper: not part of any public contract.
[[nodiscard]] std::string_view level_name(ports::AlertLevel level) noexcept {
  switch (level) {
    case ports::AlertLevel::Info:
      return "Info";
    case ports::AlertLevel::Warning:
      return "Warning";
    case ports::AlertLevel::Error:
      return "Error";
    case ports::AlertLevel::Critical:
      return "Critical";
  }
  return "Info";
}

// Build the per-channel JSON payload for an already-scrubbed message. Telegram
// expects {chat_id, text}; the generic webhook gets {level, message}. dump uses
// the non-throwing handler so an invalid-UTF-8 byte is replaced, never thrown.
[[nodiscard]] std::string build_body(const AlertChannel& channel, ports::AlertLevel level,
                                     const std::string& safe) {
  json body = json::object();
  if (channel.kind == AlertChannel::Kind::Telegram) {
    body["chat_id"] = channel.chat_id;
    body["text"] = safe;
  } else {
    body["level"] = level_name(level);
    body["message"] = safe;
  }
  return body.dump(-1, ' ', false, json::error_handler_t::replace);
}

// Invoke the injected seam, swallowing a throwing seam into a per-channel
// failure: nothing throws across the AlertSink boundary.
[[nodiscard]] bool post_ok(const PostFn& post, std::string_view url, std::string_view body) {
  try {
    auto result = post(url, body);
    return static_cast<bool>(result);
  } catch (...) {
    return false;
  }
}

}  // namespace

MultiChannelAlertSink::MultiChannelAlertSink(PostFn post, std::vector<AlertChannel> channels,
                                             const ports::ClockPort& clock)
    : post_(std::move(post)), channels_(std::move(channels)), heartbeat_(clock) {}

Result<ports::Ok> MultiChannelAlertSink::send(ports::AlertLevel level,
                                              const std::string& message) {
  // No channel configured == cannot alert at all. Surface it as an Internal
  // wiring error rather than a silent success.
  if (channels_.empty()) {
    return fail(make_error(ErrorCategory::Internal, "alert sink has no channels configured"));
  }

  // SCRUB FIRST: the in-memory message is not pre-scrubbed, so redact before any
  // body is built — no token-shaped run reaches any outbound payload (SEC-3).
  const std::string safe = domain::scrub(message);

  int delivered = 0;
  for (const AlertChannel& channel : channels_) {
    const std::string body = build_body(channel, level, safe);
    if (post_ok(post_, channel.url, body)) {
      ++delivered;
    }
  }

  if (delivered > 0) {
    // A delivered alert is itself a sign of life — beat the dead-man's-switch.
    heartbeat_.beat();
    return ports::ok();
  }

  // Every channel failed: the alert could not reach the operator. The absence of
  // heartbeats backstops this for the external watcher.
  return fail(make_error(ErrorCategory::Network, "alert delivery failed on all channels"));
}

Result<ports::Ok> MultiChannelAlertSink::send_test_alert() {
  if (channels_.empty()) {
    return fail(make_error(ErrorCategory::Internal, "alert sink has no channels configured"));
  }

  // The fixed self-test body still scrubs itself (defensive: cheap, and keeps the
  // "every outbound body is scrubbed" invariant exception-free).
  const std::string safe = domain::scrub("broker-exec alert channel test");

  int failed = 0;
  for (const AlertChannel& channel : channels_) {
    const std::string body = build_body(channel, ports::AlertLevel::Info, safe);
    if (!post_ok(post_, channel.url, body)) {
      ++failed;
    }
  }

  if (failed == 0) {
    return ports::ok();
  }
  // A test must verify EACH channel: any failure is a failed test.
  return fail(make_error(ErrorCategory::Network,
                         "test alert failed on " + std::to_string(failed) + " of " +
                             std::to_string(channels_.size()) + " channels"));
}

void MultiChannelAlertSink::heartbeat() {
  // Liveness is the beat, recorded unconditionally on the steady clock. The
  // accompanying Info alert is best-effort; its result is intentionally ignored
  // (a down channel must not suppress the authoritative sign of life).
  heartbeat_.beat();
  static_cast<void>(send(ports::AlertLevel::Info, "heartbeat"));
}

}  // namespace broker_exec::alerting
