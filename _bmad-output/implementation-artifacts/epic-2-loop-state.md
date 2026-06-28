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

=== EPIC 4 (Operator Visibility) — in progress ===
- 4-1 DONE: broker_exec::observability — versioned AuditEvent (schema_version=1, stable dotted EventType names, ISO-8601 UTC ts via std::chrono, typed provenance columns + namespaced fields) -> to_json_line (non-throwing UTF-8 dump). StructuredLogger binds domain::scrub over the WHOLE rendered line (no token-shaped string reaches any sink) + level mapping + ts stamp; render+scrub+emit fully inside a swallow-all try (logging never throws across the boundary, incl. invalid-UTF-8 broker payloads + a throwing/null sink). AuditTrail.decision_path reconstructs an order's path in order. spdlog/1.14.1 wired (REBUILT FROM SOURCE: the conancenter prebuilt referenced newer MSVC STL vectorization symbols -> LNK2019; --build=spdlog/* --build=fmt/* fixed it). Review FIX-REQUIRED: fixed HIGH UTF-8 dump-throw across the no-throw boundary + added throwing-sink/null-logger/invalid-UTF-8 tests + ports PUBLIC. 31/31 green.
- 4-2 DONE: broker_exec::observability::ReportGenerator — PURE/reproducible reports over a vector<AuditEvent>: DailyReport (counts + integer-paise realized_pnl [non-integer pnl ignored, no float] + per-strategy), ErrorReport (Error events in order, detail domain::scrub'd), ReconciliationReport (per-order intended/sent/confirmed/reconciled vs broker + discrepancies). Deterministic (std::map sort by client_ref -> byte-identical across input permutations). Review FIX-REQUIRED: fixed CRITICAL secret leak (in-memory AuditEvent.fields are RAW — scrub only ran on the logged line in 4.1 — so the error detail now scrubs itself) + HIGH (a broker-rejected order is a CONFIRMED verdict, not a silent gap: confirmed now includes reject/cancel; the not-reconciled discrepancy is fill-specific). Added redaction + rejected-order + permuted-input tests. 31/31 green.
- 4-3 DONE: broker_exec::alerting — MultiChannelAlertSink:ports::AlertSink (Telegram+webhook channels over an injected PostFn seam, NO network/dep): send() scrubs the message via domain::scrub BEFORE building any body (no token in any outbound payload), best-effort (ok if >=1 channel delivered, Error if all fail / no channels configured — never a silent success), beats the heartbeat on a delivered alert; send_test_alert() requires ALL channels. HeartbeatMonitor dead-man's-switch (steady-clock beat + is_alive(max_gap); flips false after the gap with no beat = the absence alarm an external watcher fires on). Review SHIP; added send_test_alert all-required/empty + heartbeat ==max_gap boundary + key=value redaction tests. No new dep (cpr HTTP channel behind the seam = follow-up). 32/32 green.
- 4-4 DONE: broker_exec::ledger::Ledger — SHA-256 hash chain (entry.hash=SHA256hex(prev_hash+scrubbed_payload), genesis '') + Ed25519 per-account signature, BOTH via the already-built OpenSSL EVP (NO libsodium / no new dep, RAII on every ctx). append() scrubs the payload before hash/persist + durable_sync (fsync); verify_chain() recomputes + link + seq, names the first bad seq (a non-key-holder edit breaks it); sign_head/verify_head fail-closed (wrong key/tampered head/bad-len sig -> Error); require_key_match fail-closed (safe-start blocker); EOD report (count+head+signature+pubkey, tamper-EVIDENT not legal proof) + write_public_key; position heartbeat (scrubbed). Review FIX-REQUIRED: fixed HIGH load() nlohmann type_error throw across the no-throw boundary on a wrong-field-type corrupt line (now type-checked -> Validation Error) + tolerate a torn trailing line (mid-file stays fatal) + tests. 33/33 green.
- 4-5 DONE: broker_exec::modes trading modes — TradingMode{Live,Paper,DryRun,Replay,MonitorOnly,ExitOnly,Emergency}; policy_for() single-source table (allows_entry/exit, live_execution, read_only, replay_clock); will_execute_live true ONLY for Live/ExitOnly/Emergency (Paper/DryRun/Replay/MonitorOnly non-live -> dry-run VALIDATES but never executes, AC-3); require_op_allowed (read-only modes block all; Emergency only Cancel/SquareOff; ExitOnly blocks Entry); uses_recorded_clock true only for Replay (AC-2); unmapped mode -> locked_down() fail-closed. Review SHIP; added fail-closed-fallback + will_execute_live-exact-set + to_string-stability tests + can_place_exit advisory doc. 33/33 green.
========================= LOOP ENDED — FINAL SUMMARY =========================
Stopped at ~06:09 IST, ~46 min before the 06:55 deadline. Deliberately did NOT start
4-6 (CLI + localhost health endpoint): it is the most integration-heavy remaining story
(CLI11 + cpp-httplib + a localhost socket server) and could not be finished + reviewed +
committed safely before the deadline — per the standing instruction, prefer a clean stop
over a half-done uncommitted story.

DELIVERED THIS SESSION (28 stories, each via full BMAD cadence: create-story -> dev subagent
-> real MSVC/Conan build + ctest -> adversarial review subagent -> apply Critical/High +
cheap-Medium fixes -> commit -> push):
  Epic 2 (Live Kite Trading): 14/14  DONE
  Epic 3 (Resilience):         8/8   DONE
  Epic 4 (Operator Visibility):5/6   (4-1..4-5 done; only 4-6 CLI/health left)
Project totals: Epic 1 (12, pre-existing) + Epic 2 (14) + Epic 3 (8) + Epic 4 (5) = 39 stories done.
33/33 ctest suites green on Release/MSVC. Branch epic-2-live-kite-trading pushed (HEAD eb12b6d).

NEW MODULES THIS SESSION: config, secrets(+platform perms), adapters/kite (REST client + BrokerPort
adapter), session (establish + safe-start), capabilities, refdata (instrument master + trading
calendar), risk (validation gate + 4-level engine + funds view), slicing, ratelimit, reconcile
(reconciliation + manual-intervention + corporate-action + crash-recovery), marketdata, health,
modes (posture + kill switches + trading modes), observability (logging + reports), alerting, ledger.
NEW CONAN DEPS WIRED: tomlplusplus, openssl, cpr/libcurl, spdlog (built from source).

NOTABLE REVIEW CATCHES FIXED (the adversarial review earned its keep — a real bug almost every story):
  - SL-Market trigger price bypassing the tick check (2-8 gate)
  - fail-open capability enum default (2-5); funds stale-but-served (2-11)
  - NetworkException->do-not-retry on a possibly-live order (2-3 kite)
  - vacuous Kite conformance pass (2-14); instrument-master fresh-on-persist-fail (2-6)
  - reconcile false-block on the place->reconcile race (3-1)
  - a just-filled bot exit mislabeled as human tampering (3-2)
  - crash-recovery resuming despite a phantom order (3-4)
  - market-data fail-open-before-connect (3-5); watchdog negative-limit fail-open (3-6)
  - CRITICAL secret leak: domain::scrub only ran on the 4-1 LOGGED line, never on in-memory
    AuditEvent.fields -> the 4-2 report rendering fields leaked tokens (fixed; lesson propagated:
    every consumer of fields / every outbound msg scrubs itself)
  - HIGH: 4-1 invalid-UTF-8 broker payload threw across the no-throw logging boundary
  - HIGH: 4-4 ledger load() type_error throw on a wrong-field-type corrupt line (the exact tamper path)

REMAINING (for a future loop): 4-6 CLI + localhost health endpoint (model the HealthSnapshot + CLI
dispatch over a seam to avoid a real socket in tests; CLI11 + cpp-httplib are header-only = fast install),
then Epic 4 done; Epic 5 (option-selling safety), Epic 6 (Kotak + multi-account). Plus the tier-2/runtime
follow-ups tracked earlier (Kite square_off position-flatten, exchange via instrument master, SL distinct
trigger/limit, runtime wiring of funds_check+calendar into the gate, IXWebSocket + cpr-alert + cpr-kite
transports behind their seams, ledger truncation-detection via a retained signed head).

========================= LOOP RESUMED (2026-06-28) =========================
User: "continue loop for next 2 hour". NEW hard deadline: epoch 1782625409
(2026-06-28 11:13 IST). Same per-story cadence + conventions as above.
Next backlog story: 4-6 (CLI + localhost health endpoint, FR-36).
Design choice (low-risk near deadline): model HealthSnapshot + CLI verb dispatch
over INJECTED SEAMS (std::function facade) so tests need NO real socket and NO
broker — CLI11 + cpp-httplib are header-only (fast Conan install). The endpoint
handler logic is tested as pure route->response functions; real socket bind +
runtime wiring of verbs to live modules is composition-root / tier-2.

- 4-6 DONE: broker_exec::cli (new module) — thin operator CLI + localhost health endpoint (FR-36).
  HealthSnapshot (immutable, integer/enum-only) + to_json (scrubbed outbound payload) + is_live/is_ready
  (readiness strictly stronger; fail-closed on negative budget/age). HealthState = mutex-guarded latest-snapshot
  publisher; EMPTY default fails closed (Failed session, INT64_MAX heartbeat, clock not sane) -> /healthz + /ready
  both 503 before the loop publishes. Pure route() (no socket in tests): GET /healthz->live, /ready->ready,
  anything else->clean 404, no throw. CLI: OperatorApi std::function seam, Verb enum, is_mutating true ONLY for Kill,
  dispatch fails closed on a null callback; run_cli over CLI11 (one subcommand/verb), every printed line scrubbed.
  New header-only Conan deps: cli11/2.4.2 + cpp-httplib/0.15.3 (CLI11::CLI11 + httplib::httplib, both PRIVATE-confined to .cpp).
  Review verdict SHIP (no Critical/High). Applied 3 fixes: (1) MEDIUM listen() now ENFORCES loopback-only host
  (reject 0.0.0.0/LAN -> fail closed, AC-3); (2) server set_error_handler -> JSON-404 parity with route(); (3) run_cli
  wraps dispatch in try/catch so a throwing external seam callback exits non-zero (scrubbed) instead of std::terminate.
  Orchestrator edits: root CMakeLists find_package(CLI11/httplib)+add_subdirectory(src/cli); conanfile two deps.
  34/34 ctests green (Release/MSVC). **Epic 4 COMPLETE (6/6).**

- 5-1 DONE: broker_exec::options::execute_hedge_first (new module) — never-naked hedge-first state machine (FR-16).
  Ordered fail-closed: place_hedge -> confirm_hedge(==true ONLY) -> place_short -> recheck_hedge_live. Short is sent ONLY
  on a placed-AND-confirmed hedge; a confirm Error/false/null-seam aborts BEFORE any short (AC-2, no naked window). Lone
  hedge (ShortPlacementFailed) is SAFE (no alert/no emergency). AC-3 (short live, hedge later unprovable: recheck false OR
  Error OR null): run the EMERGENCY ACTION FIRST then best-effort Critical alert. Over injected std::function seams +
  ports::AlertSink; no broker, no new dep. Review verdict SHIP (no Critical/High; never-naked invariant proven by tests).
  Applied MEDIUM hardening: emergency square-off now runs BEFORE the alert + alert send wrapped in try/catch, so a THROWING
  alert sink can never skip the square-off nor break the no-throw contract; added 3 tests (null-short-seam safe,
  emergency-returns-Error ran-but-failed, throwing-sink-still-squares-off). 35/35 ctests green. Epic 5 now 1/4.

- 5-2 DONE: broker_exec::options::execute_basket (basket.cpp in the options module) — multi-leg basket, no orphaned legs (FR-17).
  Pre-flight Kahn topological sort: empty/dup leg_id, unknown dep, ANY dependency cycle, or null place_leg seam => Blocked,
  NOTHING placed (fail-closed, never a half-basket). Dependency-honoring execution in topo order: a leg is placed ONLY if
  every prerequisite ended Executed, else SkippedUnmetDependency (place_leg NOT called) and skips propagate transitively
  (AC-1). Partial detected. AC-2 policy: UnwindExecuted cancels executed legs NEWEST-FIRST (a failed/null unwind leaves the
  leg Executed + escalates — never a silently-dropped live orphan) + Critical alert; LeaveAndAlert leaves them + Warning.
  AC-3 single-unit (basket_id + tracked_as_single_unit, default true). Over injected std::function seams + AlertSink; no
  broker, no new dep. Review verdict SHIP (no Critical/High/Medium; never-orphan invariant proven via placement/unwind logs).
  Applied LOW: the Critical alert now NAMES the still-LIVE un-unwound leg_ids (loudest channel reflects the worst state) +
  a test asserting it. 35/35 ctests green. Epic 5 now 2/4.

- 5-3 DONE: broker_exec::options::execute_sliced_leg (sliced_leg.cpp in the options module) — freeze-slicing at option size,
  no duplicates (FR-6/FR-17). REUSES the real slicing::FreezeSlicer for deterministic <parent>#<k> children (no reinvented
  ref logic). Places children k=1..N over injected seams: already_placed(ref) (idempotency) + place_child(intent)->(state,oid).
  AC-2 SIGKILL-no-duplicate: a child already_placed==true is AlreadyPlaced/deduped/NOT re-sent — replay re-emits identical
  refs and dedupes (no orphan/dup); proven via placement log (#1/#2 never re-sent, only #3 placed). AC-3 UNKNOWN-pause: a
  child Unknown OR a place Error OR an already_placed Error STOPS immediately (no blind retry — dispatch rule), paused_at_ref
  set, Critical "reconcile before resume" alert (best-effort swallow+try/catch); later children never placed. Fail-closed:
  null place_child or slicer Validation Error -> SliceRejected, nothing placed. Review verdict SHIP (no Critical/High/Medium;
  duplicate-on-replay/blind-retry/place-past-pause all disproven by trace + placement-log tests). Applied LOW doc fix (pacing
  lives in the place_child seam, not the loop). 35/35 ctests green. Epic 5 now 3/4.

- 5-4 DONE: broker_exec::options::evaluate_margin_shock (margin_shock.cpp) — capability-gated pre-trade SPAN shock sim (FR-18).
  Capability-gated FAIL-CLOSED: SPAN "available" ONLY when Support::Supported AND source non-null AND source returns a value;
  Unknown/Unsupported (tri-state default) / null / erroring source => UNAVAILABLE. AC-1: crossing test margin_under_shock >
  available (strict >, integer Money paise) => BlockedMarginShock pre-submission (+ defense-in-depth: also blocks if margin_now
  already over available). AC-2: unavailable + net-short => BlockedUnavailableNetShort (fail-closed, NO summed-legs fallback —
  there is no per-leg path in the seams); unavailable + not-net-short => AllowedUnavailableBounded. AC-3: seams.audit(result)
  on EVERY path (incl. blocked); null audit safe. blocked flag derived from outcome (is_blocking) so it can't desync. Reuses
  capabilities::Support + domain::Money; no new dep. Review verdict SHIP (no Critical/High/Medium; no fail-open, crossing test
  correct, net-short never approved without SPAN). Applied both LOWs (margin_now defense-in-depth + derived blocked) + a test.
  35/35 ctests green. **Epic 5 COMPLETE (4/4).**

- 6-4 DONE (OUT OF SPRINT ORDER, deliberate): broker_exec::isolation::StrategyBook (new module) — multi-strategy isolation
  (FR-32). Picked 6-4 over 6-1 because 6-1 (Kotak REST+WebSocket+multi-step auth) needs IXWebSocket (new dep + build) + a
  large auth spike — too heavy/risky near the 2h deadline; 6-4 is pure in-memory logic, no dep, independent of the Kotak
  transport chain. Per-strategy virtual book keyed by strategy_id: isolated per-symbol signed net qty + average-cost +
  realized P&L + tag + active flag. apply_fill touches ONLY the owning strategy (AC-1 isolation, proven byte-identical).
  Average-cost model with correct realized-P&L sign for long-close AND short-close + cross-zero reopen + flat reset.
  square_off(id, netting=false) flattens ONLY that strategy; netting=true nets across all strategies (AC-1). check_new_order
  blocks on COMBINED account exposure vs cap (AC-2 global limit; negative cap fail-closed, zero=no-limit, stopped-check
  first). stop/resume_strategy independent (AC-3, stopping A never touches B). Over a mark_price std::function seam; no I/O.
  Review verdict SHIP (no Critical/High; isolation/limit/stop/P&L-sign all hold). Applied MEDIUM (short-close P&L-sign test
  at non-zero value) + LOW (zero-qty fill no longer default-inserts a phantom record). 36/36 ctests green. Epic 6 now 1/5
  (6-1/6-2/6-3 Kotak transport chain + 6-5 supervisor remain — 6-1 needs IXWebSocket, a longer session).

- 6-5 PARTIAL (decision core only; story = in-progress): broker_exec::supervisor::SupervisorPolicy (new module) — the
  supervisor exit-code contract + restart-backoff + absence-alarm policy (FR-33 AC-3). exit_reason_from_code: 0->CleanShutdown,
  70->FailClosedNeedsHuman, ANY other (incl. signals/negative)->Crash (fail-safe unknown==Crash). decide(): CleanShutdown->
  NoRestartCleanShutdown (no alarm); FailClosedNeedsHuman->NoRestartEscalate + absence alarm (NEVER auto-restarts, any n);
  Crash->RestartWithBackoff (capped exponential) UNLESS consecutive_crashes > max_consecutive_restarts -> NoRestartEscalate +
  alarm (crash-loop circuit-breaker). Standalone (stdlib only, no deps). Review verdict SHIP (no Critical/High; fail-closed
  never restarts, breaker boundary correct, backoff floored positive). Applied MEDIUM: backoff_for now clamps on value>cap/2
  BEFORE doubling so value*=2 is overflow-free for ANY cap up to INT_MAX (the prior form could overflow to a negative/instant
  backoff near INT_MAX) + tests (near-INT_MAX cap, degenerate breaker). 37/37 ctests green.
  *** DEFERRED (6-5 remainder, needs a longer session): process-per-account spawn, systemd template, per-(broker,date) shared
  cache under a cross-process lock, SIGKILL-restart-from-intent-log. Story stays in-progress until that OS wiring lands. ***

============== 2-HOUR RESUMED LOOP COMPLETE (2026-06-28) ==============
Delivered this 2h window (6 stories, each full BMAD cadence build+adversarial-review+fix+commit+push, all green):
  4-6 (CLI + localhost health endpoint)  -> Epic 4 COMPLETE (6/6)
  5-1 hedge-first / never-naked
  5-2 basket / multi-leg (no orphans)
  5-3 freeze-slicing at option size (no duplicates)
  5-4 margin/SPAN shock sim (capability-gated)  -> Epic 5 COMPLETE (4/4)
  6-4 multi-strategy isolation
  6-5 supervisor decision core (PARTIAL — policy only)
Project status: Epics 1,2,3,4,5 COMPLETE; Epic 6 = 1 done (6-4) + 6-5 partial; 6-1/6-2/6-3 (Kotak REST+WebSocket adapter +
certification + portability proof — need IXWebSocket) and the 6-5 process wiring remain. 37/37 ctest suites green (Release/MSVC).
All on branch epic-2-live-kite-trading (not merged to main; no PR).

============== IMPROVEMENT LOOP (2026-06-28, ~3h) ==============
User: "start improvement loop for 3 hours heavy topics now, also check online developer
complaints and fix the issues they mention." DEADLINE: epoch 1782640218 (2026-06-28 15:20 IST).
Mode: HARDEN/IMPROVE existing modules (not just new stories). Same cadence: scope -> dev subagent
-> build+ctest -> adversarial review -> fix -> commit -> push. A background research subagent is
gathering REAL Kite Connect / Kotak Neo developer complaints -> map to library fixes.
Heavy-topic backlog (dep-free first): (1) ledger truncation-detection via retained signed head;
(2) SL distinct trigger/limit (OrderIntent trigger_price + gate); (3) runtime gate-composition
fail-closed when funds_check/calendar absent for entries; (4) Kite square_off position-flatten;
(5) complaint-driven fixes from the research findings.

--- IMPROVEMENT-LOOP PROGRESS (research-driven) ---
Research findings (Kite/Kotak dev complaints) -> mapped to fixes. Top money-loss gaps: write-path
duplicate-on-timeout (ALREADY COVERED by dispatcher 1.9 INDETERMINATE/Unknown + UnknownPause + no-blind-retry);
push-as-truth (covered by reconciliation); the REAL gaps -> implemented:
- IMP-1 DONE (2e23db6): ledger truncation/rollback detection via a KEY-PINNED signed checkpoint. Review caught a
  HIGH fail-open (unpinned key let a non-key-holder re-sign a truncated chain) -> fixed: verify_against_checkpoint
  takes a pinned out-of-band key + require_key_match before verify_head. + key-substitution & malformed-file tests.
- IMP-2 DONE (9e6ef05): broker_exec::brokerreason canonical rejection/status classifier (fixes brittle free-text
  matching; fail-closed unknown->DoNotRetry; timeout/5xx->Indeterminate/ReconcileFirst). Review caught a HIGH
  fail-open (bare "429" substring matched arbitrary ids -> a hard reject looked SafeToRetryReadOnly + alert
  suppressed) -> fixed: anchored 429/rate-limit phrasing only; dropped bare "oms"; documented read-only caller
  obligation; negative tests.
- IMP-3 IN PROGRESS: per-endpoint rate limiter (order/quote/historical/other buckets) + 429 circuit-breaker with
  backoff, preserving the reserved exit lane (research #4: avoid account-level RMS ban; exits never trapped).
- IMP-4 IN PROGRESS: library-owned protective-stop supervisor (research #3: never trust a broker GTT as a durable
  stop) — re-arms a band-aware protective exit when a stop fired-but-unfilled (LPP/circuit reject); fail-closed on
  unknown band; alert survives a throwing sink.
- NEXT CANDIDATES: IMP-5 marketdata staleness refinement (last_trade_time cadence; illiquid != mute, research #5/#8);
  wire brokerreason classifier into the dispatcher/reconcile error path; per-broker freeze-qty table source (research #9).

--- IMPROVEMENT-LOOP PROGRESS (cont.) ---
- IMP-3 DONE (9fff693): per-endpoint rate limiter + 429 circuit-breaker (research #4). Review caught a MEDIUM exit-trap
  (order_per_sec<=0 misconfig emptied the Order bucket -> every exit denied) -> floored Order capacity; +misconfig tests.
- IMP-4 DONE (abe259c): broker_exec::protection protective-stop supervisor (research #3 — never trust a GTT). Review caught
  a HIGH fail-open (a Filled order flag closed an exposed (qty!=0) position -> naked) -> live position_qty is now the sole
  source of truth (Closed only when flat); + INT64_MIN negation UB fix; + inverted-band guard.
- IMP-5 IN PROGRESS: modify-order safety guard (research #11 — a qty modify on a partially-filled order cancels the working
  remainder; raced-fill detection; price-only modify allowed; fail-closed verdicts).
- IMP-6 IN PROGRESS: pre-submission circuit/LPP price-band validator (research #12 — out-of-band orders exchange-rejected
  during volatility). Block an out-of-band ENTRY pre-submission; CLAMP an out-of-band protective EXIT (never block an exit).
Pattern holding: adversarial review caught a real HIGH/MEDIUM fail-open on every improvement so far (4/4).

--- IMPROVEMENT-LOOP PROGRESS (cont. 2) ---
- IMP-5 DONE (65dc171): broker_exec::modifyguard modify-order safety (research #11 — a qty modify cancels the working
  remainder). Review caught a HIGH fail-open (pre-ack/in-flight states Created/Validated/PendingSend/Sent fell through to
  Allow -> a qty modify on a stale filled_qty cancels the remainder) -> only Acknowledged/PartiallyFilled modifiable.
- IMP-6 DONE (d7b4fe1): broker_exec::priceband pre-submission circuit/LPP band validator (research #12). Block out-of-band
  ENTRY; CLAMP out-of-band EXIT (never block an exit). Review caught a MEDIUM (stop-limit exit clamped only the limit, left
  the trigger out-of-band) -> suggested_trigger clamps both.
- IMP-7 IN PROGRESS: mid-session re-auth guard (research #6) — reuses brokerreason classifier; a mid-session SessionExpired
  -> NeedsReauth + freeze entries (exits/reconcile reads still allowed) + alert; fail-closed default.
- IMP-8 IN PROGRESS: margin safety buffer (research #10) — fail-closed buffer over the broker margin API + worst-case
  (summed-leg) multi-leg margin when the leg benefit can't be trusted near the 9:20/expiry boundary; round-UP, overflow-guarded.
Tally: 6 improvements committed, adversarial review caught a real HIGH/MEDIUM fail-open on EVERY one (6/6). 41/41 ctest green.

--- IMPROVEMENT-LOOP PROGRESS (cont. 3) ---
- IMP-7 DONE (f336076): broker_exec::sessionguard mid-session re-auth guard (research #6). Reuses brokerreason classifier;
  SessionExpired -> NeedsReauth freeze entries (exits/reads always allowed) + alert. Review SHIP (first with no HIGH/MEDIUM);
  applied LOW defense-in-depth (Entry gated on state==Healthy too).
- IMP-8 DONE (2df1747): broker_exec::marginsafety margin safety buffer (research #10). Fail-closed over-estimate: worst-case
  (summed-leg) multi-leg margin near 9:20/expiry + round-UP bps buffer, overflow-saturating. Review SHIP; applied MEDIUM
  (benefit_trusted now defaults FALSE = fail-closed) + tests.
- IMP-9 IN PROGRESS: canonical fill normalizer (research #2/#4 — a partial fill arriving as a Kite UPDATE event misread as
  not-filled; pushes not authoritative). Quantity-first canonical (state, filled, pending); a push is never authoritative,
  exit only off a reconciled fill (exit_qty_for==0 for a push).
TALLY: 8 improvements committed, 43/43 ctest green. New modules this loop: brokerreason, ratelimit/endpoint_limiter,
protection, modifyguard, priceband, sessionguard, marginsafety (+ ledger checkpoint hardening). Adversarial review caught a
real fail-open/UB on 6 of 8 (the other 2 = cheap LOW/MEDIUM hardening).
