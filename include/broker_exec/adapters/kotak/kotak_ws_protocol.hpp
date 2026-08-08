#pragma once

// broker_exec::adapters::kotak — the Kotak Neo websocket PROTOCOL layer
// (Story 6.1, planning decision "WebSocket").
//
// **THERE IS NO SOCKET IN THIS FILE, ON PURPOSE.** This story adds NO websocket
// dependency (no IXWebSocket, no new Conan package). What it adds is the pure,
// testable half of a feed client:
//
//   * frame BUILDERS   — the exact subscribe/unsubscribe JSON text to send
//   * a message CLASSIFIER — order-update vs tick vs heartbeat vs unknown
//
// Both are pure functions over strings, so the whole protocol is verified by
// committed fixtures with no network and no live session. Real transport (the
// socket, the reconnect loop, the resubscribe-on-reconnect guard that
// `feedsub` already owns, and the `marketdata` tick-state wiring) is a TIER-2
// FOLLOW-UP tracked for Story 6.2. This mirrors how Story 3.5/IMP-10 landed the
// tick-state machine ahead of its transport.
//
// FAIL CLOSED: `classify_message` returns `Unknown` for anything it does not
// positively recognize. An unrecognized frame must never be silently treated as
// a tick (stale data) or as an order update (phantom fills) — the caller escalates
// or reconciles. THE PUSH IS NEVER AUTHORITATIVE: an `OrderUpdate` classification
// says only "this frame LOOKS like an order update"; the canonical fill still
// comes from a reconciled read (see `fillnorm`).
//
// NO FLOAT: the classifier reads structure/keys only. It never parses a price or
// quantity — decoding those into paise integers is the adapter's job.
//
// Cross-platform: C++20 standard library + nlohmann_json (in the .cpp only). No
// OS APIs, no `#ifdef`.

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "broker_exec/result.hpp"

namespace broker_exec::adapters::kotak {

// One instrument on the Kotak feed. `exchange_segment` is the Kotak segment slug
// ("nse_cm", "nse_fo", "bse_cm", ...) and `instrument_token` the numeric token
// as TEXT (never a number: tokens are opaque identifiers, not arithmetic).
struct KotakSubscription {
  std::string exchange_segment;
  std::string instrument_token;

  friend bool operator==(const KotakSubscription& a, const KotakSubscription& b) {
    return a.exchange_segment == b.exchange_segment && a.instrument_token == b.instrument_token;
  }
};

// What a received frame is. Anything unrecognized is `Unknown` (fail closed).
enum class KotakMessageKind { OrderUpdate, Tick, Heartbeat, Unknown };

// Stable, log-friendly names (observability contract; renames are breaking).
[[nodiscard]] std::string_view to_string(KotakMessageKind kind) noexcept;

// Build the subscribe / unsubscribe frames for a set of instruments. The Kotak
// feed takes the whole set as one `scrips` string of `segment|token` pairs joined
// by '&', which is why the resubscribe guard can re-issue the FULL set in a
// single frame after a reconnect.
//
// An empty subscription list yields an empty string (there is nothing to send) —
// callers must not transmit an empty frame.
[[nodiscard]] std::string build_subscribe_frame(std::span<const KotakSubscription> subscriptions,
                                                std::string_view channel = "1");
[[nodiscard]] std::string build_unsubscribe_frame(std::span<const KotakSubscription> subscriptions,
                                                  std::string_view channel = "1");

// Parse a frame produced by the builders back into its subscription list. This
// exists so the frame format is verified by ROUND TRIP (build -> parse ->
// compare) rather than by asserting on a brittle literal string.
[[nodiscard]] Result<std::vector<KotakSubscription>> parse_subscription_frame(
    std::string_view frame);

// Classify one received frame. Recognizes:
//   OrderUpdate — an order/trade update (`nOrdNo`/`ordSt`/type "order").
//   Tick        — a market-data packet (`tk`+`lp`/`ltp`, type "stk"/"if"/"dp").
//   Heartbeat   — a keep-alive (type "hb", or the bare "hb" text frame).
//   Unknown     — everything else, INCLUDING malformed JSON (fail closed).
// Arrays are classified by their first object element (the feed batches records
// of one kind per frame).
[[nodiscard]] KotakMessageKind classify_message(std::string_view message);

}  // namespace broker_exec::adapters::kotak
