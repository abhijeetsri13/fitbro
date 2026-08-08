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
// AND EVERYTHING IS STILL UNKNOWN AFTER STORY 6.2. The Kotak adapter now passes
// the same conformance kit that certifies Kite, with zero duplicates across the
// full fault matrix — but that is TIER-1 (recorded-fixture) certification of the
// adapter's LOGIC, not of the broker's wire contract. Fixture certification
// deliberately flips NOTHING here.
//
// THE TIER-2 FLIP GATE IS `docs/kotak-min-qty-smoke.md` — the operator-run live
// min-qty smoke. Its steps are mapped one-to-one onto the Unknowns they resolve
// (see the promotion table in kotak_capabilities.cpp).
//
// Consequences, all intended: the capability gate rejects Kotak order flow until
// that live verification promotes these entries; HeadlessSessionRefresh stays
// Unknown so the runtime blocks and the operator re-establishes rather than
// silently auto-refreshing; OrderUpdateWebsocket stays Unknown because the pure
// protocol layer exists but no socket transport does; TagCarry stays Unknown,
// which is why the adapter refuses to send a speculative client tag and
// correlates by order id plus attribute corroboration instead.
//
// Nothing is marked Unsupported either — that is a CERTIFIED ABSENCE, and we have
// certified nothing.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include "broker_exec/capabilities/capabilities.hpp"

namespace broker_exec::adapters::kotak {

[[nodiscard]] capabilities::CapabilitySet kotak_capabilities();

}  // namespace broker_exec::adapters::kotak
