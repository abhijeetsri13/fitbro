#include "broker_exec/observability/structured_logger.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include <spdlog/logger.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/spdlog.h>

#include "broker_exec/domain/redaction.hpp"
#include "broker_exec/observability/audit_event.hpp"

namespace broker_exec::observability {

StructuredLogger::StructuredLogger(std::shared_ptr<spdlog::logger> logger,
                                   const ports::ClockPort& clock)
    : logger_(std::move(logger)), clock_(clock) {}

void StructuredLogger::log(AuditEvent ev) {
  // Stamp an unset timestamp from the injected clock (never read the wall clock
  // directly — FR-23). Empty == the system_clock epoch default.
  if (ev.ts == std::chrono::system_clock::time_point{}) {
    ev.ts = clock_.now_wall();
  }

  // The WHOLE render + scrub + emit is contained here: rendering (to_json_line)
  // or scrubbing (domain::scrub) could throw std::bad_alloc, and a misbehaving
  // sink (full disk, broken pipe) could throw on emit — so swallow everything,
  // logging is best-effort and nothing throws across the boundary.
  try {
    // THE redaction binding: scrub the WHOLE rendered line, so a token-shaped
    // run in ANY field (typed column or `fields`) is replaced before it reaches
    // a sink. No secret in any observable sink (SEC-3).
    const std::string line = domain::scrub(to_json_line(ev));

    if (logger_ == nullptr) {
      return;
    }
    switch (ev.type) {
      case EventType::Error:
        logger_->error(line);
        break;
      case EventType::Override:
        logger_->warn(line);
        break;
      default:
        logger_->info(line);
        break;
    }
  } catch (...) {  // NOLINT(bugprone-empty-catch) — logging is best-effort.
    // Deliberately swallowed: a logging failure must not propagate.
  }
}

std::shared_ptr<spdlog::logger> StructuredLogger::make_logger(
    std::shared_ptr<spdlog::sinks::sink> sink, std::string name) {
  auto logger = std::make_shared<spdlog::logger>(std::move(name), std::move(sink));
  // Raw passthrough: we emit our own JSON, so spdlog must NOT prepend a
  // timestamp/level/logger-name pattern. "%v" is just the message verbatim.
  logger->set_pattern("%v");
  return logger;
}

}  // namespace broker_exec::observability
