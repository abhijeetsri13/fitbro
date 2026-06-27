#include "broker_exec/runtime/unknown_pause.hpp"

#include <string>
#include <utility>

namespace broker_exec::runtime {

void UnknownPause::mark_unknown(std::string client_ref) {
  // insert() is the idempotency: a second mark of the same ref is absorbed by the
  // set and does not change outstanding().
  outstanding_.insert(std::move(client_ref));
}

void UnknownPause::clear(std::string_view client_ref) {
  // erase() by transparent lookup is unavailable without a heterogeneous hash, so
  // materialize the key. A miss erases nothing (clearing an unknown ref is a
  // no-op, matching the contract).
  outstanding_.erase(std::string(client_ref));
}

bool UnknownPause::is_paused() const noexcept { return !outstanding_.empty(); }

std::size_t UnknownPause::outstanding() const noexcept { return outstanding_.size(); }

bool UnknownPause::allows(bool is_risk_reducing_exit) const noexcept {
  // A risk-reducing exit is NEVER blocked: trapping a live position behind an
  // UNKNOWN pause is the unsafe outcome. Anything else is blocked while paused.
  if (is_risk_reducing_exit) {
    return true;
  }
  return !is_paused();
}

}  // namespace broker_exec::runtime
