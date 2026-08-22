#include "broker_exec/accounts/shared_refdata_cache.hpp"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/platform/durable.hpp"
#include "broker_exec/platform/file_lock.hpp"

namespace broker_exec::accounts {

namespace fs = std::filesystem;

namespace {

using errors::ErrorCategory;
using errors::make_error;

// The "this is a NAME, not a path" charset for a key component, MINUS the
// underscore: the artifact filename is `<broker>_<segment>_<date><ext>`, so an
// underscore inside a component would let ("kite","nfo_opt") and
// ("kite_nfo","opt") produce the identical artifact AND lock path. Excluding it
// makes the `_` separator unambiguous. Lowercase-only for the same reason the
// account id is: these become filenames, and a case-insensitive filesystem would
// merge two distinct keys into one entry.
[[nodiscard]] bool is_safe_token(std::string_view token) {
  if (token.empty() || token.size() > 64) {
    return false;
  }
  for (const char c : token) {
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    if (!allowed) {
      return false;
    }
  }
  return true;
}

// Strict ISO YYYY-MM-DD shape (digits and dashes in fixed positions). We do not
// validate the calendar here — refdata owns date semantics; this only ensures the
// key cannot smuggle a path separator into a filename.
[[nodiscard]] bool is_iso_date(std::string_view date) {
  if (date.size() != 10) {
    return false;
  }
  for (std::size_t i = 0; i < date.size(); ++i) {
    const bool is_dash_position = (i == 4 || i == 7);
    const bool is_digit = date[i] >= '0' && date[i] <= '9';
    if (is_dash_position ? date[i] != '-' : !is_digit) {
      return false;
    }
  }
  return true;
}

// ".csv" / ".json" style: a leading dot then a short alphanumeric suffix.
[[nodiscard]] bool is_safe_extension(std::string_view ext) {
  if (ext.size() < 2 || ext.size() > 16 || ext.front() != '.') {
    return false;
  }
  for (std::size_t i = 1; i < ext.size(); ++i) {
    const char c = ext[i];
    const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (!allowed) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::string_view trim_view(std::string_view s) noexcept {
  const auto is_ws = [](char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  std::size_t begin = 0;
  std::size_t end = s.size();
  while (begin < end && is_ws(s[begin])) {
    ++begin;
  }
  while (end > begin && is_ws(s[end - 1])) {
    --end;
  }
  return s.substr(begin, end - begin);
}

// Read the cached artifact IF it exists, is non-empty, and passes `validate`.
// Anything else (missing, unreadable, empty, corrupt) is a MISS — deliberately
// indistinguishable to the caller, because all of them mean "re-fetch", and none
// of them may ever surface a half-trusted payload.
[[nodiscard]] std::optional<std::string> read_valid_artifact(const fs::path& path,
                                                             const RefdataValidateFn& validate) {
  std::error_code ec;
  if (!fs::exists(path, ec) || ec) {
    return std::nullopt;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (contents.empty()) {
    return std::nullopt;  // an empty cache file is corrupt, not "no data"
  }
  if (validate && !validate(contents)) {
    return std::nullopt;  // corrupt / truncated — re-fetch rather than trust it
  }
  return contents;
}

// Write `contents` to `target` so that no reader can ever observe a partial
// file: full write -> fflush -> durable_sync -> rename over the target. The temp
// name carries the holder's unique lock nonce, so two writers (impossible while
// the lock works, but belt-and-braces) could not collide on it either.
//
// The PUBLISH rename is retried. On Windows, renaming onto a target that another
// process currently has open for reading fails with ERROR_ACCESS_DENIED — and a
// sibling account reading yesterday's artifact at the instant we publish today's
// is exactly that. It is transient, so a persistent failure is classified
// Transient (RetrySafe), not Internal: the caller should come back, not alert.
[[nodiscard]] std::optional<errors::Error> write_atomically(
    const fs::path& target, const std::string& contents, const std::string& unique_suffix,
    int publish_attempts, const PublishRenameFn& publish, const LockWaitFn& wait) {
  fs::path tmp = target;
  tmp += ".tmp-" + unique_suffix;

  std::FILE* fp = std::fopen(tmp.string().c_str(), "wb");
  if (fp == nullptr) {
    return make_error(ErrorCategory::Internal, "shared refdata cache: cannot open temp artifact");
  }
  const std::size_t written = std::fwrite(contents.data(), 1, contents.size(), fp);
  if (written != contents.size() || std::fflush(fp) != 0) {
    std::fclose(fp);
    std::error_code cleanup;
    fs::remove(tmp, cleanup);
    return make_error(ErrorCategory::Internal, "shared refdata cache: temp artifact write failed");
  }
  const bool synced = platform::durable_sync(platform::portable_fileno(fp));
  const bool closed = std::fclose(fp) == 0;
  if (!synced || !closed) {
    std::error_code cleanup;
    fs::remove(tmp, cleanup);
    return make_error(ErrorCategory::Internal, "shared refdata cache: temp artifact sync failed");
  }

  // The publish step. rename() replaces the target atomically on POSIX and via
  // MoveFileEx(REPLACE_EXISTING) on Windows, so a concurrent reader sees either
  // the old complete artifact or the new one.
  const int attempts = publish_attempts > 0 ? publish_attempts : 1;
  for (int attempt = 0; attempt < attempts; ++attempt) {
    if (publish(tmp, target)) {
      return std::nullopt;
    }
    if (attempt + 1 < attempts && wait) {
      wait(attempt);
    }
  }

  std::error_code cleanup;
  fs::remove(tmp, cleanup);
  return make_error(ErrorCategory::Transient,
                    "shared refdata cache: could not publish the artifact after bounded retries "
                    "(the target may be held open by a sibling reader)");
}

}  // namespace

Result<ports::Ok> validate_refdata_key(const RefdataKey& key) {
  if (!is_safe_token(key.broker)) {
    return fail(make_error(ErrorCategory::Validation,
                           "shared refdata key: broker must be [a-z0-9-]{1,64} ('_' is reserved as "
                           "the filename separator; lowercase only)"));
  }
  if (!is_safe_token(key.segment)) {
    return fail(make_error(ErrorCategory::Validation,
                           "shared refdata key: segment must be [a-z0-9-]{1,64} ('_' is reserved "
                           "as the filename separator; lowercase only)"));
  }
  if (!is_iso_date(key.trading_date)) {
    return fail(make_error(ErrorCategory::Validation,
                           "shared refdata key: trading_date must be ISO YYYY-MM-DD"));
  }
  if (!is_safe_extension(key.extension)) {
    return fail(make_error(ErrorCategory::Validation,
                           "shared refdata key: extension must look like '.csv'"));
  }
  return ports::ok();
}

RefdataValidateFn csv_sanity_validator() {
  return [](std::string_view payload) {
    const std::string_view trimmed = trim_view(payload);
    if (trimmed.empty()) {
      return false;
    }
    // A header-ish first line: it must carry commas AND at least one letter, so
    // a stray numeric line is not mistaken for an instrument dump.
    const std::size_t break_pos = trimmed.find('\n');
    if (break_pos == std::string_view::npos) {
      return false;  // a one-line "CSV" is a truncated download, not a dump
    }
    const std::string_view header = trim_view(trimmed.substr(0, break_pos));
    if (header.find(',') == std::string_view::npos) {
      return false;
    }
    const bool header_has_letter =
        header.find_first_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") !=
        std::string_view::npos;
    if (!header_has_letter) {
      return false;
    }
    // ...and at least one row after it.
    return !trim_view(trimmed.substr(break_pos + 1)).empty();
  };
}

RefdataValidateFn json_sanity_validator() {
  return [](std::string_view payload) {
    const std::string_view trimmed = trim_view(payload);
    if (trimmed.size() < 2) {
      return false;
    }
    const char first = trimmed.front();
    const char last = trimmed.back();
    return (first == '{' && last == '}') || (first == '[' && last == ']');
  };
}

LockWaitFn default_lock_wait_fn(std::chrono::milliseconds step) {
  return [step](int /*attempt*/) { std::this_thread::sleep_for(step); };
}

PublishRenameFn default_publish_rename_fn() {
  return [](const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    return !ec;
  };
}

Result<ports::Ok> require_outside_account_tree(const fs::path& shared_root,
                                               const fs::path& account_data_root) {
  if (shared_root.empty() || account_data_root.empty()) {
    return fail(make_error(ErrorCategory::Validation,
                           "shared refdata cache: empty shared root or account data root"));
  }

  // A trailing separator leaves an EMPTY final component, which would make the
  // component walk below compare "b" against "" and declare two identical
  // directories disjoint. "/a/b/" and "/a/b" are the same directory, so strip it
  // before comparing — otherwise a stray slash in a config file silently defeats
  // the whole guard.
  const auto canonical_form = [](const fs::path& raw) {
    fs::path normalized = raw.lexically_normal();
    if (!normalized.has_filename()) {
      normalized = normalized.parent_path();
    }
    return normalized;
  };

  const fs::path shared = canonical_form(shared_root);
  const fs::path account_tree = canonical_form(account_data_root);

  // "Is `inner` at or below `outer`?" — a component-wise prefix test, never a
  // string prefix (which would call "/a/bc" a child of "/a/b").
  const auto contains = [](const fs::path& outer, const fs::path& inner) {
    auto o = outer.begin();
    auto i = inner.begin();
    for (; o != outer.end(); ++o, ++i) {
      if (i == inner.end() || *i != *o) {
        return false;
      }
    }
    return true;
  };

  if (contains(account_tree, shared) || contains(shared, account_tree)) {
    return fail(make_error(ErrorCategory::Validation,
                           "shared refdata cache: the shared root must live OUTSIDE the "
                           "per-account data tree"));
  }
  return ports::ok();
}

SharedRefdataCache::SharedRefdataCache(SharedRefdataCacheConfig config)
    : config_(std::move(config)) {
  if (!config_.publish_rename) {
    config_.publish_rename = default_publish_rename_fn();
  }
  // A SIGKILL mid-publish leaves a `.tmp-*`; an aborted takeover can leave a
  // `.stale-*`. Nothing ever reads them, so without this they accumulate in the
  // shared directory forever. Age-gated and best-effort — see sweep_debris().
  (void)sweep_debris();
}

int SharedRefdataCache::sweep_debris() const noexcept {
  int removed = 0;
  try {
    std::error_code ec;
    if (!fs::is_directory(config_.shared_root, ec) || ec) {
      return 0;
    }
    const fs::file_time_type now = fs::file_time_type::clock::now();
    const std::chrono::seconds age = config_.lock_staleness > std::chrono::seconds::zero()
                                         ? config_.lock_staleness
                                         : std::chrono::seconds{600};

    for (const auto& entry : fs::directory_iterator(config_.shared_root, ec)) {
      const std::string name = entry.path().filename().string();
      // ONLY our own debris shapes. An artifact or a live `.lock` is never a
      // candidate — this must not be able to delete the thing it protects.
      const bool is_debris =
          name.find(".tmp-") != std::string::npos || name.find(".stale-") != std::string::npos;
      if (!is_debris) {
        continue;
      }
      std::error_code stat_ec;
      const fs::file_time_type mtime = fs::last_write_time(entry.path(), stat_ec);
      if (stat_ec || mtime > now || (now - mtime) <= age) {
        continue;  // unreadable, future-dated, or possibly still in flight
      }
      std::error_code remove_ec;
      if (fs::remove(entry.path(), remove_ec) && !remove_ec) {
        ++removed;
      }
    }
  } catch (...) {
    // Housekeeping must never take down a caller.
  }
  return removed;
}

fs::path SharedRefdataCache::artifact_path(const RefdataKey& key) const {
  return config_.shared_root /
         (key.broker + "_" + key.segment + "_" + key.trading_date + key.extension);
}

fs::path SharedRefdataCache::lock_path(const RefdataKey& key) const {
  fs::path path = artifact_path(key);
  path += ".lock";
  return path;
}

Result<std::string> SharedRefdataCache::get_or_fetch(const RefdataKey& key,
                                                     const RefdataFetchFn& fetch,
                                                     const RefdataValidateFn& validate) const {
  const Result<ports::Ok> key_ok = validate_refdata_key(key);
  if (!key_ok) {
    return fail(key_ok.error());
  }
  if (config_.shared_root.empty()) {
    return fail(make_error(ErrorCategory::Validation, "shared refdata cache: empty shared root"));
  }
  if (!fetch) {
    return fail(make_error(ErrorCategory::Internal, "shared refdata cache: no fetch seam wired"));
  }
  // ENFORCED, not opt-in: when the caller told us where the account tree is, a
  // shared root inside it is refused here, on the path that would actually
  // create the files — not left to a guard the composition root might forget.
  if (!config_.account_data_root.empty()) {
    const Result<ports::Ok> outside =
        require_outside_account_tree(config_.shared_root, config_.account_data_root);
    if (!outside) {
      return fail(outside.error());
    }
  }

  const fs::path artifact = artifact_path(key);

  // ── 1. FAST PATH: a valid artifact is already there. No lock, no fetch. ──
  if (std::optional<std::string> cached = read_valid_artifact(artifact, validate)) {
    return std::move(*cached);
  }

  std::error_code ec;
  fs::create_directories(config_.shared_root, ec);
  std::error_code dir_ec;
  const bool root_ready = fs::is_directory(config_.shared_root, dir_ec);
  if (dir_ec || !root_ready) {
    return fail(make_error(ErrorCategory::Internal,
                           "shared refdata cache: cannot create the shared cache directory"));
  }

  const fs::path lock_file = lock_path(key);
  // At least one attempt, however the config was set (a zero/negative bound must
  // not silently mean "never try").
  const int attempts = config_.max_lock_attempts > 0 ? config_.max_lock_attempts : 1;

  for (int attempt = 0; attempt < attempts; ++attempt) {
    Result<platform::FileLock> lock =
        platform::try_acquire_file_lock(lock_file, config_.lock_staleness);

    if (lock) {
      // ── 3. DOUBLE-CHECK under the lock: the winner may have just finished. ──
      if (std::optional<std::string> cached = read_valid_artifact(artifact, validate)) {
        return std::move(*cached);
      }

      // ── 4. WE are the winner: fetch, validate, publish atomically. ──
      Result<std::string> fetched = fetch();

      // A reference-data download is the one step here that can run for a long
      // time. Refresh the lock's mtime so a slow-but-healthy holder is not
      // mistaken for a corpse and taken over mid-flight (file_lock guarantee 3).
      (void)lock.value().touch();

      if (!fetched) {
        return fail(fetched.error());  // propagate; the caller's gate blocks
      }
      if (validate && !validate(fetched.value())) {
        // NEVER cache a payload we would refuse to load. Poisoning the shared
        // cache would break every sibling account, not just this one.
        return fail(make_error(ErrorCategory::Validation,
                               "shared refdata cache: fetched artifact failed sanity validation"));
      }

      // LAST CHECK BEFORE PUBLISHING. If our lock was taken over while we were
      // downloading, another process may be writing this very artifact, and two
      // writers is the one thing the lock exists to prevent. Fail closed and let
      // the caller retry rather than race a publish we are not entitled to.
      if (!lock.value().still_ours()) {
        return fail(make_error(ErrorCategory::Transient,
                               "shared refdata cache: lost the refdata lock during the fetch; "
                               "abandoning the publish"));
      }

      const std::optional<errors::Error> write_error =
          write_atomically(artifact, fetched.value(), lock.value().payload().nonce,
                           config_.max_publish_attempts, config_.publish_rename, config_.wait);
      if (write_error.has_value()) {
        return fail(*write_error);
      }
      return std::move(fetched).value();
    }

    // ── 5. WE LOST the race (or the lock is held): re-read, wait, retry. ──
    if (std::optional<std::string> cached = read_valid_artifact(artifact, validate)) {
      return std::move(*cached);
    }
    if (attempt + 1 < attempts && config_.wait) {
      config_.wait(attempt);
    }
  }

  // Bounded and exhausted with nothing to show: FAIL CLOSED. DataStale carries
  // SuggestedAction::BlockStrategy, so safe-start halts rather than trading on
  // reference data we do not have.
  return fail(make_error(ErrorCategory::DataStale,
                         "shared refdata cache: lock held and no artifact appeared within the "
                         "bounded wait; refdata unavailable"));
}

namespace {

// Shared body of the two refdata adapters: build the key at CALL time (so the
// trading date is today's, not wiring-day's) and run the protocol. The cache is
// captured BY VALUE as a shared_ptr — these functions outlive their call site by
// design, so shared ownership is the only lifetime that is correct by
// construction rather than by comment.
[[nodiscard]] RefdataFetchFn make_shared_fetcher(std::shared_ptr<const SharedRefdataCache> cache,
                                                 std::string broker, std::string segment,
                                                 std::string extension,
                                                 std::function<std::string()> trading_date_fn,
                                                 RefdataFetchFn upstream,
                                                 RefdataValidateFn validate) {
  return [cache = std::move(cache), broker = std::move(broker), segment = std::move(segment),
          extension = std::move(extension), trading_date_fn = std::move(trading_date_fn),
          upstream = std::move(upstream), validate = std::move(validate)]() -> Result<std::string> {
    if (!cache) {
      return fail(make_error(ErrorCategory::Internal, "shared refdata cache: no cache wired"));
    }
    if (!trading_date_fn) {
      return fail(
          make_error(ErrorCategory::Internal, "shared refdata cache: no trading-date seam wired"));
    }
    RefdataKey key;
    key.broker = broker;
    key.segment = segment;
    key.trading_date = trading_date_fn();
    key.extension = extension;
    return cache->get_or_fetch(key, upstream, validate);
  };
}

}  // namespace

RefdataFetchFn shared_instrument_csv_fetcher(std::shared_ptr<const SharedRefdataCache> cache,
                                             std::string broker, std::string segment,
                                             std::function<std::string()> trading_date_fn,
                                             RefdataFetchFn upstream, RefdataValidateFn validate) {
  if (!validate) {
    validate = csv_sanity_validator();
  }
  return make_shared_fetcher(std::move(cache), std::move(broker), std::move(segment), ".csv",
                             std::move(trading_date_fn), std::move(upstream), std::move(validate));
}

RefdataFetchFn shared_calendar_json_fetcher(std::shared_ptr<const SharedRefdataCache> cache,
                                            std::string broker,
                                            std::function<std::string()> trading_date_fn,
                                            RefdataFetchFn upstream, RefdataValidateFn validate) {
  if (!validate) {
    validate = json_sanity_validator();
  }
  return make_shared_fetcher(std::move(cache), std::move(broker), "calendar", ".json",
                             std::move(trading_date_fn), std::move(upstream), std::move(validate));
}

}  // namespace broker_exec::accounts
