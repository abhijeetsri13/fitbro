#pragma once

// broker_exec::ports — umbrella include for all abstract port interfaces.
//
// The execution core depends ONLY on these abstractions and never on a concrete
// broker SDK, database, OS API, or clock. Concrete impls (adapters, the system
// clock, the durable store, ...) are injected at composition time. Include the
// individual headers where you only need one; include this for the whole set.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/clock_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/ports/refdata_port.hpp"
#include "broker_exec/ports/secret_provider.hpp"
#include "broker_exec/ports/store_port.hpp"
