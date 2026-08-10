#include "broker_exec/observability/audit_trail.hpp"

#include <string>
#include <string_view>
#include <vector>

#include "broker_exec/observability/structured_logger.hpp"

namespace broker_exec::observability {

void AuditTrail::record(const AuditEvent& ev) {
  // operator[] default-constructs an empty vector on first sight of a ref; the
  // push_back preserves insertion order, which IS the decision-path order.
  by_ref_[ev.client_ref].push_back(ev);
}

void AuditTrail::record_and_log(const AuditEvent& ev, StructuredLogger& logger) {
  record(ev);
  logger.log(ev);
}

std::vector<AuditEvent> AuditTrail::decision_path(std::string_view client_ref) const {
  const auto it = by_ref_.find(std::string(client_ref));
  if (it == by_ref_.end()) {
    return {};
  }
  return it->second;
}

}  // namespace broker_exec::observability
