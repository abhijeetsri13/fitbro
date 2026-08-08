#include "broker_exec/adapters/kotak/kotak_capabilities.hpp"

#include "broker_exec/capabilities/capabilities.hpp"

namespace broker_exec::adapters::kotak {

using capabilities::Capability;
using capabilities::CapabilitySet;
using capabilities::Support;

CapabilitySet kotak_capabilities() {
  // EVERY capability is Unknown after Story 6.1, and it is STILL Unknown after
  // Story 6.2. That is the correct answer, and the second half of that sentence
  // is the one that matters.
  //
  // The tempting mistake is to mark place/modify/cancel Supported because this
  // module's fixtures exercise them. But those fixtures are OUR OWN recorded
  // ASSUMPTION of the wire format — we wrote both the question and the answer,
  // and no request has ever reached Kotak. A fixture proves our parser agrees
  // with our fixture; it cannot certify an endpoint we have never contacted.
  // Marking Supported on that evidence is precisely how a strategy dies
  // mid-trade against a path that was wrong all along.
  //
  // STORY 6.2 SHARPENS THAT, IT DOES NOT WEAKEN IT. `KotakBrokerAdapter` now
  // passes the SAME broker-agnostic conformance kit that certifies Kite, with
  // zero duplicate orders across the full fault matrix
  // (tests/conformance/kotak_conformance_test.cpp). That is TIER-1 evidence: the
  // adapter's LOGIC is proven against a recorded endpoint. It says nothing about
  // whether the endpoint, its jData field names, or its auth handshake are real,
  // because RecordedKotakServer is still our own assumption wearing a broker's
  // clothes. Passing conformance therefore flips NOTHING here.
  //
  // Nothing is marked Unsupported either: "Unsupported" is a CERTIFIED ABSENCE,
  // and we have certified nothing. Unknown is the honest state, and the gate
  // reads it as unsupported (fail closed) — so `CapabilitySet::require()` rejects
  // Kotak order flow EARLY, at load/safe-start, rather than mid-trade (AC-2).
  //
  // THE ONE GATE THAT PROMOTES THESE ENTRIES: docs/kotak-min-qty-smoke.md, the
  // operator-run tier-2 live min-qty smoke. Its steps are written to resolve
  // specific Unknowns, and the runbook names which step resolves which:
  //
  //   PlaceOrder / ModifyOrder / CancelOrder
  //                            -> smoke steps 3-5 (a real order on a real path)
  //   SquareOff                -> smoke step 6, AND NOT BEFORE THE FEATURE EXISTS.
  //                               `KotakBrokerAdapter::square_off()` is currently a
  //                               typed NotSupported refusal: it used to issue a
  //                               cancel and return ok, which is FAIL-OPEN against
  //                               a filled position (a cancel flattens nothing, yet
  //                               the caller was told it was flat). Promoting this
  //                               entry requires a real position-flattening market
  //                               exit to be implemented AND smoke-tested — a live
  //                               run alone is not sufficient evidence here.
  //   HeadlessSessionRefresh   -> step 2 (does the Neo session renew
  //                               server-to-server, or must the operator re-MPIN?)
  //   OrderUpdateWebsocket     -> step 7 (a real socket carrying a real order
  //                               update; today only the pure protocol layer exists)
  //   BasketMargin             -> step 8 (the multi-leg margin preview endpoint)
  //   TagCarry                 -> step 3a (does Kotak echo ANY client tag? Until
  //                               that is answered the adapter refuses to send one
  //                               and correlates by order id + attribute
  //                               corroboration instead — see
  //                               kotak_broker_adapter.hpp)
  //
  // Promotion rule for whoever does that run: an entry moves to Supported only
  // on evidence from a real Kotak endpoint, never from a fixture — and the
  // captured payloads are committed as VCR fixtures in the same change.
  return CapabilitySet::builder().build();
}

}  // namespace broker_exec::adapters::kotak
