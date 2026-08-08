#pragma once

// broker_exec::adapters::kotak::kotak_capabilities — the Kotak Neo capability
// profile (Story 6.1, Story 2.5's model, FR-2, CAP-2).
//
// It lives in the ADAPTER, not in `capabilities`, because the truth it encodes
// is broker-specific evidence: what THIS adapter has actually been shown to do.
// The `capabilities` module stays the broker-neutral value type.
//
// THE RULE THAT MATTERS: `Support::Unknown` is the zero value and the gate reads
// Unknown as UNSUPPORTED. An entry may be promoted to `Supported` ONLY on
// evidence from a real Kotak endpoint.
//
// SO EVERYTHING IS UNKNOWN AFTER STORY 6.1 — including place/modify/cancel.
// A committed fixture is NOT evidence: we authored both the request and the
// response, so it proves our parser agrees with our own assumption, not that the
// endpoint exists or behaves that way. Marking a capability Supported on
// self-authored evidence is exactly how a strategy dies mid-trade.
//
// Consequences, all intended: the capability gate rejects Kotak order flow until
// a tier-2 LIVE verification promotes these entries; HeadlessSessionRefresh stays
// Unknown so the runtime blocks and the operator re-establishes rather than
// silently auto-refreshing; OrderUpdateWebsocket stays Unknown because the
// protocol layer exists but no socket transport does until Story 6.2.
//
// Nothing is marked Unsupported either — that is a CERTIFIED ABSENCE, and we have
// certified nothing.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include "broker_exec/capabilities/capabilities.hpp"

namespace broker_exec::adapters::kotak {

[[nodiscard]] capabilities::CapabilitySet kotak_capabilities();

}  // namespace broker_exec::adapters::kotak
