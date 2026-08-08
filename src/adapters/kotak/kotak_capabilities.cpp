#include "broker_exec/adapters/kotak/kotak_capabilities.hpp"

#include "broker_exec/capabilities/capabilities.hpp"

namespace broker_exec::adapters::kotak {

using capabilities::Capability;
using capabilities::CapabilitySet;
using capabilities::Support;

CapabilitySet kotak_capabilities() {
  // EVERY capability is Unknown after Story 6.1, and that is the correct answer.
  //
  // The tempting mistake is to mark place/modify/cancel Supported because this
  // module's fixtures exercise them. But those fixtures are OUR OWN recorded
  // ASSUMPTION of the wire format — we wrote both the question and the answer,
  // and no request has ever reached Kotak. A fixture proves our parser agrees
  // with our fixture; it cannot certify an endpoint we have never contacted.
  // Marking Supported on that evidence is precisely how a strategy dies
  // mid-trade against a path that was wrong all along.
  //
  // Nothing is marked Unsupported either: "Unsupported" is a CERTIFIED ABSENCE,
  // and we have certified nothing. Unknown is the honest state, and the gate
  // reads it as unsupported (fail closed), so the adapter cannot be used for
  // live order flow until a tier-2 LIVE verification promotes these entries.
  //
  // Promotion rule for whoever does that run: an entry moves to Supported only
  // on evidence from a real Kotak endpoint, never from a fixture.
  return CapabilitySet::builder().build();
}

}  // namespace broker_exec::adapters::kotak
