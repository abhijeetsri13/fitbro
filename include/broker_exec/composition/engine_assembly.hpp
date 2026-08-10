#pragma once

// broker_exec::composition — THE RUNTIME GATE-COMPOSITION ROOT (IMP-12).
//
// WHAT THIS IS: the second half of the composition root. `broker_factory.hpp`
// answers "which broker am I talking to, and may I". THIS file answers "what
// must every order survive before it is allowed to reach that broker at all".
//
// ── THE HOLE IT CLOSES ──────────────────────────────────────────────────────
// `risk::GateContext` is deliberately permissive by construction: an ABSENT
// `funds_check` PASSES, a NULL `calendar` SKIPS the time-window, an empty
// `risk_check` PASSES. That is correct for the gate — it is a mechanism, and a
// mechanism must be testable with one input at a time — but it means NOTHING in
// the library previously stopped an operator binary from assembling an
// entry-capable engine with the funds gate simply not wired. Ten hardening
// modules plus FundsView / RiskEngine / TradingCalendar / KillState /
// PostureCoordinator existed as tested islands with no production wiring path.
//
// `make_engine()` is that path, and it is FAIL-CLOSED IN THE OTHER DIRECTION:
// an entry-capable assembly REFUSES TO BUILD when any entry-safety input is
// missing, naming the missing piece in a typed `Validation`/`DoNotRetry` error.
// There is no defaulted argument, no "sensible default", and no silent
// pass-through anywhere in this module. An assembly that exists is an assembly
// whose entry pipeline is complete — that is the whole product of this file.
//
// ── A WIRED GUARD IS NOT THE SAME AS AN ARMED ONE ───────────────────────────
// The refusal matrix stops a MISSING guard. It cannot, on its own, stop a guard
// that is present but answering with nothing — and an adversarial review found
// exactly that class of hole, twice:
//   * a margin source that returns an all-zero quote (a dead broker margin API
//     has no other way to answer a by-value signature) makes `api_required == 0`,
//     which passes the gate's funds check AND the buffered margin stage on a
//     one-rupee balance;
//   * a default-constructed `RiskLimits` is all-off, so a REAL, non-empty
//     `risk_check` is handed to the gate and enforces literally nothing.
// Both are now closed. `MarginInputSource` returns a `Result` so a dead API can
// SAY so, a non-positive quote is refused outright, and an all-off `RiskLimits`
// must be declared deliberately (`EngineOptions::declares_no_risk_limits`) or the
// pre-flight refuses it. The lesson generalizes: every seam that can only answer
// "zero" is a fail-open waiting to happen.
//
// AN EXPLICITLY EXIT-ONLY ASSEMBLY (`EngineMode::ExitOnly`) is the one way to
// build without funds/risk/calendar, and it buys exactly one thing: the right to
// run the exit-legal chain. It still requires the kill switch and the posture
// source, and `preflight_entry()` on one is REFUSED at the `engine-mode` stage —
// it does not "fail the posture check", because posture might be perfectly
// Normal and saying so would be a lie about why the entry was denied.
//
// ── THE PRE-FLIGHT CHAINS (stage-named, fixed order) ────────────────────────
//
//   preflight_entry:  engine-mode -> posture -> session -> instrument ->
//                     risk-limits -> margin(quote) -> gate
//                     (-> duplicate-probe) -> price-band -> margin(buffered)
//   preflight_exit:   exit-class -> posture -> session -> instrument ->
//                     gate(risk-reducing) -> price-band(clamp)
//   preflight_modify: modify-guard -> the whole preflight_exit chain (minus
//                     exit-class), on the AMENDED intent
//
// THE `margin` STAGE BOOKENDS THE GATE, and that is deliberate: the gate's funds
// check has to be SIZED against a requirement, so the quote must be obtained (and
// sanity-checked) before the gate can even be assembled. A quote that cannot be
// obtained therefore fails at `margin` BEFORE `gate` runs. Fail-closed either
// way; the stage name still tells the truth about what was missing.
//
// Every failure carries the NAME of the stage that produced it, both as a
// machine-readable `StageResult::stage` and as a `preflight[<stage>]: ` prefix on
// the (otherwise preserved) inner Error. A caller never has to guess which guard
// spoke, and the inner category/action/broker_code survive intact so the
// runtime's deterministic switch still works (a stale-funds `DataStale`, a
// window `MarketClosed`, a risk `RiskRejected` are all still themselves).
//
// ── THE ENTRY/EXIT TRUST BOUNDARY (read this before calling preflight_exit) ──
//
// `preflight_exit` grants a REAL, load-bearing set of exemptions: no duplicate
// check, no UNKNOWN-pause, no entry cutoff, no funds check, no margin stage, and
// an out-of-band price is clamped instead of blocked. Those exemptions exist
// because a risk-REDUCING order can only ever shrink exposure, so the cost of
// letting a bad one through is bounded while the cost of blocking a good one is
// a stranded naked position.
//
// WHICH FUNCTION YOU CALL IS THEREFORE A SECURITY CLAIM, NOT A CONVENIENCE. The
// engine cannot verify it from the intent alone — "SELL 50 NIFTY" is an exit if
// you are long 50 and an entry if you are flat, and only the caller's book knows
// which. By default the claim is a DOCUMENTED CALLER ASSERTION: calling
// `preflight_exit` on an opening order silently buys every exemption above.
//
// Wire `EngineDeps::is_reducing` to make it CHECKED instead of asserted. When
// present, `preflight_exit` verifies the claim against the caller's own position
// book before granting anything, and a non-reducing intent is refused at the
// `exit-class` stage. Any book that can answer "does this reduce my position"
// should wire it; it is optional only because a book that cannot answer honestly
// would otherwise be forced to lie.
//
// ── SCOPE: PURE DECISION CORE ───────────────────────────────────────────────
// NO threads, NO sockets, NO broker I/O, NO clock reads. Everything arrives
// through the existing seams (`ports::ClockPort` inside FundsView/TradingCalendar,
// `std::function` sources for everything else). This module DECIDES; the caller
// dispatches.
//
// NO-THROW, WITH ONE CAVEAT YOU OWN. Every entry point here returns `Result<T>`
// and this module throws nothing. It cannot, however, contain an exception thrown
// by one of YOUR injected seams: a `std::function` that throws will propagate
// straight through a pre-flight call, and a `std::bad_function_call` from an
// empty `std::function` will do the same (`make_engine` refuses an empty required
// seam precisely so this cannot happen for the ones it knows about). Injected
// sources must be no-throw, returning a typed Error for every failure they can
// have — that is the contract the no-throw boundary rests on.
//
// ── EXPLICITLY OUT OF SCOPE (recorded so the boundary is not re-litigated) ──
// `fillnorm`, `feedsub`, `protection`, `brokerreason` and `ratelimit`'s
// endpoint limiter are RECONCILE- and STREAM-side guards. They do not gate an
// outbound intent; they interpret what comes BACK (a fill event, a tick
// subscription, a rejection string, a per-endpoint budget) or they supervise an
// already-working order. Wiring them here would mean inventing an inbound
// pipeline this module has no business owning. THEY WIRE AT THE
// DISPATCHER/RECONCILE LAYER, which is the only unwired layer remaining after
// this story. `intentlog`/`ledger` checkpointing is likewise a dispatch-path
// concern (record -> fsync -> send -> record), not a pre-flight decision.
//
// ── BUILD CAVEAT: THIS DECISION CORE TRANSITIVELY LINKS THE ADAPTERS ─────────
// `engine_assembly.cpp` lives in the `broker_exec_composition` target, which also
// holds `broker_factory.cpp` and therefore links `broker_exec_kite` +
// `broker_exec_adapters_kotak` (and, through them, cpr/libcurl). NOTHING in this
// file needs an adapter — it depends only on the decision-core modules — so a
// consumer that wants ONLY the pre-flight chains still pays for the broker
// transports at link time.
//
// A separate `broker_exec_engine` target would fix that, and it was considered
// and deliberately deferred: it costs a second library + a second test target out
// of ONE module directory, and a target name that no longer matches its
// `broker_exec/<module>` include path — a direct deviation from the binding
// "one CMake target per module" convention in docs/conventions.md, for a link
// footprint that only matters once a strategy-only binary exists. Revisit it when
// that binary does. The dependency DIRECTION is already correct (nothing here
// includes an adapter header); this is a packaging artifact, not a layering one.
//
// No float anywhere in the money path (integer `Money`/`Price` paise throughout).
// No OS API, no `#ifdef`. C++20 standard library only.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/marginsafety/margin_buffer.hpp"
#include "broker_exec/modes/killswitch.hpp"
#include "broker_exec/modes/posture.hpp"
#include "broker_exec/modifyguard/modify_guard.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/priceband/band_check.hpp"
#include "broker_exec/refdata/trading_calendar.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/risk/funds_view.hpp"
#include "broker_exec/risk/risk_engine.hpp"
#include "broker_exec/risk/validation_gate.hpp"
#include "broker_exec/session/session_state.hpp"

namespace broker_exec::composition {

// What this assembly is licensed to do. A CLOSED enum, and `EngineOptions`
// defaults to the SAFE member: a forgotten `mode` field yields an engine that
// cannot place an entry, never one that can.
enum class EngineMode {
  EntryCapable,  // full entry pipeline; every entry-safety input MUST be wired
  ExitOnly       // exit-legal chain only; may build without funds/risk/calendar
};

// Stable, log/serialization-friendly name (NFR-8 observability contract).
[[nodiscard]] std::string_view to_string(EngineMode mode) noexcept;

// ── Stage names ─────────────────────────────────────────────────────────────
//
// The machine-readable identity of each pre-flight stage. These strings are part
// of the observability contract exactly as `errors::to_string` is: an operator
// dashboard, an audit record and an alert rule all key off them, so RENAMING ONE
// IS A BREAKING CHANGE. They live as named constants rather than string literals
// at the call sites so the emitted name and the tested name cannot drift.
namespace stage {

// The assembly itself refused: an ExitOnly engine was asked for an entry.
inline constexpr std::string_view kEngineMode = "engine-mode";
// The caller called preflight_exit for an intent that does not reduce exposure
// (only raised when EngineDeps::is_reducing is wired — see the trust boundary).
inline constexpr std::string_view kExitClass = "exit-class";
// modes::PostureCoordinator (with the kill switch folded in as operator_floor).
inline constexpr std::string_view kPosture = "posture";
// sessionguard: the mid-session re-auth posture.
inline constexpr std::string_view kSession = "session";
// The resolved instrument does not match the intent it is supposed to describe.
inline constexpr std::string_view kInstrument = "instrument";
// PreflightInputs::risk_limits arms nothing and the caller did not say it meant
// to (EngineOptions::declares_no_risk_limits).
inline constexpr std::string_view kRiskLimits = "risk-limits";
// The idempotency reserve probe itself FAILED (as distinct from reporting a
// duplicate, which the gate names). A probe that cannot answer blocks the entry.
inline constexpr std::string_view kDuplicateProbe = "duplicate-probe";
// risk::ValidationGate. Its own inner check name ("gate: funds check failed…")
// is preserved inside the message, so a gate failure names BOTH.
inline constexpr std::string_view kGate = "gate";
// priceband: an out-of-band ENTRY (or an unknown band, if configured to block).
inline constexpr std::string_view kPriceBand = "price-band";
// marginsafety: the quote itself (obtained + sanity-checked before the gate) and
// the BUFFERED requirement against a FRESH FundsView afterwards.
inline constexpr std::string_view kMargin = "margin";
// modifyguard: a modify that could cancel a working/partially-filled remainder.
inline constexpr std::string_view kModifyGuard = "modify-guard";

}  // namespace stage

// ── Injected sources ────────────────────────────────────────────────────────
//
// EVERY SOURCE BELOW MUST BE NO-THROW (see the no-throw caveat in the file
// header). A fallible one returns a `Result`; one that cannot fail returns a
// plain value.

// The caller's CURRENT session state plus the raw broker text from the last
// mid-session call, exactly as `sessionguard::assess_session` consumes them.
//
// `last_broker_error` is UNTRUSTED and may embed a token-shaped secret. It is
// handed to the classifier and NEVER echoed into a posture detail, an Error
// message, or a log line by this module. The fail-closed default state is
// `Failed` so a forgotten assignment freezes entries rather than opening them.
struct SessionSnapshot {
  session::SessionState state = session::SessionState::Failed;
  std::string last_broker_error;
};

// The detector signals currently active, fed to the posture coordinator. This is
// a SOURCE rather than a per-call argument on purpose: if a caller could pass its
// own signal list to `preflight_entry`, a caller with a stale or optimistic view
// could talk itself past the posture gate. There is one authority.
using DetectorSignalSource = std::function<std::vector<modes::DetectorSignal>()>;

// The current mid-session auth state (same reasoning: one authority, not a
// per-call argument).
using SessionSource = std::function<SessionSnapshot()>;

// The runtime's UNKNOWN-pause flag: true while UNKNOWN orders are unresolved.
using UnknownPauseSource = std::function<bool()>;

// The idempotency reserve probe: "has this signal already been submitted?".
//
// It returns a `Result<bool>` and not a bare `bool` because the store read CAN
// fail, and the distinction between "no, it is new" and "I could not find out"
// is the whole safety property. `GateContext::is_duplicate` is a bare
// `std::function<bool()>` (it has nowhere to put an error), so this module
// evaluates the probe INSIDE that closure, treats an error as duplicate =>
// BLOCKED, and then replaces the gate's DuplicateOrder verdict with the probe's
// own typed Error under the `duplicate-probe` stage — so the caller sees the real
// reason (a store failure) instead of a fabricated "already seen".
//
// WIRING IT (the runtime owns the index/store/uuid lifetimes):
//   deps.duplicate_probe = [&](const domain::OrderIntent& intent) -> Result<bool> {
//     auto reserved = idempotency::reserve(index, store, uuids, intent.strategy, intent);
//     if (!reserved) { return broker_exec::fail(reserved.error()); }
//     return !reserved.value().is_new;
//   };
// `idempotency` is intentionally NOT linked by this module: reserve() drags in
// the SQLite projection, and the composition root has no business owning a
// database handle to answer a boolean.
using DuplicateProbe = std::function<Result<bool>(const domain::OrderIntent&)>;

// The broker-reported margin figures for one intent (single order or basket).
//
// IT RETURNS A `Result`, AND THAT IS THE WHOLE POINT. A by-value signature gives
// a dead margin API (429, 5xx, a parse failure) exactly one way to answer: an
// all-zero `MarginInputs`. Zero passes the gate's funds check against any
// balance, and zero-plus-5% passes the buffered stage too — so the single most
// likely broker failure mode used to read as "this order needs no margin". A
// source that cannot obtain a quote MUST return an Error, and the engine
// additionally REFUSES a non-positive `api_required` as a belt-and-braces guard
// against a source that swallows its own failure.
//
// NOTE WHAT THE ENGINE DOES WITH `MarginInputs::available`: IT OVERWRITES IT.
// The deployable-funds number is taken from a FRESH `FundsView` snapshot, not
// from whatever the source reports, because "buffered requirement vs FundsView,
// fail-closed" is the acceptance criterion and a second, unvalidated funds
// number is exactly the sort of stale figure this library exists to distrust.
// Populate `api_required` / `summed_leg_margin` / `is_multi_leg` /
// `benefit_trusted`; leave `available` at its default.
using MarginInputSource =
    std::function<Result<marginsafety::MarginInputs>(const domain::OrderIntent&)>;

// The naked-sell / hedge-completion check, per intent. Optional (see
// `EngineOptions::declares_no_hedge_check`).
using HedgeCheck = std::function<Result<ports::Ok>(const domain::OrderIntent&)>;

// "Does this intent actually REDUCE exposure?" — answered against the caller's
// own position book, which is the only thing that can know. See the entry/exit
// trust boundary in the file header. Optional; when absent the exit claim is a
// documented caller assertion.
using ReducingClassifier = std::function<bool(const domain::OrderIntent&)>;

// ── Injected dependencies ───────────────────────────────────────────────────
//
// LIFETIME: every pointer here is BORROWED and MUST outlive the EngineAssembly
// built from it. All five live on the single main loop alongside the assembly,
// which is why they are raw pointers and not shared ownership — the decision core
// owns nothing and outlives nothing.
//
// The `std::function` sources are captured BY VALUE into the assembly, so they
// may own their state; whatever they close OVER is still the caller's problem.
//
// NOT THREAD-SAFE, by design (NFR-2): FundsView refreshes, KillState reads and
// the whole pre-flight chain all run on the one main loop.
struct EngineDeps {
  // ── Required in BOTH modes ────────────────────────────────────────────────
  // The operator kill switch. Feeds `GateContext::kill_entry_block` AND the
  // posture coordinator's `operator_floor` (AC-4) — one switch, two effects,
  // never two sources of truth.
  const modes::KillState* kill_state = nullptr;
  // The single posture authority.
  const modes::PostureCoordinator* posture = nullptr;
  // What the detectors currently say (see DetectorSignalSource).
  DetectorSignalSource detector_signals;
  // The mid-session auth state (see SessionSource).
  //
  // REQUIRED FOR EXITS TOO, even though `sessionguard::require_op_allowed` never
  // blocks an exit. An absent source would be a silent pass — precisely the shape
  // of hole this module exists to close — and the exit chain still needs the
  // posture to record WHY it proceeded (and to surface `alert`) in the audit.
  SessionSource session;

  // ── Required for EngineMode::EntryCapable ONLY ────────────────────────────
  // The cadence'd funds view. NON-const: `ensure_fresh()`/`check_margin()` refetch.
  risk::FundsView* funds_view = nullptr;
  // The trading calendar (the gate's entry time-window authority).
  const refdata::TradingCalendar* calendar = nullptr;
  // The four-level risk engine (stateless; bound per intent via make_risk_check).
  // ALSO required whenever `EngineOptions::risk_gates_exits` is set, in either
  // mode — asking for risk-gated exits without an engine to gate them with is a
  // wiring error, not a silently-skipped check.
  const risk::RiskEngine* risk_engine = nullptr;
  // The UNKNOWN-pause flag source.
  UnknownPauseSource unknown_pause;
  // The idempotency reserve probe.
  DuplicateProbe duplicate_probe;
  // The broker margin figures for an intent (fallible — see MarginInputSource).
  MarginInputSource margin_inputs;

  // ── Optional, but it must be SAID (see declares_no_hedge_check) ───────────
  HedgeCheck hedge_check;

  // ── Optional: turn the exit claim from asserted into CHECKED ──────────────
  // See the entry/exit trust boundary in the file header. Absent is legal and
  // documented; wiring it is strictly safer.
  ReducingClassifier is_reducing;
};

// ── Composition options ─────────────────────────────────────────────────────
struct EngineOptions {
  // Defaults to the SAFE member. A caller that forgets this field gets an engine
  // that refuses every entry; a caller that wants entries has to ask for them,
  // and asking triggers the full AC-2 refusal matrix below.
  EngineMode mode = EngineMode::ExitOnly;

  // Gate allow-lists. Empty => accept any (the gate's own semantics, unchanged).
  std::vector<std::string> allowed_exchanges;
  std::vector<domain::Product> allowed_products;

  // Over-freeze handling: true => AllowWithSlicing, false => reject.
  bool slice_mode = true;

  // The margin safety cushion (bps + flat) applied over the broker figure.
  marginsafety::MarginSafetyConfig margin_config;

  // ── Does the risk engine gate EXITS? Default NO, and that is deliberate ──
  //
  // `ValidationGate` applies `risk_check` whenever one is present, for exits as
  // well as entries — and `RiskEngine`'s own header says so explicitly: "the GATE
  // decides whether to even call risk for an exit". The decision is therefore
  // delegated to THIS layer, and this layer answers: an exit is not presented
  // with a risk check unless the operator asks for it.
  //
  // WHY: a tripped daily-loss limit is exactly the moment a protective exit
  // matters most, and a risk-blocked exit strands a live position naked. The
  // limits that would fire on an exit (daily loss, strategy loss) are the ones
  // an exit is trying to STOP getting worse. Setting this true is legitimate for
  // a book where every "exit" is really a rotation, but understand what you are
  // arming. NOTHING about ValidationGate's semantics changes either way — this
  // only decides whether a check is handed to it.
  //
  // Setting it true REQUIRES `EngineDeps::risk_engine`; make_engine refuses the
  // combination rather than silently gating nothing.
  bool risk_gates_exits = false;

  // ── Silence is not a declaration (the broker_factory idiom) ──────────────
  //
  // An empty `EngineDeps::hedge_check` makes the gate's hedge check vacuously
  // pass. For a plain-equity or long-only book that is correct and normal; for a
  // naked-option seller it is the missing guard that ends the account. Since the
  // module cannot tell which you are, it refuses to guess: leaving the hedge
  // check unwired is legal ONLY when you say so here. Deliberately wordy, and
  // deliberately greppable in review.
  bool declares_no_hedge_check = false;

  // The same idiom, for the OTHER vacuous pass — and this one is nastier,
  // because it hides behind a guard that looks wired.
  //
  // A default-constructed `RiskLimits` is all-zero, and zero means "no limit" in
  // every field. `make_risk_check` still produces a perfectly real, non-empty
  // predicate; the gate still calls it; it still returns ok() — for every order,
  // at every size, at every loss. From the outside the risk check is present and
  // green. So an entry with all-off limits is REFUSED at the `risk-limits` stage
  // unless the caller states that it meant it (a pure-execution component whose
  // risk is enforced upstream is the legitimate case).
  bool declares_no_risk_limits = false;

  // ── Should an UNKNOWN band block an entry? Default NO ────────────────────
  //
  // priceband's one deliberate fail-OPEN: a missing or malformed band yields
  // BandUnknown and does NOT block, because freezing all trading on a band-feed
  // outage turns a data problem into a self-inflicted outage. That default is
  // right for a liquid book that mostly trades mid-band.
  //
  // Set this true for a book where out-of-band rejection is the norm rather than
  // the exception (far OTM options, illiquid strikes, expiry-day moves) and an
  // unvalidated entry price is a coin flip. It affects ENTRIES ONLY — an exit is
  // never blocked by this or anything else in the band stage.
  bool block_entry_on_unknown_band = false;
};

// True iff `limits` arms at least one rule — i.e. any field differs from its
// documented "off" sentinel (0 for every count/paise/bps field, false for
// `block_market_orders`). Exposed because the distinction between "a risk check
// is wired" and "a risk check will enforce something" is one every caller of the
// gate needs, and it is invisible from the predicate itself.
[[nodiscard]] bool any_risk_limit_armed(const risk::RiskLimits& limits) noexcept;

// ── Per-intent reference data ───────────────────────────────────────────────
//
// The `ctx` of `preflight_*(intent, ctx)`: the facts about THIS order that the
// caller has already resolved and genuinely owns. Everything that must have a
// single authority (posture, session, duplicate, funds, margin quote) is a
// SOURCE on EngineDeps instead, so it cannot be softened per call.
struct PreflightInputs {
  // Resolved via the instrument master by the caller (the gate keeps its resolve
  // responsibility small — same contract as `GateContext::instrument`). Its
  // `symbol` MUST equal the intent's; the engine refuses the pair otherwise,
  // because a mismatch means lot, tick, freeze and band were all validated
  // against the wrong contract.
  domain::Instrument instrument;
  // The instrument's current circuit / LPP / DPR band as the caller holds it.
  // A default-constructed (known == false) band means "band feed unavailable":
  // priceband deliberately does NOT freeze trading on that, it flags it — see
  // `PreflightOutcome::band_unvalidated` and
  // `EngineOptions::block_entry_on_unknown_band`.
  priceband::PriceBand band;
  // The configured ceilings and the current book, for the four-level risk engine.
  // An all-off `risk_limits` is REFUSED on the entry path unless
  // `EngineOptions::declares_no_risk_limits` says it was meant.
  risk::RiskLimits risk_limits;
  // NOTE ON `order_value_known`: the engine recomputes it as
  // `order_value_known && order_value_paise > 0`. A caller that leaves the
  // default `true` with a zero value would otherwise make RiskEngine's
  // fail-closed "cannot verify order value" branch UNREACHABLE while the
  // max-order-value and max-account-margin limits silently compared against zero.
  risk::RiskState risk_state;
};

// The extra broker truth a modify needs, on top of the two intents.
struct ModifyContext {
  // The order as LAST RECONCILED against broker truth.
  modifyguard::OrderModifyState current_state;
  // The fill qty the caller saw at the moment it DECIDED to modify. A larger
  // `current_state.filled_qty` means a fill raced in and the decision is stale.
  std::int64_t observed_filled_qty = 0;
};

// ── The successful outcome ──────────────────────────────────────────────────
//
// What survived the chain, plus the evidence trail each stage produced. The
// `*_checked` flags are load-bearing: `MarginSafetyResult` default-constructs to
// blocked == true and `BandCheckResult` to BandUnknown (both are those modules'
// fail-closed defaults), so a caller reading `margin.blocked` on an exit outcome
// would read a field that was never computed. Check the flag first.
struct PreflightOutcome {
  // Allow, or AllowWithSlicing when an over-freeze order must be fanned out.
  risk::GateOutcome gate_outcome = risk::GateOutcome::Allow;

  // ── session ──
  // sessionguard raised its operator alert for this pass (a non-Healthy session).
  // The order was still PERMITTED — an exit is never frozen and an entry only
  // reaches here on a Healthy posture — but the condition is worth surfacing,
  // and swallowing it would lose the one signal that says "re-establish soon".
  bool session_alert = false;

  // ── price-band ──
  bool band_checked = false;
  priceband::BandCheckResult band;
  // True when the band was unavailable/malformed and the price was therefore NOT
  // validated (verdict BandUnknown). The order is let through by default — a
  // band-feed outage must not become a self-inflicted trading freeze — but the
  // caller MAY escalate, an audit trail should record that it was unvalidated,
  // and `EngineOptions::block_entry_on_unknown_band` turns it into a refusal.
  bool band_unvalidated = false;
  // True when an EXIT was out of band and priceband suggested clamped price(s).
  // THE CLAMP IS AN OBLIGATION, NOT A NOTE: an unapplied clamp earns the exact
  // "price out of LPP range" rejection the check existed to prevent. Use
  // `apply_clamp()` rather than reaching into the suggestions by hand.
  bool exit_clamped = false;

  // ── margin (entry only) ──
  bool margin_checked = false;
  marginsafety::MarginSafetyResult margin;

  // ── modify (preflight_modify only) ──
  bool modify_checked = false;
  modifyguard::ModifyResult modify;
};

// Apply a band clamp to the intent that produced `outcome`, in place.
//
// WHY THIS EXISTS AS A FUNCTION. `exit_clamped` used to be advice: the caller was
// told to copy `band.suggested_limit` into `price` and `band.suggested_trigger`
// into `trigger_price`, each under its own `has_*` flag. Two flags, two fields,
// one silent failure mode — a caller who applies the limit and forgets the
// trigger sends a stop whose activation level is still outside the band, which
// the exchange rejects for exactly the reason the clamp existed to avoid. This
// applies BOTH or neither, and returns whether it changed anything.
//
// No-throw, no float (integer paise relabelled from Money to Price). Safe to call
// unconditionally: it does nothing when the band was not checked or when there is
// nothing to apply, so `apply_clamp(intent, outcome);` is a correct default.
bool apply_clamp(domain::OrderIntent& intent, const PreflightOutcome& outcome) noexcept;

// One stage's verdict. `stage` names the stage that DECIDED — the failing one on
// an error, the last one to run on success. Compare it against the `stage::`
// constants above rather than against a literal.
struct StageResult {
  std::string_view stage;
  Result<PreflightOutcome> result;

  [[nodiscard]] bool ok() const noexcept { return result.has_value(); }
};

class EngineAssembly;

// Compose the runtime gate. Validation order (AC-2):
//   1. the pieces required in BOTH modes: kill_state, posture, detector_signals,
//      session;
//   2. the pieces required for an ENTRY-CAPABLE engine: funds_view, calendar,
//      risk_engine, unknown_pause, duplicate_probe, margin_inputs;
//   3. the option/dependency COMBINATIONS that cannot both be true
//      (risk_gates_exits without a risk_engine);
//   4. the hedge declaration.
// Each missing piece is its own typed `Validation`/`DoNotRetry` error NAMING the
// field and saying what it gates. The wiring checks run before the declaration
// check because "you did not wire the kill switch" is a more fundamental
// diagnostic than "you did not tell me you meant to skip hedging".
//
// `options` IS MANDATORY — there is no defaulted argument, for the same reason
// `make_broker` has none: a defaulted `EngineOptions{}` would be the path of
// least resistance to an unexamined posture.
//
// This function performs NO I/O and touches no broker. It is a wiring verdict.
[[nodiscard]] Result<EngineAssembly> make_engine(const EngineDeps& deps,
                                                 const EngineOptions& options);

// A fully-wired, ready-to-use decision core.
//
// Move-only, and it can only be produced by `make_engine()` (private
// constructor): there is NO way to obtain an entry-capable assembly that skipped
// the AC-2 refusal matrix. That is the structural property the whole story buys.
class EngineAssembly {
 public:
  EngineAssembly(EngineAssembly&&) noexcept = default;
  EngineAssembly& operator=(EngineAssembly&&) noexcept = default;
  EngineAssembly(const EngineAssembly&) = delete;
  EngineAssembly& operator=(const EngineAssembly&) = delete;

  [[nodiscard]] EngineMode mode() const noexcept { return options_.mode; }
  [[nodiscard]] bool is_entry_capable() const noexcept {
    return options_.mode == EngineMode::EntryCapable;
  }

  // ── AC-4: the posture authority, with the kill switch as operator floor ──
  //
  // KillState::posture() -> `operator_floor` -> PostureCoordinator::evaluate().
  // The kill switch is therefore not a parallel check that could disagree with
  // the posture; it IS the floor beneath it, which is why a Panic kill can only
  // ever produce a Panic posture and a Soft/Broker/Account kill can only ever
  // produce SoftKill-or-worse. No-throw, cheap, callable every loop tick.
  //
  // THIS IS THE PROCESS-WIDE POSTURE: the floor is the WHOLE kill set. It is the
  // right thing for an operator dashboard or a health endpoint. It is NOT what
  // the pre-flight chains use — see evaluate_posture_for_strategy.
  [[nodiscard]] modes::Posture evaluate_posture(
      const std::vector<modes::DetectorSignal>& active) const;

  // The same, reading the active signals from the injected source. This is the
  // overload a status surface wants, so a caller cannot hand it a rosier signal
  // list than the detectors actually report.
  [[nodiscard]] modes::Posture evaluate_posture() const;

  // The posture AS IT APPLIES TO ONE STRATEGY — what the pre-flight chains use.
  //
  // The difference is the operator floor, and it is `KillState::blocks_strategy`
  // that draws the line, not this module: a Strategy kill blocks ONLY its own
  // scope, while Soft / Broker / Account / Panic are broad and block every
  // strategy. So killing strategy "beta" must not freeze strategy "alpha" —
  // multi-strategy isolation is the entire point of a scoped kill, and folding
  // the unscoped `posture()` into every strategy's floor quietly turned every
  // Strategy kill into an account-wide one.
  //
  // Detector signals are process-wide and apply unchanged; only the floor is
  // scoped. Panic and Account therefore still stop everything.
  [[nodiscard]] modes::Posture evaluate_posture_for_strategy(std::string_view strategy) const;

  // ── AC-3: the pre-flight chains ─────────────────────────────────────────

  // engine-mode -> posture -> session -> instrument -> risk-limits ->
  // margin(quote) -> gate (-> duplicate-probe) -> price-band -> margin(buffered).
  //
  // POSTURE IS CONSULTED FIRST among the runtime stages (AC-4). The engine-mode
  // refusal precedes it, but that is not a stage of the chain — it is the
  // assembly declining a request it was never built to serve (an ExitOnly engine
  // has no funds view, no calendar and no risk engine, so there is no entry
  // pipeline to run). Reporting that as a posture failure would name the wrong
  // culprit.
  //
  // On success: `gate_outcome` (watch for AllowWithSlicing), `band_checked` with
  // the WithinBand/BandUnknown verdict, `margin_checked` with the full buffered
  // evidence trail, `session_alert` if the session is degraded.
  [[nodiscard]] StageResult preflight_entry(const domain::OrderIntent& intent,
                                            const PreflightInputs& ctx) const;

  // exit-class -> posture -> session -> instrument -> gate(is_risk_reducing) ->
  // price-band(clamp).
  //
  // READ THE ENTRY/EXIT TRUST BOUNDARY IN THE FILE HEADER BEFORE CALLING THIS.
  // Calling it is a claim that the intent reduces exposure, and that claim buys
  // real exemptions. Wire `EngineDeps::is_reducing` to have the claim checked at
  // the `exit-class` stage instead of trusted.
  //
  // THE ENTRY-ONLY STAGES ARE NOT SKIPPED BY A FLAG — THEY ARE NOT HERE. There is
  // no duplicate probe, no UNKNOWN-pause, no entry cutoff, no funds check and no
  // margin stage in this function, so no future edit can accidentally re-arm one
  // against a protective leg. The gate is run with `is_risk_reducing = true`, so
  // its own entry-only blocks stand down while shape / tick / lot / exchange /
  // product / freeze still apply — a malformed stop is a broker rejection, not
  // protection.
  //
  // The band is CLAMPED, never blocking: `exit_clamped` plus the suggestions on
  // `band`, applied with `apply_clamp()`. Runs on an ExitOnly assembly and on an
  // EntryCapable one alike.
  //
  // The ONE way this returns an error at the posture stage is Panic, where the
  // normal exit gate closes because the emergency engine drives the square-off
  // out-of-band (modes' documented contract, unchanged here).
  [[nodiscard]] StageResult preflight_exit(const domain::OrderIntent& intent,
                                           const PreflightInputs& ctx) const;

  // modify-guard -> the exit-legal chain on the AMENDED intent.
  //
  // The guard runs FIRST because "this order cannot be modified at all"
  // (terminal, awaiting reconcile, a fill raced in, a qty modify on a partial)
  // dominates every question about the amended order's shape or price. The
  // request is DIFFED out of the two intents via
  // `modifyguard::make_modify_request`, never hand-asserted — that is what stops
  // a caller who moved only a stop's TRIGGER from presenting a "modify that
  // changes nothing" and having it allowed.
  //
  // The amended intent then runs the exit-legal chain: a modify is exit-shaped
  // work (it re-prices or re-sizes something already working) and must not be
  // subjected to entry-only blocks, but it absolutely must still be a well-formed,
  // in-band order. Permitted in BOTH engine modes for the same reason.
  //
  // THE `exit-class` CHECK DOES NOT RUN HERE, deliberately: modifying a working
  // ENTRY order is legitimate and is not a risk-reducing act, so demanding that
  // the amended intent reduce exposure would refuse a routine re-price. The
  // modify guard is the authority on this path.
  //
  // On success `modify_checked` is set with the guard's Allow verdict alongside
  // the exit chain's band evidence.
  [[nodiscard]] StageResult preflight_modify(const domain::OrderIntent& current,
                                             const domain::OrderIntent& amended,
                                             const ModifyContext& modify_ctx,
                                             const PreflightInputs& ctx) const;

 private:
  // Parameter names deliberately avoid the member spellings so no compiler's
  // shadow diagnostic (-Wshadow / MSVC C4458) has anything to say.
  EngineAssembly(EngineDeps wiring, EngineOptions policy) noexcept
      : deps_(std::move(wiring)), options_(std::move(policy)) {}

  friend Result<EngineAssembly> make_engine(const EngineDeps&, const EngineOptions&);

  // The shared tail of preflight_exit and preflight_modify: posture + session +
  // instrument + gate(risk-reducing) + price-band clamp, starting from an outcome
  // the caller has already begun.
  [[nodiscard]] StageResult exit_chain(const domain::OrderIntent& intent,
                                       const PreflightInputs& ctx,
                                       PreflightOutcome outcome) const;

  EngineDeps deps_;
  EngineOptions options_;
  // Stateless value; the gate holds no configuration of its own.
  risk::ValidationGate gate_;
};

}  // namespace broker_exec::composition
