#include "broker_exec/accounts/account_data_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "broker_exec/errors/error.hpp"

namespace fs = std::filesystem;

using broker_exec::Result;
using broker_exec::accounts::AccountDataDir;
using broker_exec::accounts::DirPermissionFn;
using broker_exec::accounts::kMaxAccountIdLength;
using broker_exec::accounts::validate_account_id;
using broker_exec::errors::ErrorCategory;
using broker_exec::ports::Ok;

namespace {

struct TempDir {
  fs::path path;
  TempDir() {
    std::random_device rd;
    path = fs::temp_directory_path() /
           ("brexec_accounts_" + std::to_string(rd()) + "_" + std::to_string(rd()));
    fs::create_directories(path);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

// A spy over the permission seam: records every directory it was asked to
// tighten and can be made to fail on demand.
struct PermsSpy {
  std::vector<fs::path> calls;
  bool succeed = true;
};

[[nodiscard]] DirPermissionFn spy_fn(PermsSpy& spy) {
  return [&spy](const fs::path& path) {
    spy.calls.push_back(path);
    return spy.succeed;
  };
}

// Is `inner` at or below `outer`, component-wise (never a string prefix test —
// that would call "root/ab" a child of "root/a").
[[nodiscard]] bool contains(const fs::path& outer, const fs::path& inner) {
  auto o = outer.begin();
  auto i = inner.begin();
  for (; o != outer.end(); ++o, ++i) {
    if (i == inner.end() || *i != *o) {
      return false;
    }
  }
  return true;
}

}  // namespace

TEST_CASE("account id validation accepts the documented charset", "[accounts]") {
  for (const char* id : {"acct1", "a", "9", "kite-primary", "kotak_hedge", "a-b_c-1",
                         "0123456789abcdefghijklmnopqrstuvwxyz-_"}) {
    INFO("id = " << id);
    CHECK(validate_account_id(id).has_value());
  }
  // Exactly at the length bound.
  CHECK(validate_account_id(std::string(kMaxAccountIdLength, 'a')).has_value());
}

TEST_CASE("account ids are LOWERCASE ONLY: 'AB' and 'ab' would be one directory", "[accounts]") {
  // NTFS and stock APFS/HFS+ are case-insensitive, so accepting both spellings
  // would hand two accounts the SAME intent log and SQLite file. The rule is
  // enforced by REJECTION, never by silently lowercasing (which would merge the
  // two accounts instead of refusing them).
  CHECK(validate_account_id("ab").has_value());
  for (const char* id : {"AB", "Ab", "aB", "Acct1", "KITE-PRIMARY", "kotak_Hedge"}) {
    INFO("id = " << id);
    const Result<Ok> result = validate_account_id(id);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().category == ErrorCategory::Validation);
  }

  // And the layout helper refuses it too — the rule is not bypassable by going
  // straight to create().
  const TempDir dir;
  CHECK_FALSE(AccountDataDir::create(dir.path, "AB").has_value());
  CHECK(AccountDataDir::create(dir.path, "ab").has_value());
}

TEST_CASE("account id validation is fail-closed: an id is a NAME, not a path", "[accounts]") {
  const std::vector<std::string> rejected{
      "",                                     // empty
      std::string(kMaxAccountIdLength + 1, 'a'),  // overlong
      "..",                                   // parent traversal
      ".",                                    // current dir
      "../evil",                              // traversal with separator
      "..\\evil",                             // Windows traversal
      "a/b",                                  // POSIX separator
      "a\\b",                                 // Windows separator
      "/abs",                                 // absolute POSIX
      "C:",                                   // drive letter
      "C:/x",                                 // drive-qualified path
      "acct.1",                               // dot (would create an extension)
      "acct 1",                               // space
      "acct\t1",                              // control character
      std::string("acct\0hidden", 11),        // embedded NUL
      "acct*",                                // wildcard
      "acct?",                                // wildcard
      "acct|pipe",                            // shell metacharacter
      "acct:stream",                          // NTFS alternate data stream
      "acct\"q",                              // quote
      "~",                                    // home expansion
      "$HOME",                                // env expansion
      "%APPDATA%",                            // Windows env expansion
      "acct\n",                               // newline (log injection)
      "con",  "nul",  "aux", "com1", "lpt9",  // Windows reserved device names
      "CON",  "Nul",                          // ...and their uppercase spellings
      "AB",   "Acct1",                        // uppercase: one directory on NTFS/APFS
  };

  for (const std::string& id : rejected) {
    INFO("rejected id index/len = " << id.size());
    const Result<Ok> result = validate_account_id(id);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().category == ErrorCategory::Validation);
  }
}

TEST_CASE("create rejects an invalid id and an empty data root", "[accounts]") {
  const TempDir dir;
  CHECK_FALSE(AccountDataDir::create(dir.path, "../escape").has_value());
  CHECK_FALSE(AccountDataDir::create(dir.path, "").has_value());
  CHECK_FALSE(AccountDataDir::create(fs::path{}, "acct1").has_value());
}

TEST_CASE("the layout puts every per-account file inside one id-derived directory",
          "[accounts]") {
  const TempDir dir;
  Result<AccountDataDir> account = AccountDataDir::create(dir.path, "acct1");
  REQUIRE(account.has_value());

  const AccountDataDir& a = account.value();
  CHECK(a.account_id() == "acct1");
  CHECK(a.root() == dir.path / "acct1");
  CHECK(a.root().parent_path() == dir.path);  // EXACTLY one id-derived level

  for (const fs::path& p : {a.intent_log(), a.store_db(), a.token_store(), a.ledger(),
                            a.kill_journal()}) {
    INFO("path = " << p.string());
    CHECK(contains(a.root(), p));
  }

  // The five paths are distinct files (no two subsystems share a file).
  const std::vector<fs::path> paths{a.intent_log(), a.store_db(), a.token_store(), a.ledger(),
                                    a.kill_journal()};
  for (std::size_t i = 0; i < paths.size(); ++i) {
    for (std::size_t j = i + 1; j < paths.size(); ++j) {
      CHECK(paths[i] != paths[j]);
    }
  }
}

TEST_CASE("DISTINCT ids yield provably disjoint trees (the concatenation trap)", "[accounts]") {
  const TempDir dir;

  // The classic collision: "ab"+"c" vs "a"+"bc" would meet under a naive
  // two-level layout. With ONE validated level per id they cannot.
  const std::vector<std::string> ids{"a", "ab", "abc", "a-b", "ab-c", "a_bc"};

  std::vector<fs::path> roots;
  for (const std::string& id : ids) {
    Result<AccountDataDir> account = AccountDataDir::create(dir.path, id);
    REQUIRE(account.has_value());
    REQUIRE(account.value().ensure().has_value());  // materialize them for real
    roots.push_back(account.value().root());
  }

  for (std::size_t i = 0; i < roots.size(); ++i) {
    for (std::size_t j = i + 1; j < roots.size(); ++j) {
      INFO(roots[i].string() << " vs " << roots[j].string());
      CHECK(roots[i] != roots[j]);
      CHECK_FALSE(contains(roots[i], roots[j]));  // neither contains the other
      CHECK_FALSE(contains(roots[j], roots[i]));

      // The string comparisons above only prove the PATHS differ. fs::equivalent
      // asks the REAL filesystem whether the two names resolve to the same
      // directory, which is what catches a case-insensitive or otherwise-aliasing
      // volume — the exact failure mode that made lowercase-only ids mandatory.
      std::error_code ec;
      const bool same_directory = fs::equivalent(roots[i], roots[j], ec);
      CHECK(!ec);
      CHECK_FALSE(same_directory);
    }
  }

  // Prove the check has teeth: the same id twice IS the same directory.
  std::error_code self_ec;
  CHECK(fs::equivalent(roots.front(), roots.front(), self_ec));
  CHECK(!self_ec);
}

TEST_CASE("each account's files land in its own tree once ensure() has run", "[accounts]") {
  const TempDir dir;
  Result<AccountDataDir> a = AccountDataDir::create(dir.path, "alpha");
  Result<AccountDataDir> b = AccountDataDir::create(dir.path, "beta");
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  REQUIRE(a.value().ensure().has_value());
  REQUIRE(b.value().ensure().has_value());

  // Write through each account's OWN intent-log path and prove the other account
  // cannot see it — the property every other assertion is a proxy for.
  {
    std::ofstream out(a.value().intent_log(), std::ios::binary | std::ios::trunc);
    out << "alpha-record";
  }
  REQUIRE(fs::is_regular_file(a.value().intent_log()));
  CHECK_FALSE(fs::exists(b.value().intent_log()));

  std::error_code ec;
  CHECK_FALSE(fs::equivalent(a.value().root(), b.value().root(), ec));
}

TEST_CASE("the SAME id yields the SAME tree", "[accounts]") {
  const TempDir dir;
  Result<AccountDataDir> first = AccountDataDir::create(dir.path, "acct1");
  Result<AccountDataDir> second = AccountDataDir::create(dir.path, "acct1");
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  CHECK(first.value().root() == second.value().root());
  CHECK(first.value().intent_log() == second.value().intent_log());
}

TEST_CASE("ensure() is idempotent and invokes the permission seam", "[accounts]") {
  const TempDir dir;
  PermsSpy spy;
  Result<AccountDataDir> account =
      AccountDataDir::create(dir.path / "data", "acct1", spy_fn(spy));
  REQUIRE(account.has_value());

  REQUIRE(account.value().ensure().has_value());
  CHECK(fs::is_directory(account.value().root()));
  REQUIRE(spy.calls.size() == 1);
  CHECK(spy.calls.front() == account.value().root());  // 0700 on the ACCOUNT dir

  // Idempotent: a second ensure() on an existing tree succeeds and re-asserts
  // the permissions (they may have been loosened by an operator or a restore).
  REQUIRE(account.value().ensure().has_value());
  CHECK(fs::is_directory(account.value().root()));
  CHECK(spy.calls.size() == 2);
}

TEST_CASE("ensure() FAILS CLOSED when permissions cannot be tightened", "[accounts]") {
  const TempDir dir;
  PermsSpy spy;
  spy.succeed = false;
  Result<AccountDataDir> account = AccountDataDir::create(dir.path, "acct1", spy_fn(spy));
  REQUIRE(account.has_value());

  const Result<Ok> ensured = account.value().ensure();
  REQUIRE_FALSE(ensured.has_value());
  CHECK(ensured.error().category == ErrorCategory::Internal);
}

TEST_CASE("ensure() reports an error when a non-directory blocks the account path",
          "[accounts]") {
  const TempDir dir;
  // A regular FILE sitting exactly where the account directory must go.
  const fs::path blocker = dir.path / "acct1";
  {
    std::ofstream out(blocker, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << "not a directory";
  }
  REQUIRE(fs::is_regular_file(blocker));

  PermsSpy spy;
  Result<AccountDataDir> account = AccountDataDir::create(dir.path, "acct1", spy_fn(spy));
  REQUIRE(account.has_value());

  const Result<Ok> ensured = account.value().ensure();
  REQUIRE_FALSE(ensured.has_value());
  CHECK(ensured.error().category == ErrorCategory::Internal);
  CHECK(spy.calls.empty());  // never tightened something that is not our directory
}

TEST_CASE("two accounts that both ensure() do not disturb each other", "[accounts]") {
  const TempDir dir;
  PermsSpy spy_a;
  PermsSpy spy_b;
  Result<AccountDataDir> a = AccountDataDir::create(dir.path, "alpha", spy_fn(spy_a));
  Result<AccountDataDir> b = AccountDataDir::create(dir.path, "beta", spy_fn(spy_b));
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());

  REQUIRE(a.value().ensure().has_value());
  REQUIRE(b.value().ensure().has_value());

  CHECK(fs::is_directory(a.value().root()));
  CHECK(fs::is_directory(b.value().root()));
  CHECK(a.value().root() != b.value().root());
  REQUIRE(spy_a.calls.size() == 1);
  REQUIRE(spy_b.calls.size() == 1);
  CHECK(spy_a.calls.front() != spy_b.calls.front());
}
