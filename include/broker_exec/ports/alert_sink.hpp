#pragma once

// broker_exec::ports::AlertSink — the abstract operator-notification seam.
//
// The dead-man's-switch / escalation path (the `RaiseAlert` SuggestedAction)
// surfaces here. The core emits a level + message; a concrete sink (Telegram,
// email, webhook, ...) delivers it. `send_test_alert()` exists so startup/health
// checks can prove the channel works end-to-end before relying on it. Abstract
// only this story; concrete sinks land later.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <string>

#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::ports {

// Severity of an operator alert. Stable, log-friendly ordering low->high.
enum class AlertLevel { Info, Warning, Error, Critical };

// Abstract alert delivery channel. Header-only, pure-virtual.
class AlertSink {
 public:
  virtual ~AlertSink() = default;

  // Deliver an alert. `message` MUST be redaction-safe (no secrets/tokens),
  // consistent with the Error logging policy.
  [[nodiscard]] virtual Result<Ok> send(AlertLevel level, const std::string& message) = 0;

  // Send a fixed self-test alert to prove the channel is wired and reachable.
  // Used by the startup health check and the operator "test alert" command.
  [[nodiscard]] virtual Result<Ok> send_test_alert() = 0;
};

}  // namespace broker_exec::ports
