# Multi-account operation (FR-33)

How the library runs more than one broker account on one machine, and what the
operator owns versus what the library owns.

---

## The model: one OS process per account

Each account gets its **own process**, its **own data directory**, and its **own
crash/restart state**. Nothing is shared between accounts except reference data,
and that is shared deliberately and read-only-after-write (see below).

```
                    /var/lib/broker-exec/
                    ├── accounts/
                    │   ├── kite-primary/      ← process 1, 0700, private
                    │   │   ├── intent.log             fsync'd write-ahead log
                    │   │   ├── store.sqlite3          projection of the log
                    │   │   ├── token_store.enc        AES-256-GCM session token
                    │   │   ├── ledger.jsonl           hash-chained audit ledger
                    │   │   └── kill_journal.jsonl     kill-switch journal
                    │   └── kotak-hedge/       ← process 2, 0700, private
                    │       └── … same layout …
                    └── shared/
                        └── refdata/           ← SHARED by every process
                            ├── kite_NFO_2026-08-10.csv
                            ├── kite_NFO_2026-08-10.csv.lock
                            └── kite_calendar_2026-08-10.json
```

**Why processes and not threads.** An account is a blast radius. A segfault, a
stuck broker socket, an OOM kill or a `std::terminate` in one account must not
take the others with it, and the OS is the only isolation boundary that holds
under all four. It also means an account's write-ahead intent log has exactly one
writer, which is what makes SIGKILL recovery provable.

### The layout is owned by one type

`broker_exec::accounts::AccountDataDir` (`include/broker_exec/accounts/account_data_dir.hpp`)
derives every per-account path from one validated account id and creates the tree
idempotently at 0700 through the platform permissions seam.

An **account id is a name, not a path**: `[a-z0-9_-]{1,64}`, and not a Windows
reserved device name. Anything else is *rejected*, never sanitized — `..`, `/`,
`\`, `.`, NUL, spaces and shell metacharacters all fail validation. Because
exactly one directory level is derived from the id, two distinct ids are two
distinct sibling directories, so their trees **cannot** overlap (the
`"ab"+"c"` vs `"a"+"bc"` concatenation trap is structurally impossible).

**Lowercase only, and that is a correctness rule.** NTFS and stock APFS/HFS+ are
case-insensitive, so `AB` and `ab` would be two accepted ids resolving to *one*
directory — two processes sharing an intent log and a SQLite file. Uppercase is
rejected rather than silently lowercased, because lowercasing would merge the two
accounts instead of refusing them.

---

## Shared reference data + the cross-process lock

The instrument master and the trading calendar depend only on
`(broker, segment, trading_date)` — they are identical for every account. With N
account processes waking at 09:00, N independent downloads is N times the
bandwidth, N times the rate-limit exposure and N chances for the one failure that
blocks safe-start.

`broker_exec::accounts::SharedRefdataCache` makes it exactly one:

1. **Fast path** — if the artifact exists and passes a sanity check, return it.
   No lock, no download. This is the path every process after the first takes.
2. **Lock** — otherwise take the cross-process lock
   (`broker_exec::platform::try_acquire_file_lock`).
3. **Double-check** — re-read the cache under the lock; the winner may have
   finished while we queued.
4. **Fetch, validate, publish atomically** — temp file → `fsync` → `rename` over
   the artifact name. A reader only ever sees the old complete artifact or the
   new complete one, never a half-written file.
5. **Losers wait and re-read, bounded** — a process that cannot take the lock
   re-checks, waits, and retries a bounded number of times. Exhausting the bound
   with no artifact is a typed `DataStale` error (`BlockStrategy`): **fail
   closed**. It never gives up and downloads anyway, and it never waits forever.

A corrupt, empty or unreadable cached artifact is treated as a **miss** and
re-fetched; it is never returned to a caller. A freshly fetched payload that
fails the sanity check is **never written** — poisoning the shared cache would
break every sibling account, not just the one that fetched it.

The publish rename is **retried** before it is called a failure. On Windows,
renaming onto a target another process currently has open for reading fails with
`ERROR_ACCESS_DENIED` — a sibling reading yesterday's artifact at the instant we
publish today's is exactly that, and it is transient. A persistent failure is
classified `Transient`/`RetrySafe` (come back), not `Internal`/`RaiseAlert` (page
someone).

The cache composes with `refdata::InstrumentMaster` / `refdata::TradingCalendar`
through their existing injected fetch seams
(`shared_instrument_csv_fetcher` / `shared_calendar_json_fetcher`), so those
classes are unchanged and unaware. Both take the cache as a `shared_ptr`, because
the function they return outlives the call site by design.

**Key components are lowercase and may not contain `_`.** The artifact filename
is `<broker>_<segment>_<date><ext>`, so an underscore inside a component would
make `("kite","nfo_opt")` and `("kite_nfo","opt")` the *same* artifact and the
*same* lock. Use `-` inside a component; `_` is the separator. Lowercase for the
same case-insensitive-filesystem reason as the account id.

**The shared root must live outside every account tree.** Set
`SharedRefdataCacheConfig::account_data_root` and every `get_or_fetch` *enforces*
it — the guard is a checked invariant on the path that would create the files,
not an opt-in call the composition root can forget.
`accounts::require_outside_account_tree()` is also callable directly at
composition time. A trailing separator (`/data/` vs `/data`) does not defeat it.

Abandoned `*.tmp-*` (a SIGKILL mid-publish) and `*.stale-*` (an aborted takeover)
files are swept from the shared root at cache construction, age-gated by the lock
staleness window so nothing in flight is disturbed. Artifacts and live `.lock`
files are never candidates.

### The lock's guarantee, precisely

Ownership is conferred **only** by an atomic create-exclusive
(`open(O_CREAT|O_EXCL)` / `CreateFileW(CREATE_NEW)`), so two live holders cannot
coexist — the loser is told "held" and gets a typed retryable error, never a
crash.

A process can die holding the lock, so a lock file **older than the staleness
window** (default 10 minutes) may be taken over. The takeover is *not*
delete-then-create. It runs in this exact order:

1. **Rename** the stale file to a private name — atomic on the source name, so
   exactly one contender gets past this step.
2. **Immediately create-exclusive** our own lock at the now-free path, *before*
   judging anything. If that says "exists", someone beat us to the freed name: we
   abort, drop the file we claimed, and **never restore** (restoring would
   overwrite the new owner's lock).
3. **Only then** re-check the age of what we claimed. If it turns out to have
   been live — a legitimate holder acquired between our staleness observation and
   step 1 — we undo: remove our own lock (nonce-checked) and rename the victim's
   file back. A failed restore is reported as a typed `Internal` error naming the
   residue, never swallowed.

Doing step 2 before step 3 is load-bearing. Judging first leaves the lock path
*unoccupied* while the age is re-read, so a fourth process could create a lock
there and have it silently replaced by the aborting process's restore.

Every acquisition also writes a unique nonce and reads it back; `release()`
deletes the file **only** if the nonce is still ours; and `still_ours()` lets a
holder re-verify immediately before it publishes (the shared cache does exactly
that before every write). A long-running holder should call `touch()` to refresh
its mtime rather than rely on the window being wide enough — the cache touches
the lock around every fetch.

A stale lock file whose payload does **not parse** is never taken over: arbitrary
bytes at the lock path may not be a lock at all, so it fails closed with an
`Internal` error and waits for a human. Removing it is a deliberate operator
action:

```bash
# Only after confirming no broker-exec process is running for that key.
ls -l /var/lib/broker-exec/shared/refdata/*.lock
cat  /var/lib/broker-exec/shared/refdata/<artifact>.lock   # pid + acquired_at
rm   /var/lib/broker-exec/shared/refdata/<artifact>.lock
```

> The staleness window is a **liveness assumption, not a correctness escape
> hatch**: taking over is safe only if no live holder leaves its lock file
> untouched for longer than the window. Keep it far larger than any download.
> Shrinking it is a correctness change.

---

## Supervision

The library supplies the **decision**; the OS supplies the **supervision**.

- `supervisor::SupervisorPolicy` — exit code → restart-with-backoff /
  no-restart-escalate / clean stop, with capped exponential backoff and a
  crash-loop circuit breaker.
- `supervisor::SupervisorPlan` — the same policy applied **per account**:
  independent crash counters, per-account absence alarms, and a fleet summary
  (`any_escalated`, `accounts_down`, `accounts_alarmed`).
  **Account A crash-looping into escalation never changes account B's decision.**
  A registered account counts as **down until it reports a start** — "we have
  never seen it run" must not read as healthy, or a launch failure at 09:00 stays
  invisible until someone notices there are no orders.

Neither one forks, execs, spawns, signals or sleeps. Time is injected as an
explicit monotonic `now`. Actual process supervision is systemd's job via the
template unit in [`deploy/systemd/broker-exec@.service`](../deploy/systemd/broker-exec@.service).

### Exit-code contract

A supervised account process **must** honor these:

| Exit code | Meaning | Supervisor decision | systemd |
|---|---|---|---|
| `0` | Deliberate clean stop | `NoRestartCleanShutdown` — no restart, no alarm | `Restart=on-failure` ignores success |
| `70` | Fail closed, a human is required | `NoRestartEscalate` — never auto-restart, raise the absence alarm | `RestartPreventExitStatus=70` |
| anything else (`1`, `137`, `139`, `128+n`, negative…) | Crash | `RestartWithBackoff` (capped exponential), then `NoRestartEscalate` once the crash-loop breaker trips | `Restart=on-failure` + `StartLimitBurst` |

An **unrecognized exit code is treated as a crash**, never as a clean exit. An
unexplained stop is never assumed benign.

### systemd usage

```bash
sudo install -m 0644 deploy/systemd/broker-exec@.service \
                     deploy/systemd/broker-exec-alarm@.service /etc/systemd/system/
sudo systemctl daemon-reload

# One instance per account; the text after "@" is the ACCOUNT ID and must match
# the library's charset [a-z0-9_-]{1,64} — LOWERCASE ONLY.
sudo systemctl enable --now broker-exec@kite-primary.service
sudo systemctl enable --now broker-exec@kotak-hedge.service

systemctl status 'broker-exec@*'
journalctl -u broker-exec@kite-primary -f

# A unit that hit the start limit (the crash-loop breaker) stays FAILED until a
# human looks at it — that is the intended behavior, not a bug:
sudo systemctl reset-failed broker-exec@kite-primary.service
```

Edit `ExecStart` and `User`/`Group` before use. The two data directories are
created by systemd itself via `StateDirectory=` (owned by the service user,
`StateDirectoryMode=0700`), which also grants write access to exactly those two
paths and nothing else — everything else is read-only (`ProtectSystem=strict`).
`RestartSteps`/`RestartMaxDelaySec` give the capped exponential backoff and need
**systemd ≥ 254**; a flat-delay fallback is commented in the unit.

`OnFailure=broker-exec-alarm@%i.service` fires when an instance reaches FAILED —
i.e. exactly the two "escalate" cases (a fail-closed exit 70, or a tripped
crash-loop breaker). The shipped alarm unit writes one journal line; repoint its
`ExecStart` at whatever your operator actually reads.

### Every instance must run as the SAME OS user

The shared refdata cache is written by whichever instance wins the download and
read by all the others, and `StateDirectoryMode=0700` makes it owner-only. Running
instances under **different** users breaks both the cache and the cross-process
lock. If per-account OS users are a hard requirement, do not put the shared path
in `StateDirectory=`: give it its own group, `chgrp` + `chmod 2770` it (setgid so
new files inherit the group), and set `StateDirectoryMode=0750`. The per-account
trees stay 0700 per user either way.

### What is verified where

| Property | Verified by |
|---|---|
| Lock acquire / contend / stale takeover / RAII release / payload format / `still_ours()` / `touch()` | `src/platform/file_lock_test.cpp` |
| The three concurrent takeover branches (abort-and-restore, freed name re-taken, lock replaced before read-back) | `src/platform/file_lock_test.cpp`, driven through the documented `set_lock_fault_hook` test seam |
| Id validation incl. case-insensitive-filesystem aliasing, idempotent `ensure()`, 0700 seam, disjoint trees (`fs::equivalent`) | `src/accounts/account_data_dir_test.cpp` |
| Winner/loser hand-off, corrupt + empty + truncated re-fetch, bounded lock failure, atomic publish + retry classification, debris sweep, enforced tree guard, refdata composition | `src/accounts/shared_refdata_cache_test.cpp` |
| Per-account isolation, escalation, absence alarms, never-started-is-down, fleet summary | `src/supervisor/supervisor_plan_test.cpp` |
| Real cross-process contention, systemd behavior | **Tier-2, operator-side.** The atomicity primitive is the platform seam's contract; the tests simulate two processes as two objects contending on one lock path, and drive the true interleavings through the fault hook. |
