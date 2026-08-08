# IMP-12: Runtime gate-composition root (fail-closed engine assembly)

Status: ready-for-dev
Closes the 2-8 follow-up ("gate treats ABSENT funds_check/null calendar as PASS;
runtime MUST wire them for entries — enforce at composition") and the 6-3 review
LOW ("priceband/modifyguard intent overloads have zero production callers").

## Problem

The ValidationGate accepts injectable predicates; absent ones PASS. Ten hardening
modules (priceband, modifyguard, sessionguard, marginsafety, fillnorm, feedsub,
protection, brokerreason, endpoint_limiter, ledger checkpoint) plus FundsView,
RiskEngine, TradingCalendar, KillState, PostureCoordinator all exist as tested
islands with NO production composition path. Nothing today prevents an operator
binary from assembling an entry-capable engine with the funds gate absent.

## Acceptance Criteria

1. **EngineAssembly (extend broker_exec::composition)**: `make_engine(EngineDeps)
   -> Result<EngineAssembly>` producing a fully-wired entry pipeline:
   GateContext with REAL funds_check (FundsView::make_funds_check), REAL
   calendar, REAL risk_check (RiskEngine::make_risk_check), kill/pause predicates
   (KillState + UnknownPause flag source), duplicate check (idempotency reserve
   probe), hedge check where configured.
2. **Fail-closed builder**: an entry-capable assembly REFUSES to build when
   funds_view, calendar, risk_engine, kill_state, or posture source is absent
   (typed Validation error naming the missing piece). An explicitly EXIT-ONLY
   assembly (mode/flag) may build without funds/risk but still requires
   kill/posture. No default that silently passes.
3. **Pre-flight chain**: `preflight_entry(intent, ctx)` runs, in order:
   posture allows_entries -> sessionguard entry_allowed -> ValidationGate.validate
   -> priceband check_price_band(intent) (block entry out-of-band) ->
   marginsafety buffered requirement vs FundsView (fail-closed) -> ok.
   `preflight_exit(intent, ctx)` runs the exit-legal subset (never blocked by
   entry-only checks; priceband CLAMPS instead of blocks; modifyguard consulted
   for modifies via `preflight_modify(current, amended)`). Each stage failure
   names the stage. This gives the 6-3 LOW its production callers.
4. **Posture integration**: assembly exposes `evaluate_posture(signals)` via
   PostureCoordinator with the kill-switch operator floor wired (KillState ->
   operator_floor), and preflight_entry consults it FIRST.
5. **Tests**: builder refusal matrix (each missing piece named; exit-only mode
   builds without funds/risk but not without kill/posture); preflight_entry
   stage-order proof (each stage can individually block and names itself; a
   passing intent traverses all); preflight_exit never blocked by entry-only
   stages but still shape/band-clamped; preflight_modify delegates to
   modifyguard; posture floor from kill switch blocks entries end-to-end.
   Integration smoke: assemble with real modules (fake clock/fetch seams) and
   run one entry intent end-to-end through gate+band+margin.

## Notes for dev

- Pure decision-core composition: NO threads, NO sockets, NO broker I/O in this
  module; everything over the existing seams. The dispatcher/mainloop stays
  future work (that is the ONLY remaining unwired layer after this).
- Reuse existing make_funds_check/make_risk_check factories — do not fork logic.
- Do not modify ValidationGate semantics; this story WIRES it.
- fillnorm/feedsub/protection/brokerreason/endpoint_limiter are reconcile/stream
  -side guards — OUT of scope here (they wire at the dispatcher/reconcile layer);
  state this explicitly in the header so the boundary is recorded.
