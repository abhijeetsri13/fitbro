#pragma once

// broker_exec::alerting::MultiChannelAlertSink — a best-effort, multi-channel
// AlertSink that cannot fail silently (Story 4.3, FR-28, CC-6, ID-3, SEC-3).
//
// Implements ports::AlertSink over a list of AlertChannels delivered through an
// injected PostFn seam (no network in this module). Two guarantees:
//   * Every outbound body scrubs ITSELF via domain::scrub (the Story-4.2 lesson)
//     — the in-memory message is NOT pre-scrubbed, so no token can leak into any
//     channel payload.
//   * The alerter holds a HeartbeatMonitor on the injected steady clock and
//     beat()s it on every successful delivery; an external watcher polls
//     is_alive() so a killed alerter trips the absence alarm (the dead-man's-
//     switch backstop for "every channel is down").
//
// Delivery is best-effort: send() succeeds if AT LEAST ONE channel delivered;
// send_test_alert() requires ALL channels (a test must verify EACH channel).
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`, no float.
// Never throws across the AlertSink boundary (a throwing seam is swallowed into
// a per-channel failure).

#include <chrono>
#include <string>
#include <vector>

#include "broker_exec/alerting/alert_channel.hpp"
#include "broker_exec/alerting/heartbeat_monitor.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/ports/clock_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::alerting {

class MultiChannelAlertSink final : public ports::AlertSink {
 public:
  MultiChannelAlertSink(PostFn post, std::vector<AlertChannel> channels,
                        const ports::ClockPort& clock);

  // Deliver `message` to every configured channel (each body scrubbed first).
  // ok() iff >=1 channel delivered; Error if all fail or no channel configured.
  // beat()s the heartbeat on any success. Never throws.
  [[nodiscard]] Result<ports::Ok> send(ports::AlertLevel level,
                                       const std::string& message) override;

  // Same delivery, plus TYPED PROVENANCE (IMP-16). `message` is scrubbed by the
  // IDENTICAL domain::scrub call as send() — nothing about free-form redaction is
  // relaxed — and `provenance` is rendered SEPARATELY through the whole-column
  // allowlist and APPENDED as ` [client_ref=... broker_order_id=... strategy=...]`,
  // omitting empty fields. A context field that is not id-shaped is redacted; an
  // all-empty context appends nothing, leaving the body byte-identical to send().
  // Never throws.
  [[nodiscard]] Result<ports::Ok> send_with_context(ports::AlertLevel level,
                                                    const std::string& message,
                                                    const ports::AlertContext& provenance) override;

  // Send a fixed self-test message to EVERY channel. ok() iff ALL channels
  // accepted (the test must prove each channel is wired). Never throws.
  [[nodiscard]] Result<ports::Ok> send_test_alert() override;

  // Emit a periodic heartbeat: always beat() the monitor (the authoritative
  // sign of life the external watcher polls), then best-effort send an Info
  // "heartbeat" alert. The send result is intentionally ignored — liveness is
  // the beat, not the (possibly-down) channel.
  void heartbeat();

  // Liveness view for the external watcher / tests. The watcher polls this (on
  // its own clock); a killed alerter stops beating and is_alive flips false
  // after max_gap — the absence alarm (AC-3).
  [[nodiscard]] const HeartbeatMonitor& heartbeat_monitor() const { return heartbeat_; }
  [[nodiscard]] bool is_alive(std::chrono::milliseconds max_gap) const {
    return heartbeat_.is_alive(max_gap);
  }

 private:
  // Fan `text` (ALREADY scrubbed and already carrying any provenance block) out
  // to every channel. The single delivery/heartbeat path that send() and
  // send_with_context() share, so they cannot drift in best-effort semantics.
  [[nodiscard]] Result<ports::Ok> deliver(ports::AlertLevel level, const std::string& text);

  PostFn post_;
  std::vector<AlertChannel> channels_;
  HeartbeatMonitor heartbeat_;
};

}  // namespace broker_exec::alerting
