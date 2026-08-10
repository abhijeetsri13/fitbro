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

Result<ports::Ok> MultiChannelAlertSink::deliver(ports::AlertLevel level,
                                                 const std::string& text) {
  // No channel configured == cannot alert at all. Surface it as an Internal
  // wiring error rather than a silent success.
  if (channels_.empty()) {
    return fail(make_error(ErrorCategory::Internal, "alert sink has no channels configured"));
  }

  int delivered = 0;
  for (const AlertChannel& channel : channels_) {
    const std::string body = build_body(channel, level, text);
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

Result<ports::Ok> MultiChannelAlertSink::send(ports::AlertLevel level,
                                              const std::string& message) {
  // SCRUB FIRST: the in-memory message is not pre-scrubbed, so redact before any
  // body is built — no token-shaped run reaches any outbound payload (SEC-3).
  return deliver(level, domain::scrub(message));
}

Result<ports::Ok> MultiChannelAlertSink::send_with_context(
    ports::AlertLevel level, const std::string& message,
    const ports::AlertContext& provenance) {
  // THE FREE-FORM BODY IS SCRUBBED IDENTICALLY to the send() path — same call,
  // same argument, no exemption, no relaxation. That is the whole point of
  // IMP-16: the ids do NOT ride inside `message`, so `message` never needs (and
  // never gets) a substring allowlist that would blunt the bare high-entropy rule.
  const std::string safe = domain::scrub(message);

  // The typed columns are rendered SEPARATELY, each through the whole-column
  // allowlist, and appended AFTER the scrub. Empty fields are omitted; an
  // all-empty context renders "" and leaves `safe` byte-identical to the send()
  // path. A column holding something that is not id-shaped (a token, a
  // URL, a blob) is redacted by domain::render_provenance_block — fail closed.
  //
  // `symbol` is rendered LAST and through the SYMBOL shape rule, not the id rule:
  // an instrument is one unbroken heterogeneous run, so the id rule would reject
  // it and the scrub fallback would destroy every >=20-char option symbol. Last
  // position also keeps the field order of every pre-existing block unchanged.
  const std::string block = domain::render_provenance_block({
      {"client_ref", provenance.client_ref},
      {"broker_order_id", provenance.broker_order_id},
      {"strategy", provenance.strategy},
      {"symbol", provenance.symbol, domain::ProvenanceField::Kind::Symbol},
  });
  return deliver(level, safe + block);
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
