# Epic 2 Autonomous Implement+Review Loop — State

> **This file is the durable source of truth for the autonomous loop.**
> On any resume / context-compaction, RE-READ this file first to learn where the loop is.

## Mandate

- User asked (2026-06-27 ~23:55 IST): run an autonomous **implement + review loop using BMAD**,
  full-auto over the whole of **Epic 2** (then continue into later epics if time remains),
  for the next **~7 hours**, then stop.
- **DEADLINE (hard stop): 2026-06-28 06:55 IST** (epoch `1782609932`).
  Before starting each new story, check `date +%s` < `1782609932`. If past, STOP the loop,
  commit/push what is done, write a final summary, and do NOT schedule another wakeup.

## Branch / git

- Working branch: `epic-2-live-kite-trading` (off `main`).
- Commit per story. Push at sensible checkpoints (end of run, or every few stories).

## Per-story loop procedure (BMAD cadence)

For the next `backlog` story in `sprint-status.yaml` (top-to-bottom order):

1. **create-story**: write `_bmad-output/implementation-artifacts/<key>.md` from `template.md` +
   the epic ACs (epics.md) + architecture.md. Set status `ready-for-dev`; flip sprint-status to `ready-for-dev`.
2. **dev-story**: implement code + Catch2 tests + module `CMakeLists.txt`, wire into root `CMakeLists.txt`
   and `conanfile.py` (add deps), following `docs/conventions.md` + architecture exactly.
   (Delegated to a dev subagent to keep main context lean.)
3. **build+test**: main thread owns the build (shared `build/` dir caches Conan deps across stories):
   `conan install . -of build --build=missing -s compiler.cppstd=20 -s build_type=Release` (only if deps changed),
   then `cmake -B build -S . -G "Visual Studio 17 2022" -DCMAKE_TOOLCHAIN_FILE=build/build/generators/conan_toolchain.cmake`,
   `cmake --build build --config Release`, `ctest --test-dir build -C Release --output-on-failure`.
   Fix failures (inline or fix subagent) until green.
4. **review** (BMAD adversarial): reviewer subagent over the story diff — severity-tagged findings.
   Apply Critical/High fixes; rebuild+retest.
5. **commit**: `git add -A && git commit` (normal message, no caveman). Mark sprint-status `done`.
6. **update THIS file**: tick the story below, append a one-line outcome. Then next story.

## Conventions (binding — see docs/conventions.md)

- Namespaces `broker_exec::<module>`; headers `include/broker_exec/<module>/*.hpp`; src `src/<module>/*.cpp`.
- One CMake target per module `broker_exec_<module>` + alias `broker_exec::<module>`; link
  `broker_exec_warnings` + `broker_exec_sanitizers` PRIVATE. C++20, no extensions.
- No `#ifdef _WIN32`/POSIX outside `src/platform/`. No `double`/`float` in money paths (use Money/Price).
- Fallible internal calls return `std::expected<T,Error>` (see `include/broker_exec/expected.hpp`,
  `errors/error.hpp`); never throw across strategy boundary.
- Each module: `broker_exec_<module>_tests` (Catch2::Catch2WithMain) under `if(BROKER_EXEC_BUILD_TESTS)` + `add_test`.
- `domain`/`ports` never link `adapters`. Secrets only via env/SecretProvider, never TOML/logs/intent-log/ledger.

## Story queue (Epic 2) — tick as done

- [x] 2-1  Typed configuration system (toml++ + env, fail-fast, secrets-not-in-toml) — DONE (fb8097a)
- [x] 2-2  Secret provider, token encryption (OpenSSL AES-256-GCM), redaction scrubber — DONE (3420fbf)
- [x] 2-3  Kite Connect REST client (cpr/libcurl) behind adapter port — DONE
- [x] 2-4  Kite daily session establishment + expiry detection — DONE
- [x] 2-5  Capability model + early rejection — DONE
- [x] 2-6  Instrument-master lifecycle (daily refresh, staleness gate) — DONE
- [x] 2-7  Trading-calendar lifecycle — DONE
- [x] 2-8  Pre-submission validation gate — DONE
- [x] 2-9  Freeze-quantity slicer — DONE
- [x] 2-10 Four-level risk engine — DONE
- [x] 2-11 Funds/margin view + cadence + fail-closed gate — DONE
- [x] 2-12 Rate limiter + reserved exit lane — DONE
- [x] 2-13 Safe-start cold-boot gate — DONE
- [x] 2-14 Kite adapter conformance + live-min-qty smoke — DONE
- (then Epic 3+ if time remains)

## Outcome log (append one line per completed story)

- (baseline) Epic-1 tree builds green; 16 ctests pass (Release/MSVC). Conan deps OK.
- 2-1 DONE (fb8097a): broker_exec::config, toml++ defaults->file->env, fail-closed Result<Config>,
  recursive secret-key denylist, env seam. Review verdict SHIP; applied denylist broaden + shape tests
  + all-fields env-wins test + dropped vestigial domain link. 17/17 ctests green.
- 2-2 DONE (3420fbf): domain::scrub redaction (pure), EnvSecretProvider, OpenSSL AES-256-GCM TokenStore
  (fail-closed on tamper, 0600/0700 via new platform perms seam). Review SHIP (crypto correct); applied
  6 fixes (perms-fail surfaces Error, whole-word auth needles, cleanse bad key, perms test, dropped dead link);
  reverted an over-aggressive all-letter redaction rule (broke long-word prose) — under-reach documented as accepted.
  openssl/3.2.1 wired. 18/18 ctests green.
- NOTE for Epic 4 (provenance): scrub()'s >=20 alnum rule will redact client_ref/order_id in free-form logs;
  route provenance IDs through structured (non-scrubbed) fields, scrub only free text.
- 2-3 DONE: adapters/kite — HttpClient seam + CprHttpClient (cpr confined to .cpp), KiteRestClient
  (Kite v3 envelope, Authorization built only at call site, never logged), error mapping to taxonomy
  (scrubbed). Review FIX-REQUIRED: fixed NetworkException->ReconcileFirst (was DoNotRetry, safety bug),
  nlohmann PUBLIC, scrub error_type into broker_code; added 5xx/NetworkException/Margin tests. cpr/1.10.5.
  Also fixed: cpr ErrorCode enum names (HOST/PROXY_RESOLUTION_FAILURE) + moved cpr_http_client.hpp to include/.
  19/19 ctests green.

- 2-4 DONE: broker_exec::session — KiteSessionEstablisher.establish() (SHA-256 checksum via OpenSSL, request_token->access_token, persisted encrypted via TokenStore), validate() (loads token from store, 401/TokenException->NeedsReauth state, 5xx->reconcile Error), kSupportsHeadlessSessionRefresh=false, needs_reauth_error(). No auto-refresh. Review SHIP (no leak, checksum independently verified); added validate-5xx + missing/empty-access_token tests. No new dep. 20/20 ctests green.
- 2-5 DONE: broker_exec::capabilities — Capability enum + tri-state Support{Unknown,Supported,Unsupported} (Unknown==0 so zero-init is fail-closed), CapabilitySet.supports()/require()/require_all() (NotSupported+DoNotRetry, names the cap), kite_capabilities() (lifecycle Supported, HeadlessSessionRefresh Unsupported, unverified Unknown). Review SHIP; applied fail-closed enum reorder. Fixed nested-Builder incomplete-type compile error. No new dep. 21/21 green.
- 2-6 DONE: broker_exec::refdata::InstrumentMaster — header-mapped Kite CSV parse (no float; decimal->paise), date-versioned cache, resolve() (unknown/expired->Validation), require_fresh() (DataStale+BlockStrategy safe-start gate), injected fetch_csv seam (transport-free) + ClockPort. Review SHIP; applied 3 fixes (no-fresh-on-persist-fail, reject non-positive lot/tick, expiry==today boundary test). No new dep. 22/22 green.
- 2-7 DONE: broker_exec::refdata::TradingCalendar — JSON holidays/special-sessions/windows, UTC->IST(+330) local date+minute via std::chrono, is_trading_day (special overrides weekend+holiday), half-open entry/square-off/ session windows, require_entry_allowed->MarketClosed, require_fresh->DataStale+BlockStrategy, date-versioned cache (no-fresh-on-persist-fail). Review SHIP; applied ISO-date validation of holiday entries + exact window-boundary tests. No new dep (nlohmann existing). 22/22 green.
- 2-8 DONE: broker_exec::risk::ValidationGate — non-bypassable ordered pipeline (kill-switch, UNKNOWN-pause, duplicate, exchange, product, lot, tick, freeze, time-window, funds, risk, hedge); exits exempt from entry-only blocks (kill/pause/dup/window/funds) but still run exchange/product/lot/tick/freeze/risk/hedge; over-freeze->AllowWithSlicing (slice-mode) without short-circuiting later checks; first-failure names the check; real refdata calendar + injected predicates for funds/risk/hedge/dup/kill/pause (real impls in 2.9/2.10/2.11/3.8). Review FIX-REQUIRED: fixed StopLossMarket trigger now tick-checked (HIGH bypass), kill/pause action=BlockStrategy, dropped dead capabilities link, added SL-M/SL/empty-exchange tests. 23/23 green.
  FOLLOW-UP (gate composition, for runtime/2.11/2.13): the gate treats an ABSENT funds_check / null calendar as PASS (injectable default); the runtime MUST wire funds_check + calendar for entries — enforce that invariant at composition.
- 2-9 DONE: broker_exec::slicing::FreezeSlicer — pure deterministic fan-out of an over-freeze OrderIntent into children (chunk=(freeze/lot)*lot; full=qty/chunk; rem lot-aligned; sum==qty invariant), each with inline <parent>#<k> ref (k from 1, parity-tested vs idempotency::child_ref). qty<=freeze -> passthrough {parent}. Production target links domain+errors only (no idempotency/SQLite). Review SHIP (arithmetic verified); added boundary tests (qty==freeze, qty==freeze+lot, lot=0 mod-guard, freeze=0, 100000->56 children). 24/24 green.
- 2-10 DONE: broker_exec::risk::RiskEngine — four independently-callable levels (account: daily-loss, max-open-positions[entries-only], max-margin; strategy: stopped-flag[AC-3], daily-loss, max-lots; instrument: max-lots, illiquid/stale; order: market-block, max-value, slippage). 0/false=off (never blocks), loss=negative-pnl signs verified, fail-closed on unknown order value when margin/value limit armed, check_all ordering account>strategy>instrument>order. make_risk_check() -> std::function for the 2.8 gate (self-contained, engine stateless). Review SHIP; fixed slippage doc, engine-by-value capture, added equal-limit boundary + precedence tests. 24/24 green.
- 2-11 DONE: broker_exec::risk::FundsView — caches ports::FundsSnapshot + steady fetched_at; refresh() (no stamp-advance on fetch failure), is_fresh (steady age<=cadence, monotonic), invalidate() (after-fill), ensure_fresh (auto-refresh-if-stale, fail-closed DataStale, NEVER returns a stale snapshot), check_margin (fail-closed dominates even required==0; InsufficientFunds on shortfall), make_funds_check()->std::function for the 2.8 gate. Review SHIP (anti-over-leverage crux verified); added refresh-uses-new-value test, threaded root cause into DataStale, move nit. 24/24 green.
  This satisfies the 2-8 follow-up: the runtime wires FundsView::make_funds_check as GateContext.funds_check for entries.
- 2-12 DONE: broker_exec::ratelimit::RateLimiter — token bucket over ClockPort.now_steady (integer chrono, carry => no drift, capped at capacity), reserved exit lane (entry floors at reserved_exit, exit floors at 0 and may draw the reserved pool), acquire()->RateLimited+RetrySafe on deny (slow+alert signal). Admission-only (per-send timeout =>UNKNOWN is the 1.9 dispatcher). Review SHIP; reserved lane inviolable + no-drift verified across config edges. 25/25 green.

=== Epic 2: 12/14 done. Remaining: 2-13 safe-start cold-boot gate (composes session+reconcile+clock+config+instrument master+calendar+egress-IP+crypto-keys, fail-closed), 2-14 Kite adapter conformance + live-min-qty smoke. ===
- 2-13 DONE: broker_exec::session::SafeStartGate — all-8-required fail-closed cold-boot gate (config->crypto-keys->clock->session->egress-IP->instrument-master->calendar->reconciliation), injected std::function per check; an UNSET check is a hard fail (Internal+BlockStrategy 'not configured'); a failing check is wrapped name-prefixed with inner category/action preserved (DataStale/SessionExpired survive); session_state_to_result helper. Review SHIP (all safety axes verified: empty-fail-closed for all 8, all-required, inner-category preserved, no-throw); fixed doc nits + improved session-wiring guidance. 25/25 green.
- 2-14 DONE: KiteBrokerAdapter:ports::BrokerPort over KiteRestClient (tag+order_id->client_ref correlation maps so fetch_orders recovers the signal even on ack-loss; Kite status->OrderState, integer paise, no-throw/scrubbed). RecordedKiteServer (stateful Kite-HTTP twin of FakeBroker) + the SAME conformance kit, reused verbatim -> the Kite adapter passes the full fault matrix with ZERO duplicates (tier-1 cert). Review FIX-REQUIRED: added an explicit ack-lost recovery assertion (closes the vacuous-pass hole) + a no-blind-retry place_count probe + documented tier-2 limitations (square_off flatten, exchange-from-instrument-master, SL trigger/limit). docs/kite-min-qty-smoke.md runbook. 26/26 ctests green.

========================= EPIC 2 COMPLETE (14/14) =========================
All Epic-2 stories done, each via full BMAD cadence (create->dev subagent->MSVC build+ctest->adversarial review subagent->apply Critical/High+cheap-Medium fixes->commit). 26/26 ctests green on Release/MSVC. New deps wired: tomlplusplus, openssl, cpr/libcurl. Modules added: config, secrets(+platform perms), adapters/kite (REST client + BrokerPort adapter), session (establish+safe-start), capabilities, refdata (instrument master + trading calendar), risk (validation gate + 4-level engine + funds view), slicing, ratelimit. Notable review catches fixed: SL-Market trigger bypassing tick check (gate), fail-open capability enum default, NetworkException->do-not-retry (Kite), funds stale-but-served, instrument-master fresh-on-persist-fail, vacuous Kite conformance pass.
Tier-2 follow-ups (operator/live, tracked): live-SDK unknown resolution + VCR fixtures; square_off position-flatten; exchange via instrument master (not heuristic); SL distinct trigger/limit (needs OrderIntent change); runtime must wire funds_check+calendar for entries (2-8 follow-up); provenance IDs through non-scrubbed fields (2-2 note).
Next (Epic 3, if continued): 3-1 continuous reconciliation (fetch-off-loop/apply-on-loop, adaptive cadence).

=== EPIC 3 (Resilience) — in progress ===
- 3-1 DONE: broker_exec::reconcile — Reconciler.fetch (READS ONLY; holds only the clock, structurally no writer) -> immutable ReconcileResult(orders/trades/positions/funds + ordering_key + fetched_at); ReconcileApplier.apply (SOLE writer, advances local orders ONLY via lifecycle FSM apply -> forward-progressing/terminal-absorbing, no hand-rolled transitions); mismatch (phantom broker order / vanished ACKED non-terminal local) -> AlertSink + block_new_orders; adaptive next_cadence (tight 1.5s if in-flight/open-position, loose 20s when flat). Review FIX-REQUIRED: fixed pre-ack-order-absent false mismatch (HIGH place->reconcile race), stale-snapshot false-escalation (ordering_key high-water guard, apply() now non-const), empty-client_ref FSM-bucket collision; added acked-vanished + stale-snapshot + trades/positions tests. 27/27 green.
- 3-2 DONE: broker_exec::reconcile::ManualInterventionDetector — classifies PositionClosed/Reduced/OrderCancelled Manually (a believed-open position the broker shows flat/reduced, unexplained by a bot order, or a broker-acked non-terminal order vanished/cancelled); a close explained by a non-terminal OR Filled bot order on the symbol is NOT flagged (legit). reconcile_positions folds local positions to broker truth (manual-closed -> flat) so needs_exit()==false => the no-duplicate-exit guarantee is STRUCTURAL (AC-2). Alert+return-for-audit, no-throw. Review FIX-REQUIRED: fixed HIGH false-flag of a just-Filled bot exit as manual (Filled now suppresses; caller folds own fills first) + stopped auto-adopting a manually-OPENED broker position as exitable (bot won't square a human's position) + avg_price reset + sign-flip test. 27/27 green.
- 3-3 DONE: broker_exec::reconcile::CorporateActionClassifier — CorporateAction{Split/Bonus/SymbolChange/FnoAdjustment, qty_num/qty_den, new_symbol/new_token} + abstract CorporateActionSource port; classify(believed,broker_observed): value-preserving integer re-base (qty*num/den, price*den/num) applied ONLY when it EXACTLY matches broker truth (a non-matching CA is NOT force-applied -> caller falls through to manual); a NULL/absent source + a change is fail-visible (Error alert + source_missing, never silently a CA or manual). Review SHIP; applied malformed-ratio no-op guard + non-matching-CA zero-alert test. 27/27 green.
- 3-4 DONE: broker_exec::reconcile::RecoveryCoordinator — reconcile-before-resume 5-step machine (LoadState[replay seam] -> CheckSession -> FetchBroker[Reconciler.fetch] -> ResolveUnknowns[ReconcileApplier.apply] -> SafeOrBlocked). Issues ZERO broker mutations (no place/modify/cancel/square_off anywhere -> no duplicate, no auto-square-off). Double-fault (UNKNOWN order + broker unreachable) -> each Unknown set ManualInterventionRequired, Critical escalation alert, terminal, NO square-off. Resumes ONLY when load+session+broker+safe-start ok AND no open unknowns AND no reconcile mismatch. Review FIX-REQUIRED: fixed HIGH where a phantom broker order (block_new_orders) was ignored -> recover() wrongly ResumedSafe; now Blocked on any mismatch (+ mismatches field, clamp, outage+phantom tests). 27/27 green.
  Epic 3: 4/8 (3-1..3-4). Remaining: 3-5 market-data tick stream, 3-6 watchdogs, 3-7 posture coordinator, 3-8 kill switches.
- 3-5 DONE: broker_exec::marketdata::MarketDataView — tradability over an injected tick seam (no WS dep): state_for priority Disconnected>Unknown>Stale(steady age)>Delayed(wall lag vs exchange_ts)>Live; is_tradable/ require_tradable LIVE-ONLY (DataStale+BlockStrategy block for price-sensitive entries); de-dup/out-of-order (exchange_ts<=stored ignored, never restamps freshness); auth-aware handle_reconnect (AuthFailure->SessionExpired+ ReEstablishSession+alert+disconnect, NO retry; TransportFailure->disconnected+ok; Reconnected->connected). Review SHIP; applied fail-closed fixes (connected_ default false; TransportFailure disconnects) + delay-boundary/de-dup-no-restamp/ future-ts/wall-jump tests. IXWebSocket transport behind the seam = follow-up. 28/28 green.
- 3-6 DONE: broker_exec::health::Watchdog — detects MuteFeed/Disk/Memory/Handle Pressure/ClockSkew/ClockStall over injected WatchdogInputs (usage vs headroom ResourceLimits, connected-but-mute, clock skew/stall); any breach -> degrade_to_exit_only()=true (fail-closed) + per-breach AlertSink (Critical mute/clock, Error resource); 0=no-limit, strict-> boundary, negative-limit normalized to 0 (no fail-open). feed_connected_but_mute(view,sym) reuses 3.5 (Stale-only; Disconnected is a separate transport signal). Detect+signal only — posture mapping is 3.7. Review SHIP; applied negative-limit normalize + fixed a test (de-dup needs a strictly-newer exchange_ts after steady-only advance). 29/29 green.
- 3-7 DONE: broker_exec::modes::PostureCoordinator — the SINGLE posture authority. Posture total order Normal<BlockEntries<ExitOnly<SoftKill<Panic; posture_for maps each of 10 DetectorSignals to a min posture (stale/mute/unknown/mismatch/session->BlockEntries; broker-down/clock-skew/stall/resource->ExitOnly; risk->SoftKill; no detector->Panic); evaluate() returns the SEVEREST (max) over active signals + an operator_floor (kill switch sets Panic, 3-8). Gate helpers: allows_entries (Normal only), allows_risk_reducing_exits (all but Panic), require_entry_allowed (RiskRejected+BlockStrategy naming the posture); from_health maps watchdog signals; evaluate_and_alert (one alert, level by severity). Review SHIP; applied fail-severe fallback (SoftKill) + all-10-signals-never-Panic test. 30/30 green.
- 3-8 DONE: broker_exec::modes kill switches — KillType{Soft,Strategy,Broker,Account,Panic}, KillState (in-process flag set, sole writer = main loop; blocks_entries/blocks_strategy/allows_risk_reducing_exits[false under panic]/posture ->operator_floor for 3.7). KillController: authenticated control-plane (bad token -> Auth Error, nothing persisted/ enqueued), validate->authenticate->persist->enqueue (persist BEFORE ack, fail-closed: never ack an un-persisted kill), thread-safe queue handoff (single control thread documented), drain() on the loop, replay() restores kills after a crash (still-killed). Review SHIP; applied input validation (Strategy needs scope; Soft/Panic must not), persist-error passthrough, threading-contract docs. 30/30 green.

========================= EPIC 3 COMPLETE (8/8) =========================
Epic 3 (Resilience) done, full BMAD cadence each. New modules: reconcile (continuous reconciliation + manual-intervention + corporate-action + crash-recovery), marketdata (tradability states), health (watchdogs), modes (posture coordinator + kill switches). 30/30 ctests green (Release/MSVC); NO new Conan deps in Epic 3 (all over injected seams). Notable review catches fixed: reconcile false-block on the place->reconcile race; manual-intervention false-flagging a just-filled bot exit as human tampering; crash-recovery resuming despite a phantom order; market-data fail-open-before-connect; watchdog negative-limit fail-open; posture under-degrade fallback. Tier-2/runtime follow-ups still tracked above.
Totals: Epic 1 (12, pre-existing) + Epic 2 (14) + Epic 3 (8) = 34 stories; 30 ctest suites green. Next (Epic 4, if continued): 4-1 structured logging/provenance/audit.