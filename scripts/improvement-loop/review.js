// ---------------------------------------------------------------------------
// Improvement loop, stages 1 and 2: parallel review, then adversarial refutation.
//
// Run with the Claude Code Workflow tool:
//   Workflow({ scriptPath: 'scripts/improvement-loop/review.js' })
//
// Returns { confirmed, rejected }. A finding reaches `confirmed` only if an
// independent reviewer, instructed to KILL it, failed to - and, for anything
// rated critical or high, only if a SECOND fully independent reviewer agreed.
// `rejected` keeps the reason each dead finding was killed, which is worth
// reading: it is where reviewer hallucinations and misreadings show up. Round 1
// rejected 38 of 72.
//
// Edit REVIEWERS to change the partitioning. The LENS matters more than the file
// list: two reviewers reading the same code with the same question find the same
// things. See docs/improvement-loop.md.
// ---------------------------------------------------------------------------

export const meta = {
  name: 'fitbro-improvement-loop-review',
  description: 'Round 1: parallel adversarial review of the broker-exec C++ safety library, verified findings only',
  phases: [
    { title: 'Review', detail: '14 reviewers over partitioned modules and cross-cutting lenses' },
    { title: 'Verify', detail: 'adversarial refutation of each finding, escalated for high severity' },
  ],
}

const FINDINGS_SCHEMA = {
  type: 'object',
  required: ['findings'],
  additionalProperties: false,
  properties: {
    findings: {
      type: 'array',
      maxItems: 6,
      items: {
        type: 'object',
        required: ['title', 'file', 'line', 'severity', 'area', 'problem', 'failure_scenario', 'evidence', 'fix'],
        additionalProperties: false,
        properties: {
          title: { type: 'string', maxLength: 80 },
          file: { type: 'string' },
          line: { type: 'integer' },
          severity: { type: 'string', enum: ['critical', 'high', 'medium', 'low'] },
          area: { type: 'string', enum: ['safety', 'concurrency', 'portability', 'persistence', 'adapters', 'tests', 'docs', 'correctness'] },
          problem: { type: 'string' },
          failure_scenario: { type: 'string' },
          evidence: { type: 'string' },
          fix: { type: 'string' },
        },
      },
    },
  },
}

const VERDICT_SCHEMA = {
  type: 'object',
  required: ['real', 'reasoning'],
  additionalProperties: false,
  properties: {
    real: { type: 'boolean' },
    reasoning: { type: 'string' },
    corrected_severity: { type: 'string', enum: ['critical', 'high', 'medium', 'low'] },
  },
}

const CONTEXT = `
PROJECT: the repository root — "broker_exec", a C++20 broker-neutral trading
EXECUTION AND SAFETY library for Indian brokers (Zerodha Kite, Kotak Neo). 63k LOC.
Headers in include/broker_exec/<module>/, sources in src/<module>/, tests are src/<module>/*_test.cpp (Catch2).
Build: CMake + Conan, MSVC/gcc/clang. All 51 ctest suites currently pass.

THE LIBRARY'S CORE PROMISE (violations of these are the highest-value findings):
- No duplicate orders, ever. Every intent has a client-side idempotency reference.
- Write-ahead intent log fsync'd to disk BEFORE the broker socket write; replayed on boot.
- UNKNOWN-first: an uncertain broker response pauses new risky entries until reconciled.
- Broker is the source of truth; local state is a working copy.
- Money is exact: fixed-point int64 paise. NO double/float anywhere in a money or price path.
- Fail-closed: on doubt, refuse the action rather than proceed.
- Errors use std::expected<T, Error>; never throw across the strategy-facing boundary.

BINDING CONVENTIONS (docs/conventions.md):
- No OS-specific API or #ifdef _WIN32 / POSIX includes outside src/platform/.
- std::filesystem for paths; never hand-built "/" or "\\\\" strings.
- Time only via the injected ClockPort (never call std::chrono::system_clock::now() directly
  outside src/clock/system_clock.cpp).
- Durability only via broker_exec::platform::durable_sync() — never raw fsync/FlushFileBuffers.
- domain and ports must never depend on adapters.
- The single broker-mutation path is dispatch(): record -> fsync -> send -> record.

WHAT COUNTS AS A FINDING — a defect a careful staff engineer would block a PR on:
  correctness bugs, integer overflow/truncation, unchecked error paths, TOCTOU, races,
  lifetime/dangling issues, resource leaks, missing fsync, SQL injection or unbound statements,
  unsafe parsing of broker JSON, silent swallowing of errors, a safety gate that can be bypassed,
  a fail-OPEN default where fail-closed is required, portability breaks, or a test that asserts
  something that cannot fail (fake-green coverage).
WHAT IS NOT A FINDING — do not report these, they will be rejected:
  naming/style/formatting, "add more comments", "consider using X instead of Y" with no defect,
  missing features that are simply not built yet, speculative "this might be slow",
  or anything you could not point at a specific line for.

RULES:
- You MUST actually read the files. Use Read and Grep. Do not guess from filenames.
- Every finding needs file, 1-indexed line, and a VERBATIM quoted code snippet in "evidence".
- Report at most 6 findings. Fewer, real findings beat many weak ones. Zero is a valid answer.
- Rank by severity: critical = can lose money / duplicate an order / corrupt durable state.
`

const REVIEWERS = [
  { key: 'idempotency-dispatch', scope: 'src/idempotency/, src/intentlog/, src/runtime/ (dispatcher, unknown_pause, unknown_resolver), and include/broker_exec/ counterparts',
    lens: 'DUPLICATE-ORDER SAFETY. Try to construct a sequence of crashes, retries, restarts and broker timeouts that produces two live orders from one intent, or that loses an intent that was already sent. Check the record->fsync->send->record ordering literally, the uniqueness and derivation of the client reference, the UUID source, replay-on-boot, and what happens when the log has a torn or partial trailing record.' },
  { key: 'durability-store', scope: 'src/store/, src/platform/ (durable, file_lock, permissions, process), src/intentlog/sha256.cpp',
    lens: 'CRASH DURABILITY AND PERSISTENCE. Check every SQLite call for unchecked return codes, unbound or string-concatenated SQL, missing transactions, missing WAL/synchronous pragmas, statement finalization on error paths, and file-descriptor leaks. Check that every durable write is followed by durable_sync of both the file AND its containing directory where the platform requires it. Check the file lock for TOCTOU and stale-lock handling.' },
  { key: 'risk-gates', scope: 'src/risk/ (risk_engine, validation_gate, funds_view), src/marginsafety/, src/priceband/',
    lens: 'FAIL-CLOSED RISK GATES. Hunt for any path where a check is skipped, a missing input is treated as "pass", a limit comparison uses the wrong strict/non-strict operator, an unsigned/signed mismatch flips a comparison, or an exception/error is swallowed such that the gate returns allow. Enumerate the boundary values for every numeric limit.' },
  { key: 'reconcile', scope: 'src/reconcile/ (reconciler, recovery, corporate_actions, manual_intervention)',
    lens: 'RECONCILIATION CORRECTNESS. The broker is the source of truth. Find cases where local state can silently win, where an UNKNOWN order is dropped from the set to reconcile, where a partial fill is double-counted, where quantity/price arithmetic can overflow or lose sign, and where a reconciliation failure leaves the engine in a state that permits new risky entries.' },
  { key: 'kite-adapter', scope: 'src/adapters/kite/ and include/broker_exec/adapters/kite/',
    lens: 'BROKER TRANSPORT ROBUSTNESS. Assume the broker returns malformed, truncated, unexpectedly-typed, or hostile JSON, HTTP 5xx, or an empty body. Find every place that does an unchecked .at()/[]/get<T>() on nlohmann::json, trusts a field type, parses a number without range checking, or maps an unknown broker status to a permissive default. Check timeouts, retry policy and whether a retry can re-send a mutation.' },
  { key: 'kotak-adapter', scope: 'src/adapters/kotak/ (rest client, session, ws protocol, errors, capabilities, broker adapter)',
    lens: 'BROKER TRANSPORT ROBUSTNESS AND PROTOCOL PARSING. Same adversarial stance as the Kite reviewer, plus: the WebSocket binary/text protocol parser is attacker-adjacent — check every offset arithmetic, length prefix, and buffer index for out-of-bounds reads on a short or lying frame. Check multi-step auth token lifetime and refresh races.' },
  { key: 'domain-money', scope: 'src/domain/ (money, decimal_paise, types, utf8, redaction, enums, version) and include/broker_exec/domain/',
    lens: 'EXACT ARITHMETIC AND PARSING. Find any double/float in a money or price path, any int64 multiply/add that can overflow before it is checked, any narrowing conversion, any decimal string parse that accepts garbage or silently truncates, any UTF-8 validator that accepts overlong encodings/surrogates/truncated sequences, and any redaction that leaks a secret through a path it does not cover.' },
  { key: 'options-safety', scope: 'src/options/ (hedge_first, basket, margin_shock, sliced_leg), src/slicing/, src/protection/stop_supervisor.cpp',
    lens: 'OPTION-SELLING SAFETY. Hedge-first means the protective leg must be confirmed live before the naked-risk leg is sent. Find any ordering, partial-fill, or failure path that can leave a naked short. Check basket rollback/unwind on a mid-basket failure, slice sizing arithmetic against exchange freeze limits, and whether a stop can be lost when the supervisor restarts.' },
  { key: 'modes-killswitch', scope: 'src/modes/ (killswitch, posture, trading_mode), src/sessionguard/, src/modifyguard/, src/lifecycle/, src/isolation/',
    lens: 'STATE MACHINES AND KILL SWITCHES. A kill switch must be sticky, durable and fail-closed. Find any transition table hole, any way to leave a halted state without the required operator action, any in-memory-only flag that a restart clears, and any lifecycle transition that permits an illegal order state jump.' },
  { key: 'boot-composition', scope: 'src/boot/, src/composition/, src/main/, src/config/, src/secrets/',
    lens: 'STARTUP, WIRING AND SECRETS. Check the boot check ordering (can the engine trade before replay/reconcile completes?), the exit-code contract, config parsing for missing/invalid values defaulting permissively, TOML type confusion, secret material lifetime in memory, token store file permissions, and any place a null/absent dependency is silently accepted by the composition root.' },
  { key: 'observability-ledger', scope: 'src/observability/, src/ledger/, src/alerting/, src/cli/, src/health/',
    lens: 'AUDIT INTEGRITY AND OPERATOR VISIBILITY. The ledger and audit trail are evidence. Find hash-chain breaks, non-canonical serialization that makes the chain unverifiable, unbounded memory growth, a log/alert path that can throw or block the trading loop, a health endpoint that reports healthy while a subsystem is dead, and any secret or PII that reaches a log line.' },
  { key: 'concurrency', scope: 'the WHOLE repo — grep for std::thread, std::mutex, std::atomic, condition_variable, detach(), shared_ptr, weak_ptr, callback/std::function members, and any WebSocket or HTTP callback',
    lens: 'CONCURRENCY AND LIFETIME. Find data races on non-atomic shared state, mutexes not held across a compound check-then-act, a callback that can fire after its owner is destroyed, a detached thread outliving its captures, deadlock-capable lock ordering, and any use of a member from an I/O thread that the design says is single-threaded. Report the exact shared object and the two racing paths.' },
  { key: 'portability', scope: 'the WHOLE repo — grep for _WIN32, __linux__, #include <unistd.h>, <windows.h>, fsync, localtime, gmtime, strerror, getenv, and hand-built path strings',
    lens: 'CROSS-PLATFORM CORRECTNESS. Per docs/conventions.md, OS divergence lives ONLY in src/platform/. Find violations, plus: non-thread-safe C library calls (localtime/strerror/getenv), time-zone or DST assumptions in the trading calendar, path handling that breaks on Windows, text-vs-binary file mode differences that corrupt a durable log on Windows, and locale-dependent number formatting or parsing.' },
  { key: 'test-quality', scope: 'all src/**/*_test.cpp and tests/ (conformance, portability, sigkill)',
    lens: 'FAKE-GREEN COVERAGE. All 51 suites pass — find where that green is a lie. Hunt for assertions that cannot fail, tests that assert only that a call did not throw, error paths and boundary values with no test at all, a test that would still pass if the function body were deleted or returned a default, a fake/mock so permissive it cannot catch the bug it exists to catch, and safety guarantees from the core promise that no test actually pins down. Name the specific guarantee left untested.' },
]

phase('Review')

const results = await pipeline(
  REVIEWERS,
  (r) => agent(
    `${CONTEXT}\n\nYOUR ASSIGNED SCOPE: ${r.scope}\n\nYOUR LENS: ${r.lens}\n\n` +
    `Read the code in your scope thoroughly, then report your findings.`,
    { label: `review:${r.key}`, phase: 'Review', schema: FINDINGS_SCHEMA }
  ),
  (res, r) => {
    const fs = (res && res.findings) || []
    if (!fs.length) return []
    return parallel(fs.map((f) => () =>
      agent(
        `You are a SKEPTICAL senior C++ reviewer. Another reviewer filed the finding below against\n` +
        `the repository at the repository root. Your job is to REFUTE it.\n\n` +
        `FINDING\n title: ${f.title}\n file: ${f.file}:${f.line}\n severity: ${f.severity}\n` +
        ` problem: ${f.problem}\n failure scenario: ${f.failure_scenario}\n evidence claimed: ${f.evidence}\n\n` +
        `Open ${f.file}, read the surrounding code AND the callers, and check whether the claim survives.\n` +
        `Refute it (real=false) if ANY of these hold:\n` +
        ` - the quoted evidence does not match what the file actually contains (hallucinated code)\n` +
        ` - a guard, assert, caller precondition, type invariant, or earlier validation already prevents it\n` +
        ` - the failure scenario is impossible given how the function is actually called\n` +
        ` - it is a style/preference complaint dressed up as a defect\n` +
        ` - the code is correct and the reviewer misread it\n` +
        `Confirm it (real=true) ONLY if you traced a concrete path to the failure yourself.\n` +
        `Default to real=false when you are uncertain. Also give corrected_severity if the reviewer over- or under-rated it.`,
        { label: `verify:${f.file.split('/').pop()}`, phase: 'Verify', schema: VERDICT_SCHEMA }
      ).then((v) => ({ finding: f, reviewer: r.key, verdict: v }))
        .catch(() => null)
    ))
  }
)

const judged = results.flat().filter(Boolean).filter((x) => x && x.verdict)
const survivors = judged.filter((x) => x.verdict.real)

log(`${judged.length} findings judged, ${survivors.length} survived refutation`)

phase('Verify')

// Escalation: anything the skeptic kept at critical/high gets a second, independent skeptic.
const escalated = await parallel(survivors.map((s) => () => {
  const sev = s.verdict.corrected_severity || s.finding.severity
  if (sev !== 'critical' && sev !== 'high') return Promise.resolve({ ...s, severity: sev, votes: 1 })
  return agent(
    `SECOND INDEPENDENT REVIEW. Do not trust the previous reviewers. Repository:\n` +
    `the repository root\n\n` +
    `CLAIM: ${s.finding.title}\n at ${s.finding.file}:${s.finding.line}\n` +
    ` ${s.finding.problem}\n failure: ${s.finding.failure_scenario}\n\n` +
    `Read the file and its callers yourself. Is this a real defect that would justify opening a\n` +
    `${sev}-severity issue against a library that handles real money? Answer real=true only if you\n` +
    `independently traced the failure. Give corrected_severity.`,
    { label: `escalate:${s.finding.file.split('/').pop()}`, phase: 'Verify', schema: VERDICT_SCHEMA }
  ).then((v2) => ({ ...s, severity: v2.corrected_severity || sev, votes: v2.real ? 2 : 1, second: v2 }))
    .catch(() => ({ ...s, severity: sev, votes: 1 }))
}))

const confirmed = escalated.filter(Boolean).filter((s) => {
  const sev = s.severity
  if (sev === 'critical' || sev === 'high') return s.votes === 2
  return true
})

log(`${confirmed.length} confirmed after escalation`)

return {
  confirmed: confirmed.map((c) => ({
    title: c.finding.title,
    file: c.finding.file,
    line: c.finding.line,
    severity: c.severity,
    area: c.finding.area,
    problem: c.finding.problem,
    failure_scenario: c.finding.failure_scenario,
    evidence: c.finding.evidence,
    fix: c.finding.fix,
    reviewer: c.reviewer,
    votes: c.votes,
    verifier_note: c.verdict.reasoning,
  })),
  rejected: judged.filter((x) => !x.verdict.real).map((x) => ({ title: x.finding.title, why: x.verdict.reasoning })),
}
