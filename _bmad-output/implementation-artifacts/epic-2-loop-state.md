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
- [ ] 2-14 Kite adapter conformance + live-min-qty smoke
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