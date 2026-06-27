#include "broker_exec/domain/enums.hpp"

namespace broker_exec::domain {

std::string_view to_string(OrderState state) noexcept {
  switch (state) {
    case OrderState::Created:
      return "CREATED";
    case OrderState::Validated:
      return "VALIDATED";
    case OrderState::PendingSend:
      return "PENDING_SEND";
    case OrderState::Sent:
      return "SENT";
    case OrderState::Acknowledged:
      return "ACKNOWLEDGED";
    case OrderState::PartiallyFilled:
      return "PARTIALLY_FILLED";
    case OrderState::Filled:
      return "FILLED";
    case OrderState::Rejected:
      return "REJECTED";
    case OrderState::Cancelled:
      return "CANCELLED";
    case OrderState::Unknown:
      return "UNKNOWN";
    case OrderState::Reconciled:
      return "RECONCILED";
    case OrderState::PartiallyPlaced:
      return "PARTIALLY_PLACED";
    case OrderState::ManualInterventionRequired:
      return "MANUAL_INTERVENTION_REQUIRED";
  }
  return "UNKNOWN";
}

std::string_view to_string(Side side) noexcept {
  switch (side) {
    case Side::Buy:
      return "BUY";
    case Side::Sell:
      return "SELL";
  }
  return "BUY";
}

std::string_view to_string(OrderType type) noexcept {
  switch (type) {
    case OrderType::Market:
      return "MARKET";
    case OrderType::Limit:
      return "LIMIT";
    case OrderType::StopLoss:
      return "SL";
    case OrderType::StopLossMarket:
      return "SL-M";
  }
  return "MARKET";
}

std::string_view to_string(Product product) noexcept {
  switch (product) {
    case Product::Intraday:
      return "INTRADAY";
    case Product::Delivery:
      return "DELIVERY";
    case Product::Margin:
      return "MARGIN";
    case Product::Normal:
      return "NORMAL";
  }
  return "INTRADAY";
}

}  // namespace broker_exec::domain
