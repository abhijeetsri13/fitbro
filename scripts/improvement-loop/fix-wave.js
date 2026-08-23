// ---------------------------------------------------------------------------
// Improvement loop, stage 4: fix a wave of issues in parallel.
//
// Run with the Claude Code Workflow tool, passing the partition table as args:
//
//   Workflow({
//     scriptPath: 'scripts/improvement-loop/fix-wave.js',
//     args: [
//       { key: 'store', scope: 'src/store/ and include/broker_exec/store/',
//         issues: [{ number: '17', title: '...', severity: 'high',
//                    file: 'src/store/store.cpp', line: 416,
//                    problem: '...', failure_scenario: '...', evidence: '...',
//                    fix: '...', verifier_note: '...' }] },
//       ...
//     ],
//   })
//
// THE PARTITIONS MUST HAVE DISJOINT FILE OWNERSHIP. Two agents editing one file
// corrupt each other's edits, and neither notices. Partition by module; if a fix
// needs a file another partition owns, the agent reports it instead of reaching
// for it, and it becomes the next round's work.
//
// Nobody here builds and nobody touches git: a shared build directory does not
// survive concurrent writers. The caller runs ONE serial build + full ctest +
// clang-format afterwards, and only then commits. See docs/improvement-loop.md.
// ---------------------------------------------------------------------------

export const meta = {
  name: 'improvement-loop-fix-wave',
  description: 'Fix one wave of confirmed issues in parallel, one agent per disjoint partition',
  phases: [{ title: 'Fix', detail: 'one agent per partition, disjoint file ownership' }],
}

const REPORT_SCHEMA = {
  type: 'object',
  required: ['issues', 'summary', 'files_changed', 'risk'],
  additionalProperties: false,
  properties: {
    issues: {
      type: 'array',
      items: {
        type: 'object',
        required: ['number', 'outcome', 'what_changed', 'tests_added', 'why_tests_fail_before'],
        additionalProperties: false,
        properties: {
          number: { type: 'string' },
          // "not_a_defect" is a first-class outcome. An agent that reads the code
          // and refutes the issue is doing the job; one that invents a change to
          // look productive is not.
          outcome: { type: 'string', enum: ['fixed', 'partially_fixed', 'not_a_defect', 'deferred'] },
          what_changed: { type: 'string' },
          tests_added: { type: 'string' },
          why_tests_fail_before: { type: 'string' },
          out_of_scope_work_needed: { type: 'string' },
        },
      },
    },
    summary: { type: 'string' },
    files_changed: { type: 'array', items: { type: 'string' } },
    risk: { type: 'string' },
  },
}

const RULES = `
REPOSITORY: "broker_exec", a C++20 broker-neutral trading execution and safety library
(Zerodha Kite, Kotak Neo). Money is exact int64 paise, errors are std::expected<T, Error>,
nothing throws across the strategy boundary, and the design fails CLOSED on doubt. Read
docs/conventions.md before you change anything.

YOUR JOB: fix the issue(s) below, and add or strengthen the tests that pin the fix.

HARD RULES - violating any of these breaks the other agents working in parallel:
1. EDIT ONLY THE FILES IN YOUR SCOPE. Another agent owns every other file right now.
   If the correct fix genuinely requires a file outside your scope, DO NOT edit it - make
   the fix you can inside your scope and say clearly in your report what else is needed.
2. DO NOT run cmake, ninja, msbuild, ctest, or any build command. A shared build directory
   is in use; a parallel build corrupts it. The orchestrator builds and runs the full suite
   after you finish. Reason about compilation carefully instead - you get no compiler.
3. DO NOT run any git command. No add, no commit, no checkout, no stash.
4. DO NOT reformat unrelated code. Match the file's existing style exactly: 2-space indent,
   100-column limit, and the same dense explanatory comment voice the file already uses.
   Comments here explain WHY, and name the failure they prevent. Write in that register.

QUALITY BAR - this is a library that places real orders with real money:
- Fix the CAUSE, not the symptom. If the same mistake appears at sibling call sites in your
  scope, fix those too.
- Fail CLOSED. When a value cannot be trusted, refuse the action; never default to permissive.
- No new throw across a Result-returning boundary. No double/float in a money or price path.
- No OS-specific API or #ifdef outside src/platform/.
- Every new test must FAIL against the old behaviour. A test that passes both before and
  after your change is worse than no test: it manufactures confidence. State explicitly, for
  each test you add, why it fails before the fix.
- If you conclude the reported issue is NOT real after reading the code, do not invent a
  change. Say so, with the evidence that refutes it. That is a valid and useful outcome.
- If a fix is too large or too risky to land safely in this scope (a schema migration, a
  cross-cutting redesign), do the safe part, and describe the rest precisely.

Be surgical. A reviewer will read this diff line by line.
`

const GROUPS = (args || []).map((g) => {
  const body = g.issues.map((i) =>
    `### Issue #${i.number} - ${i.title}  [${i.severity}]\n` +
    `Reported at \`${i.file}:${i.line}\` - locate by the quoted code, not the line number.\n\n` +
    `PROBLEM\n${i.problem}\n\nFAILURE SCENARIO\n${i.failure_scenario}\n\n` +
    `EVIDENCE\n\`\`\`cpp\n${i.evidence}\n\`\`\`\n\n` +
    `SUGGESTED FIX (a reviewer's proposal, not a specification - use your judgement)\n${i.fix}\n\n` +
    (i.verifier_note ? `AN INDEPENDENT VERIFIER TRACED THIS AND CONFIRMED IT:\n${i.verifier_note}\n` : '')
  ).join('\n\n')
  return {
    key: g.key,
    numbers: g.issues.map((i) => i.number),
    prompt: RULES + '\n\nYOUR SCOPE (the ONLY files you may edit): ' + g.scope +
            '\n\n=== ISSUES ASSIGNED TO YOU ===\n\n' + body,
  }
})

if (!GROUPS.length) {
  throw new Error('fix-wave.js: pass the partition table as args (see the header comment)')
}

phase('Fix')

const results = await parallel(GROUPS.map((g) => () =>
  agent(g.prompt, { label: 'fix:' + g.key, phase: 'Fix', schema: REPORT_SCHEMA })
    .then((r) => ({ group: g.key, issues: g.numbers, report: r }))
    .catch((e) => ({ group: g.key, issues: g.numbers, error: String(e) }))
))

return results.filter(Boolean)
