#include "broker_exec/capabilities/capabilities.hpp"

#include <string>

#include "broker_exec/errors/error.hpp"

namespace broker_exec::capabilities {

std::string_view to_string(Capability capability) noexcept {
  switch (capability) {
    case Capability::PlaceOrder:
      return "place_order";
    case Capability::ModifyOrder:
      return "modify_order";
    case Capability::CancelOrder:
      return "cancel_order";
    case Capability::SquareOff:
      return "square_off";
    case Capability::BasketMargin:
      return "basket_margin";
    case Capability::OrderUpdateWebsocket:
      return "order_update_websocket";
    case Capability::HeadlessSessionRefresh:
      return "headless_session_refresh";
    case Capability::GttOrders:
      return "gtt_orders";
    case Capability::AmoOrders:
      return "amo_orders";
    case Capability::CoverOrder:
      return "cover_order";
    case Capability::BracketOrder:
      return "bracket_order";
    case Capability::TagCarry:
      return "tag_carry";
    case Capability::MarginShockSim:
      return "margin_shock_sim";
  }
  return "unknown_capability";
}

std::string_view to_string(Support support) noexcept {
  switch (support) {
    case Support::Supported:
      return "supported";
    case Support::Unsupported:
      return "unsupported";
    case Support::Unknown:
      return "unknown";
  }
  return "unknown";
}

namespace {

// Build the typed rejection for an unsupported (or unknown) capability. The
// message names the capability and states the reject-only posture. NotSupported
// already defaults to SuggestedAction::DoNotRetry via default_action_for(); we
// also set it explicitly so the contract holds regardless of that default.
[[nodiscard]] errors::Error not_supported_error(Capability capability) {
  std::string message = "capability '";
  message += to_string(capability);
  message += "' is not supported by this broker (reject-only; no substitution)";
  errors::Error error = errors::make_error(errors::ErrorCategory::NotSupported, std::move(message));
  error.action = errors::SuggestedAction::DoNotRetry;
  return error;
}

}  // namespace

Result<ports::Ok> CapabilitySet::require(Capability capability) const {
  if (supports(capability)) {
    return ports::ok();
  }
  return fail(not_supported_error(capability));
}

Result<ports::Ok> CapabilitySet::require_all(std::span<const Capability> capabilities) const {
  for (const Capability capability : capabilities) {
    if (!supports(capability)) {
      return fail(not_supported_error(capability));
    }
  }
  return ports::ok();
}

CapabilitySet kite_capabilities() {
  // See header: only the certified order-lifecycle capabilities are Supported;
  // HeadlessSessionRefresh is a certified Unsupported (Story 2.4). All other
  // capabilities are intentionally left at their default Unknown so they read as
  // unsupported until certified in Story 2.14.
  return CapabilitySet::builder()
      .set(Capability::PlaceOrder, Support::Supported)
      .set(Capability::ModifyOrder, Support::Supported)
      .set(Capability::CancelOrder, Support::Supported)
      .set(Capability::SquareOff, Support::Supported)
      .set(Capability::HeadlessSessionRefresh, Support::Unsupported)
      .build();
}

}  // namespace broker_exec::capabilities
