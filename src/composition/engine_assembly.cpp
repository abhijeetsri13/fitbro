#include "broker_exec/composition/engine_assembly.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "broker_exec/domain/money.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/sessionguard/session_guard.hpp"

namespace broker_exec::composition {

namespace {

using errors::Error;
using errors::ErrorCategory;
using errors::SuggestedAction;

// A WIRING verdict: this module has spoken to no broker, so `broker_code` stays
// empty (per errors/error.hpp it carries a raw code FROM a broker or transport;
// stuffing a synthetic token into it would corrupt the field for anything that
// aggregates broker codes). The discriminator lives in the message, which names
// the missing field verbatim so an operator can grep the struct for it.
[[nodiscard]] Error wiring_error(std::string message) {
  Error error = errors::make_error(ErrorCategory::Validation, std::move(message));
  // Validation already defaults to DoNotRetry; set it explicitly so the contract
  // survives a change to default_action_for().
  error.action = SuggestedAction::DoNotRetry;
  return error;
}

// Prefix an inner Error with the stage that produced it while PRESERVING its
// category, action and broker_code. This is the same discipline the validation
// gate uses on its own injected predicates, and for the same reason: the
// runtime's deterministic switch is on (category, action), so a stale-funds
// DataStale must still arrive as a DataStale even after two layers of wrapping.
// Only the human-readable message gains the stage name.
[[nodiscard]] Error staged(std::string_view stage_name, Error inner) {
  Error error = std::move(inner);
  error.message = "preflight[" + std::string(stage_name) + "]: " + error.message;
  return error;
}

// Build a fresh, engine-owned Error that names the stage.
[[nodiscard]] Error stage_error(std::string_view stage_name, ErrorCategory category,
                                SuggestedAction action, const std::string& detail) {
  Error error =
      errors::make_error(category, "preflight[" + std::string(stage_name) + "]: " + detail);
  error.action = action;
  return error;
}

[[nodiscard]] StageResult failed(std::string_view stage_name, Error error) {
  return StageResult{stage_name, broker_exec::fail(std::move(error))};
}

[[nodiscard]] StageResult passed(std::string_view stage_name, PreflightOutcome outcome) {
  return StageResult{stage_name, Result<PreflightOutcome>(std::move(outcome))};
}

// ── The modify-guard verdict -> typed error taxonomy mapping ────────────────
//
// modifyguard is a pure decision module with no Result wrapper of its own (it
// returns a populated verdict), so the composition root is where its verdicts
// become the library's typed taxonomy. The mapping is chosen so the runtime's
// switch does the right thing WITHOUT reading the message:
//
//   RejectTerminal          -> OrderNotFound / ReconcileFirst. The order is
//                              already Filled/Cancelled/Rejected — this is
//                              literally errors.hpp's definition of
//                              OrderNotFound ("modify/cancel on an unknown /
//                              already-terminal order").
//   RejectRacedFill         -> Validation / ReconcileFirst. A fill landed since
//                              the decision; the ONLY safe next step is fresh
//                              broker truth, never a blind resend.
//   RejectNotModifiable     -> Validation / ReconcileFirst. Ambiguous /
//                              awaiting-reconcile state, same remedy.
//   RejectQtyOnPartial      -> RiskRejected / DoNotRetry. Not a transient
//   RejectShrinkBelowFilled    condition and not a reconcile problem: the
//                              REQUEST itself would cancel a working remainder.
//                              Retrying it unchanged repeats the hazard, so the
//                              caller must change the request (price-only, or a
//                              new total above filled).
struct ModifyRejection {
  ErrorCategory category = ErrorCategory::Validation;
  SuggestedAction action = SuggestedAction::ReconcileFirst;
};

[[nodiscard]] ModifyRejection rejection_for(modifyguard::ModifyVerdict verdict) noexcept {
  switch (verdict) {
    case modifyguard::ModifyVerdict::RejectQtyOnPartial:
    case modifyguard::ModifyVerdict::RejectShrinkBelowFilled:
      return ModifyRejection{ErrorCategory::RiskRejected, SuggestedAction::DoNotRetry};
    case modifyguard::ModifyVerdict::RejectTerminal:
      return ModifyRejection{ErrorCategory::OrderNotFound, SuggestedAction::ReconcileFirst};
    case modifyguard::ModifyVerdict::RejectRacedFill:
    case modifyguard::ModifyVerdict::RejectNotModifiable:
    case modifyguard::ModifyVerdict::Allow:
      break;
  }
  // Fail-closed default for Allow (never reached — the caller checks `allowed`
  // first) and for any future verdict that forgets its case: reconcile, do not
  // guess.
  return ModifyRejection{ErrorCategory::Validation, SuggestedAction::ReconcileFirst};
}

}  // namespace

std::string_view to_string(EngineMode mode) noexcept {
  switch (mode) {
    case EngineMode::EntryCapable:
      return "EntryCapable";
    case EngineMode::ExitOnly:
      return "ExitOnly";
  }
  return "unknown_engine_mode";
}

bool any_risk_limit_armed(const risk::RiskLimits& limits) noexcept {
  // EVERY field, against its documented "off" sentinel. Listed exhaustively and
  // deliberately verbose rather than folded into a clever expression: a field
  // added to RiskLimits and forgotten here would make this function claim
  // "nothing armed" for a limit that IS armed, which is the fail-open direction.
  // A reviewer can diff this list against the struct in one glance.
  return limits.daily_loss_limit_paise != 0 ||    //
         limits.max_open_positions != 0 ||        //
         limits.max_account_margin_paise != 0 ||  //
         limits.max_order_value_paise != 0 ||     //
         limits.block_market_orders ||            //
         limits.max_slippage_bps != 0 ||          //
         limits.max_lots_per_strategy != 0 ||     //
         limits.max_lots_per_instrument != 0 ||   //
         limits.strategy_daily_loss_limit_paise != 0;
}

bool apply_clamp(domain::OrderIntent& intent, const PreflightOutcome& outcome) noexcept {
  if (!outcome.band_checked) {
    return false;
  }
  bool applied = false;
  // Money and Price are both exact integer paise, so this is a relabelling, not a
  // conversion — no float, no rounding. priceband only sets a `has_*` flag for a
  // price the order type actually USES, so applying both under their own flags
  // cannot write a number into a field the broker never reads.
  if (outcome.band.has_suggestion) {
    intent.price = domain::Price::from_paise(outcome.band.suggested_limit.paise());
    applied = true;
  }
  if (outcome.band.has_trigger_suggestion) {
    intent.trigger_price = domain::Price::from_paise(outcome.band.suggested_trigger.paise());
    applied = true;
  }
  return applied;
}

// ═══════════════════════════════════════════════════════════════════════════
// AC-2 — the fail-closed builder.
// ═══════════════════════════════════════════════════════════════════════════
//
// Read the messages below as the actual specification of this story: each one
// says which field is missing AND what stops being enforced without it. That
// second half is deliberate — "EngineDeps::funds_view is required" tells an
// operator what to type; "…without it GateContext::funds_check is ABSENT, which
// the gate treats as PASS" tells them why the build refused instead of
// defaulting, which is the lesson that stops it recurring.
Result<EngineAssembly> make_engine(const EngineDeps& deps, const EngineOptions& options) {
  // ── 1. Required in BOTH modes ────────────────────────────────────────────
  // An ExitOnly assembly is a REDUCED engine, not an UNGUARDED one. It gives up
  // the entry pipeline; it does not give up knowing whether the operator has
  // pulled the kill switch or whether the process is in a degraded posture.
  if (deps.kill_state == nullptr) {
    return fail(wiring_error(
        "composition: EngineDeps::kill_state is required in every mode (modes::KillState gates "
        "entries via GateContext::kill_entry_block AND supplies the posture operator_floor — "
        "without it a Panic kill would resolve to a Normal posture)"));
  }
  if (deps.posture == nullptr) {
    return fail(wiring_error(
        "composition: EngineDeps::posture is required in every mode (modes::PostureCoordinator is "
        "the single degradation-posture authority; without it no detector failure could ever block "
        "an entry or route an exit to the emergency engine)"));
  }
  if (!deps.detector_signals) {
    return fail(wiring_error(
        "composition: EngineDeps::detector_signals is required in every mode (the posture signal "
        "source; an absent source would resolve every posture from the operator floor alone, so a "
        "stale feed, a mute socket, a clock stall or a reconcile mismatch would all read as "
        "Normal)"));
  }
  if (!deps.session) {
    return fail(wiring_error(
        "composition: EngineDeps::session is required in every mode (the mid-session re-auth "
        "state; an absent source would silently pass entries against a token that died DURING the "
        "session)"));
  }

  // ── 2. Required for an ENTRY-CAPABLE engine ──────────────────────────────
  // Every one of these corresponds to a GateContext input whose absent form is
  // "PASS". That permissiveness is correct in the gate and catastrophic in a
  // production wiring, which is the entire reason this function exists.
  if (options.mode == EngineMode::EntryCapable) {
    if (deps.funds_view == nullptr) {
      return fail(wiring_error(
          "composition: EngineDeps::funds_view is required for an EntryCapable engine "
          "(risk::FundsView supplies GateContext::funds_check via make_funds_check, and the FRESH "
          "available-margin figure the buffered marginsafety stage is tested against; an ABSENT "
          "funds_check is treated by the gate as PASS, so entries would be sized against "
          "nothing)"));
    }
    if (deps.calendar == nullptr) {
      return fail(wiring_error(
          "composition: EngineDeps::calendar is required for an EntryCapable engine "
          "(refdata::TradingCalendar is the gate's entry time-window authority; a NULL calendar "
          "makes the gate SKIP the time-window check entirely, so entries would be admitted on a "
          "holiday, before open, or past the entry cutoff)"));
    }
    if (deps.risk_engine == nullptr) {
      return fail(wiring_error(
          "composition: EngineDeps::risk_engine is required for an EntryCapable engine "
          "(risk::RiskEngine supplies GateContext::risk_check via make_risk_check; an EMPTY "
          "risk_check is treated by the gate as PASS, so every account/strategy/instrument/order "
          "limit would be unenforced)"));
    }
    if (!deps.unknown_pause) {
      return fail(wiring_error(
          "composition: EngineDeps::unknown_pause is required for an EntryCapable engine (the "
          "runtime's UNKNOWN-pause flag; it defaults to false on GateContext, so without a source "
          "entries would keep flowing while UNKNOWN orders are still unresolved)"));
    }
    if (!deps.duplicate_probe) {
      return fail(wiring_error(
          "composition: EngineDeps::duplicate_probe is required for an EntryCapable engine (the "
          "idempotency reserve probe behind GateContext::is_duplicate; an empty probe means NO "
          "duplicate is ever detected, so one strategy signal could become two live orders)"));
    }
    if (!deps.margin_inputs) {
      return fail(wiring_error(
          "composition: EngineDeps::margin_inputs is required for an EntryCapable engine (the "
          "broker-reported margin figures; without them there is no requirement to buffer and the "
          "marginsafety stage would have nothing to check)"));
    }
  }

  // ── 3. Combinations that cannot both be true ─────────────────────────────
  // Asking for risk-gated exits without an engine to gate them with is not a
  // harmless no-op: the caller has explicitly OPTED IN to the stricter posture
  // and would silently get the permissive one. Refuse rather than under-deliver.
  if (options.risk_gates_exits && deps.risk_engine == nullptr) {
    return fail(wiring_error(
        "composition: EngineOptions::risk_gates_exits is set but EngineDeps::risk_engine is null — "
        "the exit chain would present NO risk check to the gate, silently giving you the "
        "permissive posture you explicitly opted out of. Wire risk_engine, or leave "
        "risk_gates_exits false."));
  }

  // ── 4. The hedge declaration — silence is not a declaration ──────────────
  // Runs LAST on purpose: "you did not wire the kill switch" is a more
  // fundamental diagnostic than "you did not tell me you meant to skip hedging",
  // and a caller missing both should hear the former first.
  if (!deps.hedge_check && !options.declares_no_hedge_check) {
    return fail(wiring_error(
        "composition: EngineDeps::hedge_check is empty — an empty hedge check makes the gate's "
        "naked-sell / hedge-completion check pass VACUOUSLY. That is correct for a long-only or "
        "cash-equity book and fatal for a naked option seller, and this module cannot tell which "
        "you are. If the book genuinely needs no hedge check, set "
        "EngineOptions::declares_no_hedge_check = true."));
  }

  return EngineAssembly(deps, options);
}

// ═══════════════════════════════════════════════════════════════════════════
// AC-4 — posture, with the kill switch as the operator floor.
// ═══════════════════════════════════════════════════════════════════════════
modes::Posture EngineAssembly::evaluate_posture(
    const std::vector<modes::DetectorSignal>& active) const {
  // KillState -> operator_floor -> PostureCoordinator::evaluate. The kill switch
  // is the FLOOR beneath the detector signals, not a parallel verdict that could
  // disagree with them: a Panic kill therefore cannot resolve to anything below
  // Panic no matter what the detectors say, and a clean detector set cannot talk
  // the engine out of an active kill.
  //
  // NB the spelling: NOT `floor` — that would shadow ::floor from <cmath> and the
  // project builds with -Wshadow/-Werror (MSVC C4459 under /W4 /WX).
  const modes::Posture operator_floor = deps_.kill_state->posture();
  return deps_.posture->evaluate(active, operator_floor);
}

modes::Posture EngineAssembly::evaluate_posture() const {
  return evaluate_posture(deps_.detector_signals());
}

modes::Posture EngineAssembly::evaluate_posture_for_strategy(std::string_view strategy) const {
  // THE SCOPE RULE IS KillState's, NOT OURS. `blocks_strategy` already answers
  // "does the active kill set stop THIS strategy", and it answers true for every
  // broad kill (Soft / Broker / Account / Panic) and for a Strategy kill whose
  // scope matches. So this reads: if the kills apply to me, my floor is the full
  // kill posture; if they do not, my floor is Normal and the detector signals
  // alone decide.
  //
  // Deriving the floor from the unscoped `posture()` instead would promote every
  // Strategy kill into an account-wide freeze, which is precisely the isolation
  // a scoped kill exists to provide.
  const modes::Posture operator_floor = deps_.kill_state->blocks_strategy(strategy)
                                            ? deps_.kill_state->posture()
                                            : modes::Posture::Normal;
  return deps_.posture->evaluate(deps_.detector_signals(), operator_floor);
}

// ═══════════════════════════════════════════════════════════════════════════
// AC-3 — preflight_entry.
// ═══════════════════════════════════════════════════════════════════════════
StageResult EngineAssembly::preflight_entry(const domain::OrderIntent& intent,
                                            const PreflightInputs& ctx) const {
  PreflightOutcome outcome;

  // ── engine-mode ──────────────────────────────────────────────────────────
  // Not a stage of the chain: the assembly is declining a request it was never
  // built to serve. Answering "posture blocked it" here would be a lie whenever
  // the posture is Normal, and a lie in a rejection message is how an operator
  // spends an hour debugging the wrong subsystem. It runs BEFORE the posture for
  // the same reason — an ExitOnly engine has no entry pipeline to evaluate, so
  // there is nothing for a posture verdict to be about.
  if (!is_entry_capable()) {
    return failed(
        stage::kEngineMode,
        stage_error(stage::kEngineMode, ErrorCategory::NotSupported, SuggestedAction::DoNotRetry,
                    "this assembly was composed as " + std::string(to_string(options_.mode)) +
                        " and has no entry pipeline (it was permitted to build without a funds "
                        "view, calendar or risk engine); it can never place an entry"));
  }

  // ── posture (consulted FIRST among the runtime stages — AC-4) ────────────
  // Scoped to this intent's strategy: a Strategy kill on another strategy must
  // not freeze this one (see evaluate_posture_for_strategy).
  const modes::Posture posture = evaluate_posture_for_strategy(intent.strategy);
  if (auto entry_allowed = modes::PostureCoordinator::require_entry_allowed(posture);
      !entry_allowed) {
    return failed(stage::kPosture, staged(stage::kPosture, std::move(entry_allowed.error())));
  }

  // ── session ──────────────────────────────────────────────────────────────
  const SessionSnapshot snapshot = deps_.session();
  const sessionguard::SessionPosture session_posture =
      sessionguard::assess_session(snapshot.state, snapshot.last_broker_error);
  outcome.session_alert = session_posture.alert;
  if (auto op_allowed =
          sessionguard::require_op_allowed(session_posture, sessionguard::OpClass::Entry);
      !op_allowed) {
    return failed(stage::kSession, staged(stage::kSession, std::move(op_allowed.error())));
  }

  // ── instrument ───────────────────────────────────────────────────────────
  // The resolved reference data must actually describe the order under test. A
  // mismatch is not cosmetic: lot size, tick size, freeze ceiling and the price
  // band would every one of them be validated against a DIFFERENT contract, so
  // the gate would return Allow having checked nothing that applies.
  if (intent.symbol != ctx.instrument.symbol) {
    return failed(
        stage::kInstrument,
        stage_error(stage::kInstrument, ErrorCategory::Validation, SuggestedAction::DoNotRetry,
                    "resolved instrument '" + ctx.instrument.symbol +
                        "' does not match the intent's symbol '" + intent.symbol +
                        "' — lot, tick, freeze and band would all be checked against the "
                        "wrong contract"));
  }

  // ── risk-limits (silence is not a declaration) ───────────────────────────
  // An all-off RiskLimits still produces a real, non-empty risk_check that the
  // gate will happily call and that will always say ok(). From the outside the
  // guard looks wired and green. Refuse it unless the caller says it meant it.
  if (!options_.declares_no_risk_limits && !any_risk_limit_armed(ctx.risk_limits)) {
    return failed(
        stage::kRiskLimits,
        stage_error(stage::kRiskLimits, ErrorCategory::Validation, SuggestedAction::DoNotRetry,
                    "PreflightInputs::risk_limits arms NOTHING (every field is at its "
                    "'off' sentinel), so the risk check would be handed to the gate and "
                    "pass for every order at every size and every loss. If this "
                    "component's risk is genuinely enforced upstream, set "
                    "EngineOptions::declares_no_risk_limits = true."));
  }

  // ── margin: the QUOTE (an input the gate's funds check is sized against) ──
  // Obtained before the gate because the gate cannot be assembled without a
  // requirement. A source that cannot obtain one MUST say so; the returned Error
  // is normalized to DataStale/BlockStrategy because, whatever the transport-level
  // cause, what the engine is missing is a trustworthy margin figure.
  marginsafety::MarginInputs margin;
  {
    auto quoted = deps_.margin_inputs(intent);
    if (!quoted) {
      return failed(
          stage::kMargin,
          stage_error(stage::kMargin, ErrorCategory::DataStale, SuggestedAction::BlockStrategy,
                      "the broker margin quote could not be obtained (" +
                          std::string(errors::to_string(quoted.error().category)) + ": " +
                          quoted.error().message + ")"));
    }
    margin = std::move(quoted.value());
  }
  // BELT AND BRACES over the Result. A source that swallows its own failure has
  // exactly one way to express it in a value: zeros. A zero requirement passes
  // the gate's funds check against ANY balance and passes the buffered stage too
  // (zero plus five percent is still zero), so it is refused outright. There is
  // no entry that genuinely blocks no margin.
  if (margin.api_required.paise() <= 0) {
    return failed(
        stage::kMargin,
        stage_error(stage::kMargin, ErrorCategory::DataStale, SuggestedAction::BlockStrategy,
                    "margin quote absent or non-positive (api_required " +
                        margin.api_required.to_string() +
                        ") — a zero requirement would pass the funds check against any "
                        "balance; refusing rather than sizing an entry against nothing"));
  }

  // ── gate ─────────────────────────────────────────────────────────────────
  // THIS is the assembly promised by AC-1: a GateContext with the REAL funds
  // check, the REAL calendar, the REAL risk check, live kill/pause flags, the
  // duplicate probe and the hedge check. Nothing here is a stub and nothing is
  // left absent — make_engine already refused the assembly if it could have been.
  std::optional<Error> probe_error;
  risk::GateContext gate_ctx{.intent = intent, .instrument = ctx.instrument};
  gate_ctx.is_risk_reducing = false;
  // SCOPED, matching the posture floor above. `KillState::blocks_entries()` is the
  // unscoped "is any kill active" question; using it here would re-broaden every
  // Strategy kill into an account-wide freeze at the gate, undoing at check #1
  // exactly the isolation the posture stage just honoured.
  gate_ctx.kill_entry_block = deps_.kill_state->blocks_strategy(intent.strategy);
  gate_ctx.unknown_pause_active = deps_.unknown_pause();
  // The probe runs INSIDE the gate's own ordering (kill -> pause -> duplicate),
  // so wiring it does not re-order the gate's checks. `GateContext::is_duplicate`
  // is a bare bool seam with nowhere to put an error, so a probe FAILURE is
  // reported as `true` (fail-closed: an unanswerable duplicate question blocks
  // the entry) and the real Error is carried out through `probe_error`.
  gate_ctx.is_duplicate = [this, &intent, &probe_error]() -> bool {
    Result<bool> probed = deps_.duplicate_probe(intent);
    if (!probed) {
      probe_error = probed.error();
      return true;
    }
    return probed.value();
  };
  gate_ctx.calendar = deps_.calendar;
  gate_ctx.funds_check = deps_.funds_view->make_funds_check(margin.api_required.paise());
  {
    // TWO wiring corrections to the caller's RiskState, both fail-closed:
    //
    //  * `is_entry` is set BY THE COMPOSITION ROOT, not trusted from the caller:
    //    this is the entry path by construction, and letting a caller mark an
    //    entry as a non-entry would silently disarm the max-open-positions limit.
    //  * `order_value_known` is ANDed with an actually-positive value. RiskEngine
    //    fails closed on an unknown order value for the two limits that need it
    //    (max order value, max account margin) — but the default state is
    //    `known == true, value == 0`, so a caller that never populates the value
    //    makes that fail-closed branch UNREACHABLE and both limits compare
    //    against zero, which always passes. A value of zero is not a known value.
    //
    // Neither touches RiskEngine's semantics; both decide what it is told.
    risk::RiskState state = ctx.risk_state;
    state.is_entry = true;
    state.order_value_known = state.order_value_known && state.order_value_paise > 0;
    gate_ctx.risk_check = risk::make_risk_check(*deps_.risk_engine, intent, ctx.risk_limits, state);
  }
  if (deps_.hedge_check) {
    gate_ctx.hedge_check = [this, &intent]() -> Result<ports::Ok> {
      return deps_.hedge_check(intent);
    };
  }
  gate_ctx.allowed_exchanges = options_.allowed_exchanges;
  gate_ctx.allowed_products = options_.allowed_products;
  gate_ctx.slice_mode = options_.slice_mode;

  auto gated = gate_.validate(gate_ctx);
  // Checked BEFORE the gate's own verdict and regardless of it: if the probe
  // could not answer, the entry is blocked whatever else the gate concluded, and
  // the caller hears the store's real Error rather than a fabricated
  // "already seen".
  if (probe_error) {
    return failed(stage::kDuplicateProbe, staged(stage::kDuplicateProbe, std::move(*probe_error)));
  }
  if (!gated) {
    return failed(stage::kGate, staged(stage::kGate, std::move(gated.error())));
  }
  outcome.gate_outcome = gated.value();

  // ── price-band (entry: BLOCK out of band) ────────────────────────────────
  // The intent-driven overload (IMP-11) reads the REAL (limit, trigger) pair off
  // the intent instead of a synthesized one — this is the production caller that
  // overload was missing. `check_price_band` and `require_band_ok` are both pure,
  // so evaluating both (evidence + typed error) costs nothing and avoids forking
  // the error text.
  outcome.band_checked = true;
  outcome.band = priceband::check_price_band(intent, ctx.band, /*is_exit=*/false);
  outcome.band_unvalidated = outcome.band.verdict == priceband::BandVerdict::BandUnknown;
  if (outcome.band.blocked) {
    auto band_ok = priceband::require_band_ok(intent, ctx.band, /*is_exit=*/false);
    if (!band_ok) {
      return failed(stage::kPriceBand, staged(stage::kPriceBand, std::move(band_ok.error())));
    }
    // Unreachable today: both calls derive from the same branch. If the two ever
    // diverge, BLOCK — a refused entry is the safe direction, and a blocked
    // verdict that silently passed would be the worst possible reconciliation.
    return failed(
        stage::kPriceBand,
        stage_error(stage::kPriceBand, ErrorCategory::Validation, SuggestedAction::DoNotRetry,
                    "entry is out of band (verdict " +
                        std::string(priceband::to_string(outcome.band.verdict)) + ")"));
  }
  // The configurable half of the band posture: priceband deliberately lets an
  // UNKNOWN band through (a feed outage must not freeze trading), which is right
  // for a liquid book and wrong for one where out-of-band rejection is the norm.
  // ENTRIES ONLY — the exit chain never consults this.
  if (options_.block_entry_on_unknown_band && outcome.band_unvalidated) {
    return failed(
        stage::kPriceBand,
        stage_error(stage::kPriceBand, ErrorCategory::DataStale, SuggestedAction::BlockStrategy,
                    "the circuit/LPP band is unavailable or malformed and this engine is "
                    "configured to refuse UNVALIDATED entry prices "
                    "(EngineOptions::block_entry_on_unknown_band)"));
  }

  // ── margin (buffered requirement vs a FRESH FundsView, fail-closed) ──────
  // The deployable-funds figure comes from the funds view, NOT from whatever the
  // margin source reported: a stale second opinion about available funds is
  // exactly the input this library refuses to act on. An unrefreshable view is a
  // DataStale/BlockStrategy failure here, which dominates even a zero
  // requirement.
  auto funds = deps_.funds_view->ensure_fresh();
  if (!funds) {
    return failed(stage::kMargin, staged(stage::kMargin, std::move(funds.error())));
  }
  margin.available = funds.value().available_margin;
  outcome.margin_checked = true;
  outcome.margin = marginsafety::evaluate_margin(margin, options_.margin_config);
  if (auto margin_ok = marginsafety::require_margin_ok(outcome.margin); !margin_ok) {
    return failed(stage::kMargin, staged(stage::kMargin, std::move(margin_ok.error())));
  }

  return passed(stage::kMargin, std::move(outcome));
}

// ═══════════════════════════════════════════════════════════════════════════
// AC-3 — preflight_exit / the shared exit-legal chain.
// ═══════════════════════════════════════════════════════════════════════════
StageResult EngineAssembly::preflight_exit(const domain::OrderIntent& intent,
                                           const PreflightInputs& ctx) const {
  // ── exit-class ───────────────────────────────────────────────────────────
  // Calling this function IS the claim that the intent reduces exposure, and the
  // claim buys the entry-only exemptions below. It runs first because if the
  // claim is false, nothing downstream is meaningful — every exemption the chain
  // grants was granted on its strength.
  //
  // Optional by necessity: only the caller's position book can answer, and a book
  // that cannot answer honestly should not be forced to. Absent => the claim is a
  // documented caller assertion (see the header's trust-boundary section).
  if (deps_.is_reducing && !deps_.is_reducing(intent)) {
    return failed(
        stage::kExitClass,
        stage_error(stage::kExitClass, ErrorCategory::Validation, SuggestedAction::DoNotRetry,
                    "preflight_exit was called for an intent that does NOT reduce "
                    "exposure. The exit chain waives the duplicate, UNKNOWN-pause, "
                    "entry-cutoff, funds and margin stages and clamps instead of "
                    "blocking out of band; an opening order must go through "
                    "preflight_entry"));
  }
  return exit_chain(intent, ctx, PreflightOutcome{});
}

StageResult EngineAssembly::exit_chain(const domain::OrderIntent& intent,
                                       const PreflightInputs& ctx, PreflightOutcome outcome) const {
  // NOTE THE ABSENCES, they are the design: there is no engine-mode refusal (an
  // ExitOnly assembly exists precisely to run this), no duplicate probe, no
  // UNKNOWN-pause, no entry cutoff, no funds check, no margin stage and no
  // risk-limits declaration anywhere in this function. The exit chain is not the
  // entry chain with flags turned off — the entry-only stages are not present to
  // be turned back on.

  // ── posture ──────────────────────────────────────────────────────────────
  // Open for Normal/BlockEntries/ExitOnly/SoftKill; CLOSED only under Panic,
  // where modes' documented contract routes the square-off to the emergency
  // engine OUT OF BAND. Using the posture (which already folds in the kill
  // switch as its floor) keeps that a single authority rather than a second
  // KillState read that could disagree. Scoped to the strategy for the same
  // isolation reason as the entry path — though note that the only posture that
  // closes this gate, Panic, is broad and blocks every strategy anyway.
  const modes::Posture posture = evaluate_posture_for_strategy(intent.strategy);
  if (!modes::PostureCoordinator::allows_risk_reducing_exits(posture)) {
    return failed(
        stage::kPosture,
        stage_error(stage::kPosture, ErrorCategory::RiskRejected, SuggestedAction::SquareOff,
                    "posture " + std::string(modes::to_string(posture)) +
                        " closes the normal exit gate; the emergency engine drives the "
                        "cancel + square-off OUT OF BAND"));
  }

  // ── session ──────────────────────────────────────────────────────────────
  // This NEVER blocks: sessionguard always permits an Exit, even under Failed,
  // because freezing an exit behind a dead token would TRAP an open position —
  // the single most dangerous outcome that module exists to prevent. It is called
  // anyway, for three reasons: it gives the invariant a production caller, it
  // surfaces `alert` so a degraded session is still visible on the exit path, and
  // if a future edit ever made an exit blockable the failure shows up here,
  // named, instead of as a silently stranded position.
  const SessionSnapshot snapshot = deps_.session();
  const sessionguard::SessionPosture session_posture =
      sessionguard::assess_session(snapshot.state, snapshot.last_broker_error);
  outcome.session_alert = session_posture.alert;
  if (auto op_allowed =
          sessionguard::require_op_allowed(session_posture, sessionguard::OpClass::Exit);
      !op_allowed) {
    return failed(stage::kSession, staged(stage::kSession, std::move(op_allowed.error())));
  }

  // ── instrument ───────────────────────────────────────────────────────────
  // Enforced on the exit path exactly as on the entry path. It is arguably MORE
  // important here: an exit checked against the wrong contract is a protective
  // leg whose lot, tick and band were all validated against something else.
  if (intent.symbol != ctx.instrument.symbol) {
    return failed(
        stage::kInstrument,
        stage_error(stage::kInstrument, ErrorCategory::Validation, SuggestedAction::DoNotRetry,
                    "resolved instrument '" + ctx.instrument.symbol +
                        "' does not match the intent's symbol '" + intent.symbol +
                        "' — lot, tick, freeze and band would all be checked against the "
                        "wrong contract"));
  }

  // ── gate, as a risk-reducing op ──────────────────────────────────────────
  risk::GateContext gate_ctx{.intent = intent, .instrument = ctx.instrument};
  gate_ctx.is_risk_reducing = true;
  // kill_entry_block / unknown_pause_active / is_duplicate / calendar /
  // funds_check are LEFT UNSET. The gate already exempts a risk-reducing op from
  // every one of them; not wiring them is belt AND braces, and it means an
  // ExitOnly assembly (which has no calendar or funds view at all) runs the exact
  // same code path as an EntryCapable one.
  if (options_.risk_gates_exits) {
    // make_engine refuses risk_gates_exits with a null risk_engine, so this
    // pointer is non-null by construction whenever the flag is set.
    risk::RiskState state = ctx.risk_state;
    state.is_entry = false;
    state.order_value_known = state.order_value_known && state.order_value_paise > 0;
    gate_ctx.risk_check = risk::make_risk_check(*deps_.risk_engine, intent, ctx.risk_limits, state);
  }
  if (deps_.hedge_check) {
    // The hedge check DOES apply to exits (the gate's own semantics): unwinding
    // one leg of a spread is exactly how a book becomes momentarily naked.
    gate_ctx.hedge_check = [this, &intent]() -> Result<ports::Ok> {
      return deps_.hedge_check(intent);
    };
  }
  gate_ctx.allowed_exchanges = options_.allowed_exchanges;
  gate_ctx.allowed_products = options_.allowed_products;
  gate_ctx.slice_mode = options_.slice_mode;

  auto gated = gate_.validate(gate_ctx);
  if (!gated) {
    // Shape / tick / lot / exchange / product / freeze still bite here, and they
    // should: a malformed protective stop is not protection, it is a broker
    // rejection at the worst possible moment.
    return failed(stage::kGate, staged(stage::kGate, std::move(gated.error())));
  }
  outcome.gate_outcome = gated.value();

  // ── price-band: CLAMP, never block ───────────────────────────────────────
  // `BandCheckResult::blocked` is deliberately NOT consulted on this path, and
  // neither is `block_entry_on_unknown_band`. For an exit priceband already
  // guarantees blocked == false, and the exit chain does not read the field at
  // all so that no future change to that module — or to this engine's options —
  // can start blocking protective legs through this door. The caller applies the
  // suggestions with `apply_clamp()`.
  outcome.band_checked = true;
  outcome.band = priceband::check_price_band(intent, ctx.band, /*is_exit=*/true);
  outcome.band_unvalidated = outcome.band.verdict == priceband::BandVerdict::BandUnknown;
  outcome.exit_clamped = outcome.band.has_suggestion || outcome.band.has_trigger_suggestion;

  return passed(stage::kPriceBand, std::move(outcome));
}

// ═══════════════════════════════════════════════════════════════════════════
// AC-3 — preflight_modify.
// ═══════════════════════════════════════════════════════════════════════════
StageResult EngineAssembly::preflight_modify(const domain::OrderIntent& current,
                                             const domain::OrderIntent& amended,
                                             const ModifyContext& modify_ctx,
                                             const PreflightInputs& ctx) const {
  PreflightOutcome outcome;

  // The request is DIFFED from the two intents (IMP-11's make_modify_request),
  // never hand-asserted. This is that helper's production caller, and it is what
  // stops a caller who moved only a stop's TRIGGER from presenting a "modify that
  // changes nothing" and having the guard Allow it on an order it should refuse.
  const modifyguard::ModifyRequest request = modifyguard::make_modify_request(current, amended);
  outcome.modify_checked = true;
  outcome.modify = modifyguard::evaluate_modify(modify_ctx.current_state, request,
                                                modify_ctx.observed_filled_qty);
  if (!outcome.modify.allowed) {
    const ModifyRejection mapping = rejection_for(outcome.modify.verdict);
    return failed(stage::kModifyGuard,
                  stage_error(stage::kModifyGuard, mapping.category, mapping.action,
                              std::string(modifyguard::to_string(outcome.modify.verdict)) + ": " +
                                  outcome.modify.detail));
  }

  // A permitted modify still has to be a well-formed, in-band order. It runs the
  // exit-legal chain on the AMENDED intent: re-pricing or re-sizing something
  // already working is not new risk, so it must not meet an entry-only block —
  // but it must absolutely still meet the shape, tick and band checks. The
  // `exit-class` check is skipped here by design (see the header): modifying a
  // working ENTRY order is legitimate and is not a risk-reducing act.
  return exit_chain(amended, ctx, std::move(outcome));
}

}  // namespace broker_exec::composition
