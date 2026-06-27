#pragma once

// broker_exec::observability::AuditTrail — in-memory provenance reconstruction
// (Story 4.1, AC-2, FR-27).
//
// Records every AuditEvent per `client_ref` IN ORDER, so an executed order's
// full decision path (placed -> risk -> ack -> (partial) fill / reject /
// unknown / reconcile) can be replayed. This is the in-memory + logged
// provenance VIEW; the DURABLE audit store is the ledger/intent-log (Story 4.4).
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "broker_exec/observability/audit_event.hpp"

namespace broker_exec::observability {

class StructuredLogger;

class AuditTrail {
 public:
  // Append `ev` to its client_ref's path, preserving call order.
  void record(const AuditEvent& ev);

  // Record AND emit through the logger in one step (convenience).
  void record_and_log(const AuditEvent& ev, StructuredLogger& logger);

  // Every event recorded for `client_ref`, IN ORDER. Empty if none seen.
  [[nodiscard]] std::vector<AuditEvent> decision_path(std::string_view client_ref) const;

 private:
  std::unordered_map<std::string, std::vector<AuditEvent>> by_ref_;
};

}  // namespace broker_exec::observability
