# The improvement loop

A repeatable cycle for finding, recording and fixing defects in this repository. It exists
because this is a library that places real orders with real money, the easy defects were
already gone by the time it started, and the ones that remain are the kind a single reading
does not surface.

Every claim below is gated on evidence: a finding must survive an adversarial attempt to
refute it before it becomes an issue, and a fix must survive a full build and the complete
test suite before it becomes a commit.

## The cycle

```
  ┌─ 1. REVIEW ────────────────────────────────────────────────────────────┐
  │  Partition the tree by module and by lens. One reviewer per partition,  │
  │  all in parallel. Each returns at most 6 findings, every one carrying   │
  │  a file, a line, a verbatim code quote, and a concrete failure path.    │
  └────────────────────────────────────────────────────────────────────────┘
                                    ↓
  ┌─ 2. REFUTE ────────────────────────────────────────────────────────────┐
  │  Every finding goes to an independent reviewer whose instruction is to  │
  │  KILL it: check the quote against the file, look for the guard that     │
  │  already prevents it, read the callers, default to "not real" when      │
  │  uncertain. Anything rated critical or high then faces a second, fully  │
  │  independent reviewer; it survives only on two votes.                   │
  └────────────────────────────────────────────────────────────────────────┘
                                    ↓
  ┌─ 3. RECORD ────────────────────────────────────────────────────────────┐
  │  Each survivor becomes a GitHub issue carrying the whole chain: the     │
  │  problem, the failure scenario, the evidence, the proposed fix, and the │
  │  verifier's independent trace. Labelled sev:* and area:*, tagged loop.  │
  │  An issue is the unit of work from here on; commits cite its number.    │
  └────────────────────────────────────────────────────────────────────────┘
                                    ↓
  ┌─ 4. FIX ───────────────────────────────────────────────────────────────┐
  │  Issues are grouped into partitions with DISJOINT file ownership, and   │
  │  one agent fixes each partition in parallel. Nobody builds and nobody   │
  │  touches git — a shared build directory does not survive concurrent     │
  │  writers. Every fix must come with a test that FAILS against the old    │
  │  behaviour, and the agent must state why it fails.                      │
  └────────────────────────────────────────────────────────────────────────┘
                                    ↓
  ┌─ 5. GATE ──────────────────────────────────────────────────────────────┐
  │  One serial build. `ctest` in full. `clang-format` over the diff.       │
  │  Nothing is committed until all three are clean. Then one commit per    │
  │  issue, and CI on the branch is the second, independent gate.           │
  └────────────────────────────────────────────────────────────────────────┘
                                    ↓
                         back to 1, on the fixed tree
```

## Rules that make it work

**A finding is not a defect until someone has tried to kill it.** The refutation pass is not
a formality — in round 1 it rejected 38 of 72 findings, including several that read
convincingly. Reviewers hallucinate code, miss the guard three lines up, and dress
preferences as defects. The verifier's trace is kept on the issue so the reasoning can be
audited later.

**Disjoint file ownership, or nothing.** Parallel fixers that share a file corrupt each
other's edits. Partition by module; if a fix needs a file outside its partition, the agent
reports what is needed instead of reaching for it, and it becomes the next round's work.

**One builder.** Agents edit and reason about compilation; they never invoke the build. A
single serial build and test run after the wave is what turns edits into a commit.

**A test that passes before the fix is worse than no test.** It manufactures confidence.
Every fix states its discriminating test — the observation that differs between the old and
new behaviour. Where no such test is possible, that is said plainly in the commit message
rather than papered over. (Example: the token-store fix in #6 is pinned by comparing
`st_ino` across two saves, because inode identity is the one externally visible difference
between rewriting a file in place and replacing it.)

**Fail closed, in the fix as much as in the code.** When a fix cannot be made safely inside
its scope, the agent does the safe part and describes the rest. Half a correct fix beats a
whole speculative one in a money path.

## Running it

The review and fix waves are driven by workflow scripts under
[`scripts/improvement-loop/`](../scripts/improvement-loop/). They are checked in so a round
is reproducible and so the review partitions and lenses can be edited rather than reinvented.

- `review.js` — the parallel review + adversarial refutation pass. Returns confirmed
  findings and the rejected ones with the reason each was killed.
- `fix-wave.js` — takes a partition table of issues and fixes each partition in parallel.

Between the two, findings are filed as issues; after the fix wave, the gate is:

```bash
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
pip install "clang-format==17.0.6" && clang-format --dry-run --Werror <changed files>
bash scripts/check-sources-tracked.sh
```

## What round 1 produced

41 issues filed. The three that mattered most were invisible from inside the working tree:

- **#2** — `.gitignore` carried the unanchored pattern `secrets/`, so `src/secrets/` was
  never committed and a clean clone could not configure. Every CI run had been red since the
  first C++ commit; the working tree built fine, which is exactly why nobody noticed.
- **#8** — `store.cpp` did not compile with clang under the project's own `-Werror`. MSVC
  does not implement the warning, so the Windows build was green.
- **#10** — a default `cpr::Session` follows redirects and re-sends the POST body, so one
  `place()` could put up to 50 orders on the wire — a duplicate created *below* the
  idempotency layer built to prevent it.

The pattern in all three: the defect was in the space *between* the developer's environment
and everyone else's. That is the space this loop is for.
