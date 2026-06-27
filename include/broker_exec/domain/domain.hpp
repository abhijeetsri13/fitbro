#pragma once

// Umbrella include for the broker_exec::domain value layer (Story 1.2).
// Pulls in the enums, the fixed-point Money/Price/Quantity types, and the
// aggregate value types (Instrument, OrderIntent, Order, Trade, Position).
// Consumers may include the narrower headers directly instead.

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
