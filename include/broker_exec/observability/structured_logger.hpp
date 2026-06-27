#pragma once

// broker_exec::observability::StructuredLogger — the redaction-bound JSON log
// sink for audit events (Story 4.1, AC-1, SEC-3).
//
// THE REDACTION BINDING: every rendered line is passed through `domain::scrub`
// before it reaches spdlog, so no token-shaped string can survive in ANY field
// (typed column or `fields`) — this is the load-bearing security property.
//
// SHAPE-BOUNDED: domain::scrub catches >=20-char alnum token runs and sensitive
// key=value pairs; real Kite access_tokens are 32-char alnum so they are covered,
// but an arbitrary short / all-letter secret encoding is NOT a guarantee.
// Provenance IDs are non-secret and intended to survive scrubbing.
//
// spdlog is an IMPLEMENTATION DETAIL confined to structured_logger.cpp: this
// header only forward-declares `spdlog::logger` / `spdlog::sinks::sink` (held by
// shared_ptr), so no consumer of the observability public API takes a spdlog
// include. The logger is injected (built from a caller-supplied sink) so tests
// can capture lines without a file.
//
// NO-THROW: log() swallows any sink exception — logging must never derail the
// engine (no throw across the boundary, per the errors policy).

#include <memory>
#include <string>

#include "broker_exec/observability/audit_event.hpp"
#include "broker_exec/ports/clock_port.hpp"

// Forward declarations keep spdlog out of this public header.
namespace spdlog {
class logger;
namespace sinks {
class sink;
}  // namespace sinks
}  // namespace spdlog

namespace broker_exec::observability {

class StructuredLogger {
 public:
  // `logger` is a configured spdlog logger (build one with make_logger() so its
  // pattern is the raw "%v" that emits our JSON verbatim). `clock` is the
  // injected wall-time source used to stamp events with an empty `ts`.
  StructuredLogger(std::shared_ptr<spdlog::logger> logger, const ports::ClockPort& clock);

  // Render `ev` to its JSON line, scrub it (the redaction binding), and emit at
  // a level derived from the event type (Error -> error, Override -> warn, else
  // info). Stamps an empty `ts` from the clock first. Never throws.
  void log(AuditEvent ev);

  // Build a logger over `sink` whose pattern is raw "%v" — spdlog must NOT wrap
  // our JSON (no timestamp/level prefix), because the JSON IS the line.
  [[nodiscard]] static std::shared_ptr<spdlog::logger> make_logger(
      std::shared_ptr<spdlog::sinks::sink> sink, std::string name = "audit");

 private:
  std::shared_ptr<spdlog::logger> logger_;
  const ports::ClockPort& clock_;
};

}  // namespace broker_exec::observability
