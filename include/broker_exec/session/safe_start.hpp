#pragma once

// broker_exec::session::SafeStartGate — the fail-closed cold-boot gate (Story
// 2.13, FR-22). Trading is blocked until the WHOLE world is verified fresh and
// authenticated, so a restart can never trade into a stale/unauthenticated state.
//
// The gate is decoupled from every concrete module: it composes the world via
// INJECTED `std::function<Result<Ok>()>` checks (one per concern). The
// composition root binds each to the real module (session::validate, the
// reconciler, clock skew/stall, config, refdata::InstrumentMaster::require_fresh,
// TradingCalendar::require_fresh, egress-IP allow-list, crypto-key presence). The
// gate itself depends INWARD only on `ports` (Ok) and `errors`, so it stays
// uniformly testable with simple lambdas.
//
// FAIL-CLOSED, ALL-REQUIRED semantics (unlike the validation gate): every check
// is mandatory. An UNSET (empty std::function) check is a HARD FAILURE — a safety
// gate must never pass a world it cannot verify. The checks run in a FIXED,
// documented order (cheap/foundational first; reconciliation last); the FIRST
// failing/empty check returns a NAMED Error and short-circuits the rest. On a
// wrapped inner Error the category/action/broker_code are PRESERVED (e.g. a
// DataStale from require_fresh, a SessionExpired from the session check) while the
// check name is prepended to the message — mirroring the validation gate's naming.
//
// NO-THROW POLICY: every method returns `Result<Ok>`; nothing throws.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <functional>
#include <string>
#include <vector>

#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/session/session_state.hpp"

namespace broker_exec::session {

// One world-verification check: success-or-Error, no value. The composition root
// binds each to a real module; tests bind simple lambdas.
using SafeCheck = std::function<Result<ports::Ok>()>;

// The ten cold-boot checks, all REQUIRED. An empty std::function is treated as
// an unconfigured (hence unverifiable) world and BLOCKS the start.
struct SafeStartContext {
  SafeCheck config_check;             // config integrity / parse
  SafeCheck strategy_name_check;      // configured strategy names are id-safe (see below)
  SafeCheck crypto_keys_check;        // crypto-key presence (SE-5)
  SafeCheck clock_check;              // clock sanity (skew / stall)
  SafeCheck session_check;            // broker session establishment / liveness
  SafeCheck egress_ip_check;          // egress-IP allow-list match
  SafeCheck instrument_master_check;  // fresh instrument master (refdata)
  SafeCheck calendar_check;           // fresh trading calendar (refdata)
  SafeCheck legacy_stop_check;        // no pre-IMP-11 trigger-less stops (see below)
  SafeCheck reconciliation_check;     // reconciliation completeness (Epic 3)
};

// The single cold-boot authority. There is NO partial/skip variant: verify()
// flips "may trade" true only when the entire world passes.
class SafeStartGate {
 public:
  // Run all ten checks in the FIXED order: config -> strategy-names ->
  // crypto-keys -> clock -> session -> egress-IP -> instrument-master ->
  // calendar -> legacy-stops -> reconciliation. An empty check -> a fail-closed
  // Internal/BlockStrategy "not configured" Error naming it. A check that returns
  // an Error -> that Error WRAPPED (category/action/broker_code preserved, name
  // prepended). The first failing/empty check returns; all pass -> ok() (trading
  // allowed).
  [[nodiscard]] Result<ports::Ok> verify(const SafeStartContext& ctx) const;
};

// ── The IMP-19 strategy-name guard ──────────────────────────────────────────
//
// THE HAZARD, IN ONE SENTENCE: a strategy name is the FIRST SEGMENT of every
// client_ref idempotency::make_client_ref mints (`<strategy>-<sig8>-<uuid>`), and
// domain::is_provenance_id_shape admits a ref into an alert or a ledger entry only
// if EVERY segment is homogeneous — so a strategy called `S1` (one letter plus one
// digit in a single run) makes the ~48-char ref fall back to domain::scrub, which
// destroys it. EVERY alert and EVERY ledger entry about that strategy's orders
// then reads `client_ref=***REDACTED***`, and the order is unlinkable to the
// store, the intent log and the ledger. A name with a space ("iron condor v2") is
// wholly redacted in the `strategy=` column too. Spelling it `S-1` fixes it
// entirely — which is exactly why this is worth catching before trading rather
// than discovering it in the middle of an incident, when the alert you cannot read
// is the one you needed.
//
// WHY IT IS A COLD-BOOT GATE AND NOT AN ORDER-PATH REJECTION. A strategy name is
// CONFIGURATION, known at startup. The only order-path boundary is
// reserve()/Dispatcher::place(), where refusing a name means REFUSING TO PLACE AN
// ORDER — a trading-behaviour change mid-session. A deployment therefore fails
// visibly BEFORE it trades. Nothing in idempotency or the dispatcher changed.
//
// WHAT IT DOES NOT DO: this gate checks the names it is HANDED — in practice
// `config.strategies.names`, a DECLARED list. It is not containment. A host that
// fills `OrderIntent::strategy` directly still mints refs from a name this check
// never saw, and the library itself does exactly that for the square-off exit
// (`exit.strategy = "square_off"`). See the residual note on
// domain::is_valid_strategy_name (domain/redaction.hpp) for why closing that would
// mean an order-path rejection, which is the thing this design declined to add.
//
// Returns ok() when every name in `names` satisfies domain::is_valid_strategy_name
// (an EMPTY list passes vacuously — this check is about the names that exist, and
// the unset-std::function rule above is what catches a forgotten wiring). Otherwise
// a fail-closed Validation/BlockStrategy Error naming the offending name and the
// rule it broke, with a suggested spelling where one exists. The echoed name is
// sanitised and truncated by domain::explain_invalid_strategy_name, so an
// ill-formed or hostile name cannot forge a provenance block or overrun an alert.
//
// RENAMING IS NOT FREE: `strategy` is hashed into idempotency::signal_signature,
// so `S1` -> `S-1` changes the signature of every signal that strategy emits and
// the restart dedupe will not recognise its own in-flight orders — the same hazard
// class as the IMP-11 stop upgrade. Rename BETWEEN sessions, FLAT.
//
// Wire it as: strategy_name_check = [&] {
//   return session::require_valid_strategy_names(config.strategies.names);
// };
[[nodiscard]] Result<ports::Ok> require_valid_strategy_names(const std::vector<std::string>& names);

// ── The IMP-11 legacy-stop guard ────────────────────────────────────────────
//
// THE HAZARD, IN ONE SENTENCE: a stop placed by a pre-IMP-11 binary carried its
// activation level in `price`, the new binary carries it in `trigger_price`, so
// the two disagree about the order's identity — its signal signature changes,
// restart dedupe misses, and the working stop can be PLACED A SECOND TIME. Both
// then fire and the position ends up inverted (naked the other way).
//
// It is cheaply DETECTABLE, which is why this is a gate and not a release note.
// Store migration 2 backfills `trigger_price_paise` as NULL for every pre-existing
// row, so a row that is (a) a StopLoss/StopLossMarket, (b) still WORKING, and
// (c) carrying no trigger is, by construction, a stop written by the old binary —
// a new binary can never produce that combination (the validation gate refuses a
// trigger-less stop outright).
//
// Returns ok() when `orders` holds no such row. Otherwise a fail-closed
// Validation/BlockStrategy Error naming the count and the FIRST offending
// client_ref, telling the operator what to do: flatten or cancel the outstanding
// stops on the old binary, then deploy. No prices appear in the message
// (redaction-safe), only ids the operator needs to act.
//
// ROLLBACK IS SYMMETRIC, and the docs say so: rolling BACK past this release with
// new-binary stops outstanding has the same duplicate hazard in reverse (the old
// build cannot read `trigger_price` at all, and a v2 database refuses to open on
// it). Flatten stops before moving in EITHER direction.
// See docs/upgrade-imp-11-stops.md.
//
// Wire it as: legacy_stop_check = [&] {
//   auto rows = store.all_orders();
//   if (!rows) return broker_exec::fail(std::move(rows.error()));  // fail closed
//   return session::require_no_legacy_stops(rows.value());
// };
[[nodiscard]] Result<ports::Ok> require_no_legacy_stops(const std::vector<domain::Order>& orders);

// True iff `order` is the fingerprint above: a WORKING stop with no trigger.
// Exposed so a caller can log/enumerate the offenders it must clear.
[[nodiscard]] bool is_legacy_trigger_less_stop(const domain::Order& order) noexcept;

// Convenience for wiring the session check: turn a SessionState into a Result.
// Healthy -> ok(); NeedsReauth or Failed -> KiteSessionEstablisher::
// needs_reauth_error() (SessionExpired + ReEstablishSession).
//
// Composition-root wiring (recommended): preserve a transport/5xx error from
// validate() rather than collapsing it to SessionExpired — only map the STATE:
//   session_check = [&]() -> Result<ports::Ok> {
//     auto s = establisher.validate();
//     if (!s) return fail(std::move(s.error()));   // keep Network/ReconcileFirst etc.
//     return session_state_to_result(s.value());   // Healthy/NeedsReauth/Failed
//   };
// (`validate().value_or(SessionState::Failed)` also fails closed, but it
// mislabels a transient transport failure as a dead session.)
[[nodiscard]] Result<ports::Ok> session_state_to_result(SessionState state);

}  // namespace broker_exec::session
