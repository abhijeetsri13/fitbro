// Kotak Neo websocket PROTOCOL tests + the capability profile (Story 6.1).
//
// There is no socket here by design (see kotak_ws_protocol.hpp): the frame
// builders and the classifier are pure functions, so the whole protocol is
// verified by committed fixtures. Real transport is the tier-2 follow-up.

#include "broker_exec/adapters/kotak/kotak_ws_protocol.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>
#include <vector>

#include "broker_exec/adapters/kotak/kotak_capabilities.hpp"
#include "broker_exec/capabilities/capabilities.hpp"

using broker_exec::adapters::kotak::build_subscribe_frame;
using broker_exec::adapters::kotak::build_unsubscribe_frame;
using broker_exec::adapters::kotak::classify_message;
using broker_exec::adapters::kotak::kotak_capabilities;
using broker_exec::adapters::kotak::KotakMessageKind;
using broker_exec::adapters::kotak::KotakSubscription;
using broker_exec::adapters::kotak::parse_subscription_frame;
using broker_exec::capabilities::Capability;
using broker_exec::capabilities::Support;

namespace {

// ── Committed Kotak feed fixtures ────────────────────────────────────────────

// An order/trade update pushed on the order feed.
constexpr const char* kOrderUpdateFrame =
    R"([{"type":"order","nOrdNo":"220101000000001","ordSt":"complete","fldQty":"1","trdSym":"INFY-EQ"}])";

// A quote packet on the market feed (tk = token, lp = last price as TEXT).
constexpr const char* kTickFrame =
    R"([{"type":"stk","tk":"11536","e":"nse_cm","lp":"1450.25","ltt":"01/01/2022 09:30:00"}])";

// An order update that ALSO carries a price-like field — it must never be read
// as a tick.
constexpr const char* kOrderUpdateWithPrice =
    R"({"nOrdNo":"220101000000001","ordSt":"complete","prc":"1450.25","tk":"11536","lp":"1450.25"})";

constexpr const char* kHeartbeatFrame = R"({"type":"hb"})";

constexpr const char* kUnknownFrame = R"({"type":"cm","msg":"channel switched"})";

[[nodiscard]] std::vector<KotakSubscription> sample_subscriptions() {
  return {KotakSubscription{"nse_cm", "11536"}, KotakSubscription{"nse_cm", "1594"},
          KotakSubscription{"nse_fo", "45213"}};
}

}  // namespace

TEST_CASE("a subscribe frame round-trips through build -> parse", "[kotak][ws]") {
  const auto subscriptions = sample_subscriptions();
  const std::string frame = build_subscribe_frame(subscriptions);

  REQUIRE_FALSE(frame.empty());
  // The whole set travels in ONE frame — this is what lets a resubscribe guard
  // re-issue the full subscription set after a reconnect.
  CHECK(frame.find("nse_cm|11536&nse_cm|1594&nse_fo|45213") != std::string::npos);
  CHECK(frame.find(R"("task":"mws")") != std::string::npos);

  const auto parsed = parse_subscription_frame(frame);
  REQUIRE(parsed.has_value());
  CHECK(parsed.value() == subscriptions);
}

TEST_CASE("an unsubscribe frame round-trips and carries the other task", "[kotak][ws]") {
  const auto subscriptions = sample_subscriptions();
  const std::string frame = build_unsubscribe_frame(subscriptions);

  CHECK(frame.find(R"("task":"mwu")") != std::string::npos);
  const auto parsed = parse_subscription_frame(frame);
  REQUIRE(parsed.has_value());
  CHECK(parsed.value() == subscriptions);
}

TEST_CASE("a single-instrument frame round-trips", "[kotak][ws]") {
  const std::vector<KotakSubscription> one{KotakSubscription{"nse_cm", "11536"}};
  const auto parsed = parse_subscription_frame(build_subscribe_frame(one));
  REQUIRE(parsed.has_value());
  REQUIRE(parsed.value().size() == 1);
  CHECK(parsed.value().front().exchange_segment == "nse_cm");
  CHECK(parsed.value().front().instrument_token == "11536");
}

TEST_CASE("the channel is selectable and preserved", "[kotak][ws]") {
  const auto subscriptions = sample_subscriptions();
  const std::string frame = build_subscribe_frame(subscriptions, "2");
  CHECK(frame.find(R"("channel":"2")") != std::string::npos);
}

TEST_CASE("an empty subscription set produces no frame to send", "[kotak][ws]") {
  const std::vector<KotakSubscription> none;
  CHECK(build_subscribe_frame(none).empty());
  CHECK(build_unsubscribe_frame(none).empty());
}

TEST_CASE("a malformed subscription frame fails closed, never half-parsed", "[kotak][ws]") {
  CHECK_FALSE(parse_subscription_frame("not json {{{").has_value());
  CHECK_FALSE(parse_subscription_frame(R"({"task":"mws"})").has_value());
  CHECK_FALSE(parse_subscription_frame(R"({"task":"nope","scrips":"nse_cm|1"})").has_value());
  // A scrip entry that is not `segment|token` must not yield a partial list.
  CHECK_FALSE(parse_subscription_frame(R"({"task":"mws","scrips":"nse_cm|1&broken"})").has_value());
  CHECK_FALSE(parse_subscription_frame(R"({"task":"mws","scrips":"nse_cm|1&"})").has_value());
}

TEST_CASE("messages classify into the four kinds", "[kotak][ws]") {
  CHECK(classify_message(kOrderUpdateFrame) == KotakMessageKind::OrderUpdate);
  CHECK(classify_message(kTickFrame) == KotakMessageKind::Tick);
  CHECK(classify_message(kHeartbeatFrame) == KotakMessageKind::Heartbeat);
  CHECK(classify_message(kUnknownFrame) == KotakMessageKind::Unknown);
  // Some deployments send a bare text keep-alive.
  CHECK(classify_message("hb") == KotakMessageKind::Heartbeat);
}

TEST_CASE("an order update carrying a price is NOT mistaken for a tick", "[kotak][ws]") {
  CHECK(classify_message(kOrderUpdateWithPrice) == KotakMessageKind::OrderUpdate);
}

TEST_CASE("an unrecognized or malformed frame fails CLOSED to Unknown", "[kotak][ws]") {
  // Never silently a tick (stale data) and never silently an order update
  // (phantom fill) — the caller escalates or reconciles.
  CHECK(classify_message("not json {{{") == KotakMessageKind::Unknown);
  CHECK(classify_message("") == KotakMessageKind::Unknown);
  CHECK(classify_message("[]") == KotakMessageKind::Unknown);
  CHECK(classify_message("123") == KotakMessageKind::Unknown);
  CHECK(classify_message(R"({})") == KotakMessageKind::Unknown);
}

TEST_CASE("message-kind names are stable", "[kotak][ws]") {
  CHECK(to_string(KotakMessageKind::OrderUpdate) == "order_update");
  CHECK(to_string(KotakMessageKind::Tick) == "tick");
  CHECK(to_string(KotakMessageKind::Heartbeat) == "heartbeat");
  CHECK(to_string(KotakMessageKind::Unknown) == "unknown");
}

TEST_CASE("the Kotak profile claims NOTHING — a fixture cannot certify an endpoint",
          "[kotak][capabilities]") {
  const auto caps = kotak_capabilities();

  // Including the order lifecycle. The fixtures in this module are our OWN
  // recorded assumption of the wire format — we wrote both the question and the
  // answer, and no request has ever reached Kotak. Promotion to Supported
  // requires evidence from a live endpoint (tier-2), never from a fixture.
  CHECK(caps.support_of(Capability::PlaceOrder) == Support::Unknown);
  CHECK(caps.support_of(Capability::ModifyOrder) == Support::Unknown);
  CHECK(caps.support_of(Capability::CancelOrder) == Support::Unknown);

  CHECK(caps.support_of(Capability::HeadlessSessionRefresh) == Support::Unknown);
  CHECK(caps.support_of(Capability::OrderUpdateWebsocket) == Support::Unknown);
  CHECK(caps.support_of(Capability::SquareOff) == Support::Unknown);
  CHECK(caps.support_of(Capability::BasketMargin) == Support::Unknown);
  CHECK(caps.support_of(Capability::GttOrders) == Support::Unknown);
  CHECK(caps.support_of(Capability::MarginShockSim) == Support::Unknown);

  // And nothing is claimed ABSENT either: Unsupported is a certified absence.
  for (std::size_t i = 0; i < broker_exec::capabilities::kCapabilityCount; ++i) {
    const auto capability = static_cast<Capability>(i);
    INFO("capability index " << i);
    CHECK(caps.support_of(capability) == Support::Unknown);
  }
}

TEST_CASE("an Unknown capability is REJECTED at the gate, not silently allowed",
          "[kotak][capabilities]") {
  const auto caps = kotak_capabilities();

  // Fail closed: until a live run certifies them, the gate blocks Kotak order
  // flow outright rather than letting a strategy discover the gap mid-trade.
  CHECK_FALSE(caps.supports(Capability::PlaceOrder));
  CHECK_FALSE(caps.supports(Capability::HeadlessSessionRefresh));
  CHECK_FALSE(caps.supports(Capability::OrderUpdateWebsocket));

  const auto rejected = caps.require(Capability::PlaceOrder);
  REQUIRE_FALSE(rejected.has_value());
  CHECK(rejected.error().category == broker_exec::errors::ErrorCategory::NotSupported);
  CHECK(rejected.error().action == broker_exec::errors::SuggestedAction::DoNotRetry);
}
