#pragma once

// broker_exec::accounts — the SHARED REFERENCE-DATA CACHE (Story 6.5b, FR-33,
// AC-2; architecture COH-2/IBR-4).
//
// WHY THIS EXISTS. Reference data is identical for every account on a broker:
// the instrument master and the trading calendar depend on
// (broker, segment, trading_date) and on nothing else. With one process per
// account (ID-1), N processes waking at 09:00 would each download the same
// multi-megabyte dump — N times the bandwidth, N times the rate-limit exposure,
// and N chances to be the one that fails and blocks safe-start. Exactly ONE
// sibling should download; the rest should read what it wrote.
//
// ── THE PROTOCOL ────────────────────────────────────────────────────────────
//   1. FAST PATH. If the artifact for the key exists, is non-empty, and passes
//      the injected `validate` sanity check, return its contents. No lock, no
//      fetch. (This is the path taken by every process after the first.)
//   2. ACQUIRE THE CROSS-PROCESS LOCK (`platform::try_acquire_file_lock`).
//   3. DOUBLE-CHECK. Re-read the cache under the lock: the winner may have
//      finished while we were queueing. Classic double-checked locking, and it
//      is what makes "N processes start simultaneously" cost exactly one fetch.
//   4. FETCH through the injected seam, VALIDATE the result, and write it
//      ATOMICALLY: a temp file in the same directory, fsync'd via
//      `platform::durable_sync`, then renamed onto the artifact name. A reader
//      therefore only ever observes the OLD complete artifact or the NEW complete
//      artifact — never a half-written one, even if we are killed mid-write.
//   5. LOSERS WAIT AND RE-READ, bounded. A process that cannot take the lock
//      re-checks the cache, waits (injected wait seam — no hidden sleeps in
//      tests), and retries a bounded number of times. If the bound is exhausted
//      with no artifact, that is a typed DataStale Error: FAIL CLOSED. It never
//      "gives up and fetches anyway" (that would defeat the whole point) and it
//      never waits forever (that would hang safe-start).
//
// ── FAILURE POSTURES (all fail-closed, none crash) ──────────────────────────
//   * A corrupt, empty, or unreadable cached artifact is treated as A MISS and
//     re-fetched — it is never returned to a caller and never parsed on trust.
//   * A fetched payload that fails `validate` is NEVER written to the cache; the
//     call returns a Validation Error so the caller's freshness gate blocks.
//   * A lock timeout with no artifact returns DataStale (BlockStrategy).
//
// ── COMPOSITION WITH refdata ────────────────────────────────────────────────
// `refdata::InstrumentMaster` and `refdata::TradingCalendar` each take an
// injected `std::function<Result<std::string>()>` fetch seam. The adapters at the
// bottom of this header produce EXACTLY that shape, wrapping the composition
// root's real downloader so the cache is consulted first. Neither refdata class
// is modified or even linked by this module — the seam is the whole contract.
//
// ── LOCATION ────────────────────────────────────────────────────────────────
// The shared root MUST live OUTSIDE every per-account tree (an account directory
// is owner-private and per-account; refdata is neither). Use
// `require_outside_account_tree()` at composition time to make that a checked
// invariant rather than a comment.
//
// Conventions: no-throw across the boundary (`Result<T>`), std::filesystem for
// paths, no OS API outside src/platform, no float.

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::accounts {

// The cache key. Reference data varies by exactly these three things.
//   broker / segment — `[a-z0-9-]{1,64}`, NO underscore (see below).
//   trading_date     — strict ISO `YYYY-MM-DD`.
//
// WHY NO UNDERSCORE. The artifact filename is `<broker>_<segment>_<date><ext>`,
// so an underscore inside a component reopens the concatenation trap the account
// id closed: ("kite", "nfo_opt") and ("kite_nfo", "opt") would produce the SAME
// filename AND the same lock path — two different data sets silently sharing one
// cache entry, and two unrelated downloads serialized behind one lock. Excluding
// `_` from the components makes the `_` separator unambiguous. `-` is still
// allowed and is the separator to use inside a component.
//
// Lowercase-only for the same reason the account id is: the components become a
// filename, and a case-insensitive filesystem would merge two keys into one.
struct RefdataKey {
  std::string broker;
  std::string segment;
  std::string trading_date;
  std::string extension = ".csv";  // artifact suffix, e.g. ".csv" / ".json"
};

// Reject a key that could not safely become a filename (fail closed — the key is
// never sanitized into something "close enough").
[[nodiscard]] Result<ports::Ok> validate_refdata_key(const RefdataKey& key);

// Produces the payload for a cache miss (the composition root's real download).
using RefdataFetchFn = std::function<Result<std::string>()>;

// Cheap sanity check on a payload: true == "this looks like real reference
// data". Used BOTH on a cached artifact (a corrupt file must not be trusted) and
// on a freshly fetched one (garbage must never be written to the shared cache).
using RefdataValidateFn = std::function<bool(std::string_view)>;

// Called between bounded lock-acquisition attempts. Injected so tests never
// sleep against the wall clock; production uses `default_lock_wait_fn()`.
using LockWaitFn = std::function<void(int attempt)>;

// A CSV artifact must be more than "not empty": it needs a header-ish first line
// (commas AND at least one letter, so a stray "0,0" is not mistaken for a dump)
// and at least one line after it. That catches the realistic corruptions — a
// truncated download, an error page, a header with no rows — without pretending
// to be a parser (refdata owns the real parse and its typed errors).
[[nodiscard]] RefdataValidateFn csv_sanity_validator();

// Non-empty (ignoring whitespace) and structurally bracketed: first non-space
// char is '{' or '[' and the last is the matching '}' or ']'. Rejects a
// truncated JSON document (the classic half-written-cache failure). It is a
// SHAPE check, not a parse — a document with balanced outer brackets and garbage
// inside still reaches refdata's real parser, which rejects it with a typed
// error.
[[nodiscard]] RefdataValidateFn json_sanity_validator();

// The production wait: a short fixed sleep between attempts. Only ever runs at
// safe-start, never on the trading hot path.
[[nodiscard]] LockWaitFn default_lock_wait_fn(std::chrono::milliseconds step);

// The publish step, as an injectable seam: rename `from` onto `to`, returning
// true on success. Defaults to `std::filesystem::rename` (error_code overload).
// It is a seam because it is the one step with a REAL, platform-specific
// transient failure mode — see `max_publish_attempts`.
using PublishRenameFn =
    std::function<bool(const std::filesystem::path& from, const std::filesystem::path& to)>;

[[nodiscard]] PublishRenameFn default_publish_rename_fn();

struct SharedRefdataCacheConfig {
  // Where the shared artifacts live. MUST be outside every account data dir.
  std::filesystem::path shared_root;

  // The per-account data root, if the caller has one. When set, every
  // `get_or_fetch` ENFORCES `require_outside_account_tree` — the guard becomes a
  // checked invariant instead of an opt-in call the composition root can forget.
  // Empty means "not supplied" and the check is skipped.
  std::filesystem::path account_data_root;

  // Staleness window for the cross-process lock (see platform/file_lock.hpp:
  // this is a LIVENESS assumption — keep it far larger than a download). The
  // cache also `touch()`es the lock around the fetch so a slow download does not
  // make a live holder look like a corpse.
  std::chrono::seconds lock_staleness{600};

  // Bounded retries for a process that loses the lock race. Exhausting them is a
  // typed DataStale Error, never an unbounded wait and never a rogue fetch.
  int max_lock_attempts = 50;

  // Bounded retries for the atomic publish rename. On Windows a rename onto a
  // target another process currently has OPEN FOR READ fails with
  // ERROR_ACCESS_DENIED — a sibling reading yesterday's artifact at the instant
  // we publish today's is exactly that case, and it is TRANSIENT. Retrying a few
  // times turns a spurious hard failure into a short wait.
  int max_publish_attempts = 10;

  // Injected wait between attempts. Defaults to a real 200 ms sleep so
  // production has sane behavior without configuring anything; tests set it to
  // an EMPTY function, which is the documented "never sleep" opt-out.
  LockWaitFn wait = default_lock_wait_fn(std::chrono::milliseconds{200});

  // Injected publish seam (see PublishRenameFn). Empty means the default.
  PublishRenameFn publish_rename;
};

// Fail-closed composition-time guard: the shared cache root must not live inside
// the per-account data tree (nor the reverse). Compares lexically normalized
// paths component-wise, and a trailing separator is stripped first — `"/a/b/"`
// and `"/a/b"` name the same directory, and treating them as different would let
// a trailing slash silently defeat the check.
[[nodiscard]] Result<ports::Ok> require_outside_account_tree(
    const std::filesystem::path& shared_root, const std::filesystem::path& account_data_root);

// The cache. Holds no state between calls beyond its configuration: the
// filesystem IS the shared state, which is exactly why this works across
// processes. Copyable/movable; safe to build one per account process.
class SharedRefdataCache {
 public:
  // Construction also runs `sweep_debris()` once (best-effort, no-throw): a
  // process that was SIGKILLed mid-publish leaves a `.tmp-*` file, and an
  // aborted takeover can leave a `.stale-*` file. Nothing reads them, so they
  // would otherwise accumulate forever in the shared directory.
  explicit SharedRefdataCache(SharedRefdataCacheConfig config);

  // The artifact path for a key: `<shared_root>/<broker>_<segment>_<date><ext>`.
  [[nodiscard]] std::filesystem::path artifact_path(const RefdataKey& key) const;

  // The cross-process lock path for a key: the artifact path + ".lock".
  [[nodiscard]] std::filesystem::path lock_path(const RefdataKey& key) const;

  // Remove abandoned `*.tmp-*` / `*.stale-*` files under the shared root that
  // are older than the lock staleness window. Age-gated so a concurrent
  // publish/takeover in flight is never disturbed. Returns how many were
  // removed. Never touches an artifact or a live `.lock`. No-throw.
  int sweep_debris() const noexcept;

  // The protocol described at the top of this header. Returns the artifact's
  // contents (cached or freshly fetched) or a typed Error. Never throws, never
  // returns a payload that failed `validate`, never writes one either.
  [[nodiscard]] Result<std::string> get_or_fetch(const RefdataKey& key,
                                                 const RefdataFetchFn& fetch,
                                                 const RefdataValidateFn& validate) const;

  [[nodiscard]] const SharedRefdataCacheConfig& config() const noexcept { return config_; }

 private:
  SharedRefdataCacheConfig config_;
};

// ── refdata composition adapters ────────────────────────────────────────────
// Both produce the EXACT `std::function<Result<std::string>()>` shape that
// `refdata::InstrumentMaster` (fetch_csv) and `refdata::TradingCalendar`
// (fetch_json) accept, so the composition root wires them in place of the raw
// downloader and gets the shared cache for free.
//
// The trading date is resolved by `trading_date_fn` AT CALL TIME (not at wiring
// time) so a process that lives across midnight keys the next day's artifact
// correctly — the same reason refdata stamps its own cache per day.
//
// LIFETIME: the cache is taken as a `shared_ptr` and CAPTURED BY VALUE, so the
// returned function keeps it alive. These functions outlive their call site by
// design — they are handed to an `InstrumentMaster`/`TradingCalendar` that may
// live for the whole session — and a raw reference would make a dangling-cache
// bug both easy to write and impossible to see at the call site.

// fetch_csv seam for `refdata::InstrumentMaster` (artifact suffix ".csv",
// `csv_sanity_validator()` unless overridden).
[[nodiscard]] RefdataFetchFn shared_instrument_csv_fetcher(
    std::shared_ptr<const SharedRefdataCache> cache, std::string broker, std::string segment,
    std::function<std::string()> trading_date_fn, RefdataFetchFn upstream,
    RefdataValidateFn validate = {});

// fetch_json seam for `refdata::TradingCalendar` (artifact suffix ".json",
// `json_sanity_validator()` unless overridden). The calendar has no segment, so
// a fixed "calendar" segment keys it.
[[nodiscard]] RefdataFetchFn shared_calendar_json_fetcher(
    std::shared_ptr<const SharedRefdataCache> cache, std::string broker,
    std::function<std::string()> trading_date_fn, RefdataFetchFn upstream,
    RefdataValidateFn validate = {});

}  // namespace broker_exec::accounts
