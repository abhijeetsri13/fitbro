#pragma once

// portable::hedged_short — a representative strategy, written against the
// broker-neutral port ONLY (Story 6.3, SM-3, AC-1/AC-3).
//
// THE RULE THIS FILE LIVES BY: nothing in this translation unit may name a
// broker. Not a type, not an include, not an identifier, not a comment. The only
// things it is allowed to know about the outside world are
// `ports::BrokerPort`, the `domain` value types, the `capabilities` vocabulary
// and `Result`/`Error`. An automated scan test reads this file (and its .cpp) at
// test time and fails the build if the substrings "k i t e", "k o t a k" or
// "z e r o d h a" appear anywhere in it — which is why this paragraph spells
// them out that way rather than writing them.
//
// WHY THAT MATTERS: a strategy that can name its broker will eventually branch on
// it, and the day it branches is the day switching brokers becomes a code change
// instead of a config change. The scan is the mechanical guard on that claim; the
// proof test is the behavioural one (the same `run()` below, executed against two
// different adapters, must produce an EQUAL outcome value).
//
// THE STRATEGY ITSELF: a hedged short — the option-selling shape the library
// exists to protect. Buy the protective leg FIRST, read broker truth back, and
// only then sell the leg that carries the risk. If the hedge is not confirmed
// live, the short is never attempted; the account is never momentarily naked.
//
// NO-THROW AND TOTAL: `run()` returns an outcome VALUE and never propagates a
// broker error. Every failure the broker reports is converted into a
// broker-NEUTRAL decision recorded in the outcome, because that decision — halt
// or continue — is the thing that has to be identical across brokers. Comparing
// outcomes with `operator==` is what the proof test asserts.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`, no float
// (all money is integer paise through domain::Price).

#include <cstddef>
#include <string>
#include <vector>

#include "broker_exec/capabilities/capabilities.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/ports/broker_port.hpp"

namespace portable {

// Everything the strategy needs, as data. No broker handle, no credentials, no
// endpoint — those belong to the composition root.
struct StrategyParams {
  std::string strategy_id = "hedged_short";

  // The protective leg (bought first) and the risk leg (sold second).
  std::string hedge_symbol;
  std::string short_symbol;

  broker_exec::domain::Quantity quantity;
  broker_exec::domain::Price hedge_price;
  broker_exec::domain::Price short_price;

  // Idempotency keys. In production the dispatcher mints these (Story 1.7);
  // injecting them keeps a test run deterministic and comparable.
  std::string hedge_ref;
  std::string short_ref;
};

// What the strategy observed for one leg. DELIBERATELY CONTAINS NO BROKER ORDER
// ID: id FORMATS are broker-specific ("O1" here, "XYZ7" there) and comparing
// them across brokers would fail for a reason that has nothing to do with
// portability. What is compared instead is whether an id was assigned at all,
// and whether the row came back attributed to us.
struct LegOutcome {
  bool placed = false;                // the port accepted the order
  bool broker_id_assigned = false;    // a non-empty id came back (format ignored)
  bool present_in_orderbook = false;  // a later read found the order
  bool attributed_to_us = false;      // that row carried OUR client_ref
  broker_exec::domain::OrderState state = broker_exec::domain::OrderState::Created;
  broker_exec::domain::Quantity filled;
  broker_exec::domain::Price avg_price;

  [[nodiscard]] bool operator==(const LegOutcome&) const = default;
};

// Fixed, broker-neutral halt vocabulary. A halt caused by a broker failure
// records the normalized `errors::to_string(category)` instead — also
// broker-neutral by contract (CAP-13: the same condition must normalize to the
// same category on every broker).
inline constexpr const char* kHaltHedgeNotConfirmed = "hedge_not_confirmed";

// The complete decision path, comparable with `==`. Two brokers driven by the
// same params must produce EQUAL outcomes; that equality IS the portability
// proof.
struct StrategyOutcome {
  LegOutcome hedge;
  LegOutcome short_leg;

  // Whether the risk leg was even attempted. Hedge-first means this is false
  // whenever the protective leg did not confirm.
  bool short_leg_attempted = false;

  std::size_t orderbook_rows = 0;  // rows broker truth reported on the final read

  // The net position book — what any exit would actually be sized off. Both the
  // row COUNT and the risk leg's signed net quantity are recorded: a count alone
  // would compare equal between two brokers that both returned nothing.
  std::size_t net_positions = 0;
  broker_exec::domain::Quantity short_net_qty;  // signed; negative = short
  bool short_position_found = false;

  // ── The exit path ──
  // An entry strategy that cannot wind its own orders back down has no business
  // opening the position, so the exit is exercised on the same run rather than
  // being assumed to work.
  bool stand_down_attempted = false;
  bool stand_down_accepted = false;

  bool completed = false;   // the full path ran without halting
  std::string halt_reason;  // empty iff completed; see the vocabulary above

  [[nodiscard]] bool operator==(const StrategyOutcome&) const = default;
};

// The capabilities this strategy declares AS DATA, checked at composition time
// (never mid-trade). Declaring them here — next to the code that needs them —
// is what lets the loader reject an incapable broker before a position exists.
[[nodiscard]] std::vector<broker_exec::capabilities::Capability> required_capabilities();

// Run the strategy against ANY broker. The parameter type is the entire coupling
// surface: an abstract port and a plain value struct.
[[nodiscard]] StrategyOutcome run(broker_exec::ports::BrokerPort& broker,
                                  const StrategyParams& params);

}  // namespace portable
