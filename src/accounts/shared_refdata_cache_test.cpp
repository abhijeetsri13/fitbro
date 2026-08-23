#include "broker_exec/accounts/shared_refdata_cache.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/platform/file_lock.hpp"
#include "broker_exec/refdata/instrument_master.hpp"
#include "broker_exec/refdata/trading_calendar.hpp"

namespace fs = std::filesystem;

using broker_exec::Result;
using broker_exec::accounts::csv_sanity_validator;
using broker_exec::accounts::default_publish_rename_fn;
using broker_exec::accounts::json_sanity_validator;
using broker_exec::accounts::RefdataFetchFn;
using broker_exec::accounts::RefdataKey;
using broker_exec::accounts::RefdataValidateFn;
using broker_exec::accounts::require_outside_account_tree;
using broker_exec::accounts::shared_calendar_json_fetcher;
using broker_exec::accounts::shared_instrument_csv_fetcher;
using broker_exec::accounts::SharedRefdataCache;
using broker_exec::accounts::SharedRefdataCacheConfig;
using broker_exec::accounts::validate_refdata_key;
using broker_exec::clock::TestClock;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::make_error;
using broker_exec::errors::SuggestedAction;
using broker_exec::platform::FileLock;
using broker_exec::platform::try_acquire_file_lock;

namespace {

struct TempDir {
  fs::path path;
  TempDir() {
    std::random_device rd;
    path = fs::temp_directory_path() /
           ("brexec_sharedcache_" + std::to_string(rd()) + "_" + std::to_string(rd()));
    fs::create_directories(path);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

constexpr const char* kDate = "2026-08-10";
constexpr const char* kArtifactName = "kite_nfo_2026-08-10.csv";

[[nodiscard]] RefdataKey csv_key() {
  RefdataKey key;
  key.broker = "kite";
  key.segment = "nfo";
  key.trading_date = kDate;
  key.extension = ".csv";
  return key;
}

// A shared cache whose waits are INJECTED as an EMPTY function — the documented
// "never sleep" opt-out. Production defaults to a real 200 ms sleep, which is
// why every test config must opt out explicitly rather than rely on the default.
[[nodiscard]] SharedRefdataCacheConfig config_for(const fs::path& root, int attempts = 3) {
  SharedRefdataCacheConfig cfg;
  cfg.shared_root = root;
  cfg.lock_staleness = std::chrono::minutes{10};
  cfg.max_lock_attempts = attempts;
  cfg.wait = {};
  return cfg;
}

[[nodiscard]] std::shared_ptr<const SharedRefdataCache> cache_for(const fs::path& root,
                                                                  int attempts = 3) {
  return std::make_shared<const SharedRefdataCache>(config_for(root, attempts));
}

// A counting fetch seam.
struct Fetcher {
  int calls = 0;
  std::string payload;
  std::optional<broker_exec::errors::Error> error;
  std::function<void()> on_call;

  [[nodiscard]] RefdataFetchFn fn() {
    return [this]() -> Result<std::string> {
      ++calls;
      if (on_call) {
        on_call();
      }
      if (error.has_value()) {
        return broker_exec::fail(*error);
      }
      return payload;
    };
  }
};

[[nodiscard]] std::string read_all(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_text(const fs::path& path, std::string_view text) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

void backdate(const fs::path& path, std::chrono::seconds age) {
  std::error_code ec;
  fs::last_write_time(path, fs::file_time_type::clock::now() - age, ec);
  REQUIRE(!ec);
}

// Any leftover temp file would mean the atomic-publish step leaked.
[[nodiscard]] int count_matching(const fs::path& dir, std::string_view needle) {
  int n = 0;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (entry.path().filename().string().find(needle) != std::string::npos) {
      ++n;
    }
  }
  return n;
}

constexpr const char* kInstrumentsCsv =
    "instrument_token,tradingsymbol,exchange,expiry,tick_size,lot_size\n"
    "256265,NIFTY26AUG24000CE,NFO,2026-08-27,0.05,75\n"
    "260105,BANKNIFTY26AUG52000PE,NFO,2026-08-27,0.05,15\n";

constexpr const char* kCalendarJson =
    "{\"holidays\":[\"2026-08-15\"],\"special_sessions\":[],"
    "\"windows\":{\"open\":\"09:15\",\"entry_cutoff\":\"15:00\","
    "\"square_off\":\"15:15\",\"close\":\"15:30\"}}";

[[nodiscard]] TestClock clock_on(int year, unsigned month, unsigned day) {
  return TestClock(
      std::chrono::steady_clock::time_point{},
      std::chrono::system_clock::time_point(std::chrono::sys_days{
          std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day}}));
}

}  // namespace

TEST_CASE("miss -> fetch -> atomic write -> hit (the second caller never fetches)",
          "[accounts][cache]") {
  const TempDir dir;
  const SharedRefdataCache cache(config_for(dir.path / "shared"));
  const RefdataKey key = csv_key();

  Fetcher fetcher;
  fetcher.payload = kInstrumentsCsv;

  Result<std::string> first = cache.get_or_fetch(key, fetcher.fn(), csv_sanity_validator());
  REQUIRE(first.has_value());
  CHECK(first.value() == kInstrumentsCsv);
  CHECK(fetcher.calls == 1);

  const fs::path artifact = cache.artifact_path(key);
  REQUIRE(fs::exists(artifact));
  CHECK(read_all(artifact) == kInstrumentsCsv);
  CHECK(artifact.filename().string() == kArtifactName);

  // The FAST PATH: a valid artifact is returned without touching the fetch seam
  // or the lock. NOTE: this is the UNCONTENDED path — the contended
  // winner/loser interleaving is exercised by the lock-held test below.
  Result<std::string> second = cache.get_or_fetch(key, fetcher.fn(), csv_sanity_validator());
  REQUIRE(second.has_value());
  CHECK(second.value() == kInstrumentsCsv);
  CHECK(fetcher.calls == 1);  // still ONE download for N readers

  // The lock file is released, and nothing is left half-written.
  CHECK_FALSE(fs::exists(cache.lock_path(key)));
  CHECK(count_matching(artifact.parent_path(), ".tmp-") == 0);
}

TEST_CASE("a SECOND cache object over the same root reads the first one's artifact (fast path)",
          "[accounts][cache]") {
  const TempDir dir;
  const fs::path shared = dir.path / "shared";
  // Two "processes": two independent cache objects over the SAME shared root.
  // They do not contend here — the winner has already finished — so this pins
  // the serial hand-off, not the race.
  const SharedRefdataCache winner(config_for(shared));
  const SharedRefdataCache reader(config_for(shared));
  const RefdataKey key = csv_key();

  Fetcher winner_fetch;
  winner_fetch.payload = kInstrumentsCsv;
  Fetcher reader_fetch;
  reader_fetch.payload = "should,never\nbe,fetched\n";

  REQUIRE(winner.get_or_fetch(key, winner_fetch.fn(), csv_sanity_validator()).has_value());

  Result<std::string> reader_result =
      reader.get_or_fetch(key, reader_fetch.fn(), csv_sanity_validator());
  REQUIRE(reader_result.has_value());
  CHECK(reader_result.value() == kInstrumentsCsv);  // the WINNER's artifact
  CHECK(reader_fetch.calls == 0);
  CHECK(winner_fetch.calls == 1);
}

TEST_CASE("CONTENDED: a loser blocked on the lock waits, re-reads, and takes the winner's artifact",
          "[accounts][cache]") {
  const TempDir dir;
  const fs::path shared = dir.path / "shared";
  const RefdataKey key = csv_key();

  SharedRefdataCacheConfig cfg = config_for(shared, 5);
  // Simulate a sibling process holding the lock while it downloads.
  fs::create_directories(shared);
  Result<FileLock> held = try_acquire_file_lock(shared / (std::string(kArtifactName) + ".lock"),
                                                std::chrono::minutes{10});
  REQUIRE(held.has_value());

  // The injected wait is where the "other process" makes progress: on the second
  // attempt it publishes its artifact and releases the lock. No sleeping, no
  // threads, fully deterministic.
  int waits = 0;
  const fs::path artifact = shared / kArtifactName;
  cfg.wait = [&](int /*attempt*/) {
    ++waits;
    if (waits == 2) {
      write_text(artifact, kInstrumentsCsv);
      held.value().release();
    }
  };

  const SharedRefdataCache cache(cfg);
  Fetcher fetcher;
  fetcher.payload = "our,own\ndownload,x\n";

  Result<std::string> result = cache.get_or_fetch(key, fetcher.fn(), csv_sanity_validator());
  REQUIRE(result.has_value());
  CHECK(result.value() == kInstrumentsCsv);  // the winner's bytes, not ours
  CHECK(fetcher.calls == 0);                 // we never downloaded a second copy
  CHECK(waits == 2);
}

TEST_CASE("lock held and NO artifact appears: bounded failure, fail closed", "[accounts][cache]") {
  const TempDir dir;
  const fs::path shared = dir.path / "shared";
  const RefdataKey key = csv_key();

  fs::create_directories(shared);
  Result<FileLock> held = try_acquire_file_lock(shared / (std::string(kArtifactName) + ".lock"),
                                                std::chrono::minutes{10});
  REQUIRE(held.has_value());

  SharedRefdataCacheConfig cfg = config_for(shared, 4);
  int waits = 0;
  cfg.wait = [&waits](int /*attempt*/) { ++waits; };
  const SharedRefdataCache cache(cfg);

  Fetcher fetcher;
  fetcher.payload = kInstrumentsCsv;

  Result<std::string> result = cache.get_or_fetch(key, fetcher.fn(), csv_sanity_validator());
  REQUIRE_FALSE(result.has_value());
  // DataStale carries BlockStrategy: safe-start halts rather than trading blind.
  CHECK(result.error().category == ErrorCategory::DataStale);
  CHECK(result.error().action == SuggestedAction::BlockStrategy);
  CHECK(fetcher.calls == 0);  // NEVER "give up and fetch anyway"
  CHECK(waits == 3);          // bounded: attempts-1 waits, then it stops
}

TEST_CASE("a CORRUPT cached artifact is re-fetched, not returned and not crashed on",
          "[accounts][cache]") {
  const TempDir dir;
  const SharedRefdataCache cache(config_for(dir.path / "shared"));
  const RefdataKey key = csv_key();

  // Garbage that the sanity validator refuses (an error page, not a CSV).
  write_text(cache.artifact_path(key), "<html>502 Bad Gateway</html>");

  Fetcher fetcher;
  fetcher.payload = kInstrumentsCsv;

  Result<std::string> result = cache.get_or_fetch(key, fetcher.fn(), csv_sanity_validator());
  REQUIRE(result.has_value());
  CHECK(result.value() == kInstrumentsCsv);
  CHECK(fetcher.calls == 1);
  CHECK(read_all(cache.artifact_path(key)) == kInstrumentsCsv);  // the corpse was replaced
}

TEST_CASE("a TRUNCATED cached artifact (header only) is re-fetched", "[accounts][cache]") {
  const TempDir dir;
  const SharedRefdataCache cache(config_for(dir.path / "shared"));
  const RefdataKey key = csv_key();

  // The realistic half-download: a valid header line and nothing after it.
  write_text(cache.artifact_path(key),
             "instrument_token,tradingsymbol,exchange,expiry,tick_size,lot_size\n");

  Fetcher fetcher;
  fetcher.payload = kInstrumentsCsv;
  Result<std::string> result = cache.get_or_fetch(key, fetcher.fn(), csv_sanity_validator());
  REQUIRE(result.has_value());
  CHECK(fetcher.calls == 1);
  CHECK(read_all(cache.artifact_path(key)) == kInstrumentsCsv);
}

TEST_CASE("an EMPTY cached artifact is re-fetched (empty is corrupt, not 'no data')",
          "[accounts][cache]") {
  const TempDir dir;
  const SharedRefdataCache cache(config_for(dir.path / "shared"));
  const RefdataKey key = csv_key();

  write_text(cache.artifact_path(key), "");
  REQUIRE(fs::exists(cache.artifact_path(key)));

  Fetcher fetcher;
  fetcher.payload = kInstrumentsCsv;
  Result<std::string> result = cache.get_or_fetch(key, fetcher.fn(), csv_sanity_validator());
  REQUIRE(result.has_value());
  CHECK(fetcher.calls == 1);
  CHECK(read_all(cache.artifact_path(key)) == kInstrumentsCsv);
}

TEST_CASE("the artifact name only ever appears fully written (temp + rename)",
          "[accounts][cache]") {
  const TempDir dir;
  const SharedRefdataCache cache(config_for(dir.path / "shared"));
  const RefdataKey key = csv_key();
  const fs::path artifact = cache.artifact_path(key);

  Fetcher fetcher;
  fetcher.payload = kInstrumentsCsv;
  bool artifact_absent_during_fetch = false;
  fetcher.on_call = [&]() { artifact_absent_during_fetch = !fs::exists(artifact); };

  REQUIRE(cache.get_or_fetch(key, fetcher.fn(), csv_sanity_validator()).has_value());

  // While the download was in flight the artifact NAME did not exist at all —
  // a reader could not have seen a partial file under it.
  CHECK(artifact_absent_during_fetch);
  CHECK(fs::exists(artifact));
  CHECK(read_all(artifact) == kInstrumentsCsv);
  CHECK(count_matching(artifact.parent_path(), ".tmp-") == 0);  // renamed, not leaked
}

TEST_CASE("the publish rename is RETRIED and a persistent failure is Transient, not Internal",
          "[accounts][cache]") {
  const TempDir dir;
  const RefdataKey key = csv_key();

  SECTION("a transient rename failure is absorbed by the retry") {
    // On Windows a rename onto a target a sibling currently has OPEN FOR READ
    // fails with ERROR_ACCESS_DENIED. It is transient; the retry rides it out.
    SharedRefdataCacheConfig cfg = config_for(dir.path / "shared");
    cfg.max_publish_attempts = 5;
    int publish_calls = 0;
    int publish_waits = 0;
    cfg.wait = [&publish_waits](int /*attempt*/) { ++publish_waits; };
    const auto real_rename = default_publish_rename_fn();
    cfg.publish_rename = [&publish_calls, real_rename](const fs::path& from, const fs::path& to) {
      ++publish_calls;
      return publish_calls >= 3 && real_rename(from, to);
    };

    const SharedRefdataCache cache(cfg);
    Fetcher fetcher;
    fetcher.payload = kInstrumentsCsv;

    Result<std::string> result = cache.get_or_fetch(key, fetcher.fn(), csv_sanity_validator());
    REQUIRE(result.has_value());
    CHECK(publish_calls == 3);
    CHECK(publish_waits == 2);
    CHECK(read_all(cache.artifact_path(key)) == kInstrumentsCsv);
  }

  SECTION("a persistent rename failure is classified RetrySafe and leaves no temp behind") {
    SharedRefdataCacheConfig cfg = config_for(dir.path / "shared2");
    cfg.max_publish_attempts = 4;
    int publish_calls = 0;
    cfg.publish_rename = [&publish_calls](const fs::path&, const fs::path&) {
      ++publish_calls;
      return false;
    };

    const SharedRefdataCache cache(cfg);
    Fetcher fetcher;
    fetcher.payload = kInstrumentsCsv;

    Result<std::string> result = cache.get_or_fetch(key, fetcher.fn(), csv_sanity_validator());
    REQUIRE_FALSE(result.has_value());
    // Transient/RetrySafe, NOT Internal/RaiseAlert: a busy target is a reason to
    // come back, not a reason to page a human.
    CHECK(result.error().category == ErrorCategory::Transient);
    CHECK(result.error().action == SuggestedAction::RetrySafe);
    CHECK(publish_calls == 4);
    CHECK_FALSE(fs::exists(cache.artifact_path(key)));
    CHECK(count_matching(cache.artifact_path(key).parent_path(), ".tmp-") == 0);
  }
}

TEST_CASE("a fetch Error propagates and nothing is cached", "[accounts][cache]") {
  const TempDir dir;
  const SharedRefdataCache cache(config_for(dir.path / "shared"));
  const RefdataKey key = csv_key();

  Fetcher fetcher;
  fetcher.error = make_error(ErrorCategory::Network, "refdata download failed");

  Result<std::string> result = cache.get_or_fetch(key, fetcher.fn(), csv_sanity_validator());
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::Network);
  CHECK_FALSE(fs::exists(cache.artifact_path(key)));
  CHECK_FALSE(fs::exists(cache.lock_path(key)));  // the lock was released
}

TEST_CASE("a fetched payload that fails validation is NEVER written to the shared cache",
          "[accounts][cache]") {
  const TempDir dir;
  const SharedRefdataCache cache(config_for(dir.path / "shared"));
  const RefdataKey key = csv_key();

  Fetcher fetcher;
  fetcher.payload = "<html>rate limited</html>";  // fails the CSV sanity check

  Result<std::string> result = cache.get_or_fetch(key, fetcher.fn(), csv_sanity_validator());
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::Validation);
  // Poisoning the SHARED cache would break every sibling account, not just us.
  CHECK_FALSE(fs::exists(cache.artifact_path(key)));
}

TEST_CASE("a lock lost DURING the fetch abandons the publish (fail closed)", "[accounts][cache]") {
  const TempDir dir;
  const SharedRefdataCache cache(config_for(dir.path / "shared"));
  const RefdataKey key = csv_key();

  // While "downloading", our lock is taken over by another process — simulated
  // by overwriting the lock file with a foreign payload. Publishing now would
  // mean two writers, the one thing the lock exists to prevent.
  Fetcher fetcher;
  fetcher.payload = kInstrumentsCsv;
  fetcher.on_call = [&]() {
    broker_exec::platform::LockPayload other;
    other.pid = 999999;
    other.acquired_at = "2026-08-09T09:15:00Z";
    other.nonce = "someone-else";
    write_text(cache.lock_path(key), broker_exec::platform::format_lock_payload(other));
  };

  Result<std::string> result = cache.get_or_fetch(key, fetcher.fn(), csv_sanity_validator());
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::Transient);
  CHECK(fetcher.calls == 1);
  CHECK_FALSE(fs::exists(cache.artifact_path(key)));  // nothing was published
}

TEST_CASE("the key is validated: it becomes a filename, so it is never a path",
          "[accounts][cache]") {
  RefdataKey key = csv_key();
  CHECK(validate_refdata_key(key).has_value());

  const auto rejected = [](RefdataKey k) {
    const Result<broker_exec::ports::Ok> r = validate_refdata_key(k);
    return !r.has_value() && r.error().category == ErrorCategory::Validation;
  };

  key = csv_key();
  key.broker = "../etc";
  CHECK(rejected(key));
  key = csv_key();
  key.broker = "";
  CHECK(rejected(key));
  key = csv_key();
  key.segment = "n/fo";
  CHECK(rejected(key));
  key = csv_key();
  key.segment = "nfo\\x";
  CHECK(rejected(key));
  key = csv_key();
  key.segment = "NFO";  // uppercase would alias on a case-insensitive volume
  CHECK(rejected(key));
  key = csv_key();
  key.trading_date = "2026/08/10";
  CHECK(rejected(key));
  key = csv_key();
  key.trading_date = "10-08-2026x";
  CHECK(rejected(key));
  key = csv_key();
  key.trading_date = "";
  CHECK(rejected(key));
  key = csv_key();
  key.extension = "csv";  // no leading dot
  CHECK(rejected(key));
  key = csv_key();
  key.extension = "./x";
  CHECK(rejected(key));
}

TEST_CASE("the key's UNDERSCORE is reserved as the separator (the filename collision)",
          "[accounts][cache]") {
  // ("kite", "nfo-opt") and ("kite-nfo", "opt") are distinct keys. Were '_'
  // allowed inside a component, ("kite","nfo_opt") and ("kite_nfo","opt") would
  // produce the IDENTICAL artifact AND lock path — two unrelated data sets
  // silently sharing one cache entry. Both spellings are rejected outright.
  RefdataKey a = csv_key();
  a.broker = "kite";
  a.segment = "nfo_opt";
  RefdataKey b = csv_key();
  b.broker = "kite_nfo";
  b.segment = "opt";

  CHECK_FALSE(validate_refdata_key(a).has_value());
  CHECK_FALSE(validate_refdata_key(b).has_value());

  // The '-' spellings ARE allowed and are provably distinct entries.
  const TempDir dir;
  const SharedRefdataCache cache(config_for(dir.path / "shared"));
  RefdataKey a_ok = a;
  a_ok.segment = "nfo-opt";
  RefdataKey b_ok = b;
  b_ok.broker = "kite-nfo";
  REQUIRE(validate_refdata_key(a_ok).has_value());
  REQUIRE(validate_refdata_key(b_ok).has_value());
  CHECK(cache.artifact_path(a_ok) != cache.artifact_path(b_ok));
  CHECK(cache.lock_path(a_ok) != cache.lock_path(b_ok));
}

TEST_CASE("the shared root must live OUTSIDE the per-account data tree", "[accounts][cache]") {
  CHECK(require_outside_account_tree("/var/lib/broker-exec/shared", "/var/lib/broker-exec/accounts")
            .has_value());

  // Inside the account tree — rejected.
  CHECK_FALSE(require_outside_account_tree("/var/lib/broker-exec/accounts/a/shared",
                                           "/var/lib/broker-exec/accounts")
                  .has_value());
  // The account tree inside the shared root — also rejected (same hazard).
  CHECK_FALSE(require_outside_account_tree("/var/lib/broker-exec", "/var/lib/broker-exec/accounts")
                  .has_value());
  // Identical paths.
  CHECK_FALSE(require_outside_account_tree("/data", "/data").has_value());
  // A string prefix that is NOT a path prefix is fine ("accounts2" is not inside
  // "accounts").
  CHECK(require_outside_account_tree("/var/lib/accounts2", "/var/lib/accounts").has_value());
  // ".." is normalized before the comparison.
  CHECK_FALSE(
      require_outside_account_tree("/var/lib/accounts/x/../y", "/var/lib/accounts").has_value());
  CHECK_FALSE(require_outside_account_tree("", "/var/lib/accounts").has_value());
}

TEST_CASE("a TRAILING SEPARATOR does not defeat the outside-the-account-tree guard",
          "[accounts][cache]") {
  // "/a/b/" and "/a/b" are the same directory. Left unhandled, the trailing
  // separator leaves an empty final component and the component walk declares
  // two identical directories disjoint — a stray slash in a config file would
  // silently switch the guard off.
  CHECK_FALSE(require_outside_account_tree("/data/", "/data").has_value());
  CHECK_FALSE(require_outside_account_tree("/data", "/data/").has_value());
  CHECK_FALSE(require_outside_account_tree("/data/", "/data/").has_value());
  CHECK_FALSE(
      require_outside_account_tree("/var/lib/accounts/shared/", "/var/lib/accounts").has_value());
  CHECK_FALSE(
      require_outside_account_tree("/var/lib/accounts/shared", "/var/lib/accounts/").has_value());
  // A genuinely disjoint pair still passes with trailing separators.
  CHECK(require_outside_account_tree("/var/lib/shared/", "/var/lib/accounts/").has_value());
}

TEST_CASE("when an account root is configured the guard is ENFORCED, not optional",
          "[accounts][cache]") {
  const TempDir dir;
  const RefdataKey key = csv_key();

  SharedRefdataCacheConfig cfg = config_for(dir.path / "accounts" / "alpha" / "shared");
  cfg.account_data_root = dir.path / "accounts";
  const SharedRefdataCache cache(cfg);

  Fetcher fetcher;
  fetcher.payload = kInstrumentsCsv;

  Result<std::string> result = cache.get_or_fetch(key, fetcher.fn(), csv_sanity_validator());
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::Validation);
  CHECK(fetcher.calls == 0);                          // refused before any I/O
  CHECK_FALSE(fs::exists(cache.artifact_path(key)));  // and nothing was created

  // A correctly-placed shared root with the same enforcement works.
  SharedRefdataCacheConfig ok_cfg = config_for(dir.path / "shared");
  ok_cfg.account_data_root = dir.path / "accounts";
  const SharedRefdataCache ok_cache(ok_cfg);
  CHECK(ok_cache.get_or_fetch(key, fetcher.fn(), csv_sanity_validator()).has_value());
  CHECK(fetcher.calls == 1);
}

TEST_CASE("sweep_debris removes ONLY aged temp/claim residue", "[accounts][cache]") {
  const TempDir dir;
  const fs::path shared = dir.path / "shared";
  const RefdataKey key = csv_key();

  // A SIGKILL mid-publish and an aborted takeover leave these behind.
  const fs::path old_tmp = shared / (std::string(kArtifactName) + ".tmp-dead-1");
  const fs::path old_claim = shared / (std::string(kArtifactName) + ".lock.stale-dead-2");
  const fs::path fresh_tmp = shared / (std::string(kArtifactName) + ".tmp-live-3");
  const fs::path artifact = shared / kArtifactName;
  const fs::path live_lock = shared / (std::string(kArtifactName) + ".lock");

  write_text(old_tmp, "partial");
  write_text(old_claim, "claimed corpse");
  write_text(fresh_tmp, "a publish in flight right now");
  write_text(artifact, kInstrumentsCsv);
  write_text(live_lock, "a live lock");
  backdate(old_tmp, std::chrono::hours{2});
  backdate(old_claim, std::chrono::hours{2});
  backdate(artifact, std::chrono::hours{48});  // old, but it is the DATA

  // Construction sweeps once.
  const SharedRefdataCache cache(config_for(shared));

  CHECK_FALSE(fs::exists(old_tmp));
  CHECK_FALSE(fs::exists(old_claim));
  // In-flight residue is age-gated out: never disturb a publish in progress.
  CHECK(fs::exists(fresh_tmp));
  // The artifact and the lock are NEVER candidates, however old.
  CHECK(fs::exists(artifact));
  CHECK(fs::exists(live_lock));
  CHECK(read_all(artifact) == kInstrumentsCsv);
  CHECK(cache.artifact_path(key) == artifact);

  // Explicit sweeps are idempotent and report what they removed.
  CHECK(cache.sweep_debris() == 0);
  backdate(fresh_tmp, std::chrono::hours{2});
  CHECK(cache.sweep_debris() == 1);
  CHECK_FALSE(fs::exists(fresh_tmp));

  // A missing shared root is not an error, just nothing to do.
  const SharedRefdataCache absent(config_for(dir.path / "does-not-exist"));
  CHECK(absent.sweep_debris() == 0);
}

TEST_CASE("the sanity validators reject the classic corrupt-cache payloads", "[accounts][cache]") {
  const RefdataValidateFn csv = csv_sanity_validator();
  CHECK(csv("a,b\n1,2\n"));
  CHECK(csv(kInstrumentsCsv));
  CHECK_FALSE(csv(""));
  CHECK_FALSE(csv("   \n\t "));
  CHECK_FALSE(csv("no commas here"));
  CHECK_FALSE(csv("a,b\n"));       // header with no rows — a truncated download
  CHECK_FALSE(csv("a,b"));         // one line, no newline at all
  CHECK_FALSE(csv("1,2\n3,4\n"));  // no letters in the header: not a dump header

  const RefdataValidateFn json = json_sanity_validator();
  CHECK(json("{\"a\":1}"));
  CHECK(json("  [1,2]  "));
  CHECK(json(kCalendarJson));
  CHECK_FALSE(json(""));
  CHECK_FALSE(json("{\"a\":1"));  // truncated mid-document
  CHECK_FALSE(json("<html></html>"));
}

TEST_CASE("composition: TWO InstrumentMasters over one shared cache download ONCE",
          "[accounts][cache][refdata]") {
  const TempDir dir;
  const std::shared_ptr<const SharedRefdataCache> cache = cache_for(dir.path / "shared");

  // The upstream downloader the composition root would wire (here: a fake).
  Fetcher upstream;
  upstream.payload = kInstrumentsCsv;

  const auto date_fn = []() { return std::string(kDate); };

  // Account A and account B — two processes in production, two objects here.
  const TestClock clock = clock_on(2026, 8, 10);
  broker_exec::refdata::InstrumentMaster master_a(
      shared_instrument_csv_fetcher(cache, "kite", "nfo", date_fn, upstream.fn()), clock,
      dir.path / "acct_a" / "refdata", "kite", "nfo");
  broker_exec::refdata::InstrumentMaster master_b(
      shared_instrument_csv_fetcher(cache, "kite", "nfo", date_fn, upstream.fn()), clock,
      dir.path / "acct_b" / "refdata", "kite", "nfo");

  REQUIRE(master_a.refresh().has_value());
  REQUIRE(master_b.refresh().has_value());

  // ONE download served BOTH accounts — the whole point of the shared cache.
  CHECK(upstream.calls == 1);

  CHECK(master_a.is_fresh());
  CHECK(master_b.is_fresh());
  REQUIRE(master_a.resolve("NFO", "NIFTY26AUG24000CE").has_value());
  REQUIRE(master_b.resolve("NFO", "NIFTY26AUG24000CE").has_value());
  CHECK(master_a.resolve("NFO", "NIFTY26AUG24000CE").value().token ==
        master_b.resolve("NFO", "NIFTY26AUG24000CE").value().token);

  // Each account still keeps its OWN date-versioned copy (refdata's own cache),
  // in its OWN tree — the shared cache sits in front of the download, it does not
  // merge the accounts' private state.
  CHECK(fs::exists(dir.path / "acct_a" / "refdata" / kArtifactName));
  CHECK(fs::exists(dir.path / "acct_b" / "refdata" / kArtifactName));
}

TEST_CASE("composition: the fetcher keeps the cache alive (shared ownership)",
          "[accounts][cache][refdata]") {
  const TempDir dir;
  Fetcher upstream;
  upstream.payload = kInstrumentsCsv;

  RefdataFetchFn fetcher;
  {
    // The composition root's local handle goes out of scope; the fetcher it
    // produced is still wired into a long-lived InstrumentMaster.
    const std::shared_ptr<const SharedRefdataCache> cache = cache_for(dir.path / "shared");
    fetcher = shared_instrument_csv_fetcher(
        cache, "kite", "nfo", []() { return std::string(kDate); }, upstream.fn());
  }

  const Result<std::string> result = fetcher();
  REQUIRE(result.has_value());
  CHECK(result.value() == kInstrumentsCsv);
  CHECK(upstream.calls == 1);
}

TEST_CASE("composition: TWO TradingCalendars over one shared cache download ONCE",
          "[accounts][cache][refdata]") {
  const TempDir dir;
  const std::shared_ptr<const SharedRefdataCache> cache = cache_for(dir.path / "shared");

  Fetcher upstream;
  upstream.payload = kCalendarJson;
  const auto date_fn = []() { return std::string(kDate); };

  const TestClock clock = clock_on(2026, 8, 10);
  broker_exec::refdata::TradingCalendar cal_a(
      shared_calendar_json_fetcher(cache, "kite", date_fn, upstream.fn()), clock,
      dir.path / "acct_a" / "refdata", "kite");
  broker_exec::refdata::TradingCalendar cal_b(
      shared_calendar_json_fetcher(cache, "kite", date_fn, upstream.fn()), clock,
      dir.path / "acct_b" / "refdata", "kite");

  REQUIRE(cal_a.refresh().has_value());
  REQUIRE(cal_b.refresh().has_value());
  CHECK(upstream.calls == 1);

  // The shared artifact is keyed with the fixed "calendar" segment and .json.
  CHECK(fs::exists(cache->artifact_path(RefdataKey{"kite", "calendar", kDate, ".json"})));
}

TEST_CASE("composition: an upstream failure still propagates through the cache adapter",
          "[accounts][cache][refdata]") {
  const TempDir dir;
  const std::shared_ptr<const SharedRefdataCache> cache = cache_for(dir.path / "shared");

  Fetcher upstream;
  upstream.error = make_error(ErrorCategory::Network, "instruments download failed");
  const auto date_fn = []() { return std::string(kDate); };

  const TestClock clock = clock_on(2026, 8, 10);
  broker_exec::refdata::InstrumentMaster master(
      shared_instrument_csv_fetcher(cache, "kite", "nfo", date_fn, upstream.fn()), clock,
      dir.path / "acct_a" / "refdata", "kite", "nfo");

  const auto refreshed = master.refresh();
  REQUIRE_FALSE(refreshed.has_value());
  CHECK(refreshed.error().category == ErrorCategory::Network);
  CHECK_FALSE(master.is_fresh());  // the safe-start gate stays shut
}

TEST_CASE("composition: a missing trading-date seam or cache fails closed",
          "[accounts][cache][refdata]") {
  const TempDir dir;
  const std::shared_ptr<const SharedRefdataCache> cache = cache_for(dir.path / "shared");
  Fetcher upstream;
  upstream.payload = kInstrumentsCsv;

  const RefdataFetchFn no_date =
      shared_instrument_csv_fetcher(cache, "kite", "nfo", {}, upstream.fn());
  const Result<std::string> date_result = no_date();
  REQUIRE_FALSE(date_result.has_value());
  CHECK(date_result.error().category == ErrorCategory::Internal);

  const RefdataFetchFn no_cache = shared_instrument_csv_fetcher(
      nullptr, "kite", "nfo", []() { return std::string(kDate); }, upstream.fn());
  const Result<std::string> cache_result = no_cache();
  REQUIRE_FALSE(cache_result.has_value());
  CHECK(cache_result.error().category == ErrorCategory::Internal);

  CHECK(upstream.calls == 0);
}
