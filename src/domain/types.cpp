#include "broker_exec/domain/types.hpp"

#include <string>

#include "broker_exec/domain/enums.hpp"

namespace broker_exec::domain {

std::string Instrument::to_string() const {
  std::string out = "Instrument{symbol=";
  out += symbol;
  out += ",token=";
  out += std::to_string(token);
  out += ",exchange=";
  out += exchange;
  out += ",lot_size=";
  out += lot_size.to_string();
  out += ",tick_size=";
  out += tick_size.to_string();
  out += ",freeze_qty=";
  out += freeze_qty.to_string();
  out += ",expiry=";
  out += expiry;
  out += "}";
  return out;
}

std::string OrderIntent::to_string() const {
  std::string out = "OrderIntent{client_ref=";
  out += client_ref;
  out += ",symbol=";
  out += symbol;
  out += ",side=";
  out += domain::to_string(side);
  out += ",quantity=";
  out += quantity.to_string();
  out += ",price=";
  out += price.to_string();
  out += ",order_type=";
  out += domain::to_string(order_type);
  out += ",product=";
  out += domain::to_string(product);
  out += ",strategy=";
  out += strategy;
  out += "}";
  return out;
}

std::string Order::to_string() const {
  std::string out = "Order{intent=";
  out += intent.to_string();
  out += ",state=";
  out += domain::to_string(state);
  out += ",broker_order_id=";
  out += broker_order_id;
  out += ",filled_qty=";
  out += filled_qty.to_string();
  out += ",avg_price=";
  out += avg_price.to_string();
  out += "}";
  return out;
}

std::string Trade::to_string() const {
  std::string out = "Trade{trade_id=";
  out += trade_id;
  out += ",client_ref=";
  out += client_ref;
  out += ",broker_order_id=";
  out += broker_order_id;
  out += ",quantity=";
  out += quantity.to_string();
  out += ",price=";
  out += price.to_string();
  out += "}";
  return out;
}

std::string Position::to_string() const {
  std::string out = "Position{symbol=";
  out += symbol;
  out += ",net_qty=";
  out += net_qty.to_string();
  out += ",avg_price=";
  out += avg_price.to_string();
  out += "}";
  return out;
}

}  // namespace broker_exec::domain
