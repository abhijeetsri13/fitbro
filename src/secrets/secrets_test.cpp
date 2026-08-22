#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/secret_provider.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/secrets/env_secret_provider.hpp"
#include "broker_exec/secrets/token_store.hpp"

// POSIX-only: AC-1 ("mode 0600 in a 0700 dir") can only be asserted by reading
// the real mode bits via stat(2). MSVC/Windows has no equivalent owner-only
// mode-bit notion (see permissions.cpp), so the assertion is necessarily guarded
// to non-Windows and compiled out cleanly under _WIN32.
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

using broker_exec::Result;
using broker_exec::errors::ErrorCategory;
using broker_exec::secrets::EnvLookup;
using broker_exec::secrets::EnvSecretProvider;
using broker_exec::secrets::TokenStore;

namespace {

// Deterministic, in-test env seam — the ONLY source of "env" here. The real
// process environment is never read (mirrors the config test's env_from()).
EnvLookup env_from(std::map<std::string, std::string> vars) {
  return [vars = std::move(vars)](std::string_view key) -> std::optional<std::string> {
    auto it = vars.find(std::string(key));
    if (it == vars.end()) {
      return std::nullopt;
    }
    return it->second;
  };
}

// A deterministic key source: returns a fixed 32-byte (256-bit) key for any
// account-scoped key name, so save/load are reproducible across the test.
class FakeKeySource final : public broker_exec::ports::SecretProvider {
 public:
  [[nodiscard]] Result<std::string> get(std::string_view /*key*/) const override {
    return std::string(32, '\x2A');  // 32 bytes of 0x2A
  }
};

// A unique temp directory, recursively removed on scope exit so runs never
// collide and leave no artifacts behind.
struct TempDir {
  fs::path path;

  explicit TempDir(const std::string& tag)
      : path(fs::temp_directory_path() / ("broker_exec_secrets_" + tag + "_" +
                                          std::to_string(reinterpret_cast<std::uintptr_t>(this)))) {
    std::error_code ec;
    fs::create_directories(path, ec);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

}  // namespace

TEST_CASE("EnvSecretProvider resolves a present key and errors on a missing one", "[secrets]") {
  const EnvSecretProvider provider(
      env_from({{"kite.api_key", "ak-live-value"}, {"kite.api_secret", "shh"}}));

  const auto present = provider.get("kite.api_key");
  REQUIRE(present.has_value());
  CHECK(present.value() == "ak-live-value");

  const auto missing = provider.get("kite.totp_secret");
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().category == ErrorCategory::Validation);
  // The error names the (non-secret) key but never any value.
  CHECK(missing.error().message.find("kite.totp_secret") != std::string::npos);
  CHECK(missing.error().message.find("ak-live-value") == std::string::npos);
}

TEST_CASE("TokenStore round-trips a token through AES-256-GCM encrypt/decrypt", "[secrets]") {
  const TempDir dir("roundtrip");
  const FakeKeySource keys;
  const TokenStore store(keys, dir.path);

  const std::string plaintext = "kite-access-token-Xk29mZpQ7rTb4Lw8Nc1Vd6Ya3";

  const auto saved = store.save("acct-1", "access_token", plaintext);
  REQUIRE(saved.has_value());

  // The on-disk blob exists and is NOT the plaintext (encrypted at rest).
  const fs::path blob_file = dir.path / "acct-1" / "access_token.enc";
  REQUIRE(fs::exists(blob_file));
  {
    std::ifstream in(blob_file, std::ios::binary);
    const std::string on_disk((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    CHECK(on_disk.find(plaintext) == std::string::npos);
    // IV(12) + ciphertext(len) + tag(16).
    CHECK(on_disk.size() == 12 + plaintext.size() + 16);
  }

  const auto loaded = store.load("acct-1", "access_token");
  REQUIRE(loaded.has_value());
  CHECK(loaded.value() == plaintext);
}

TEST_CASE("TokenStore fails closed when the on-disk blob is tampered", "[secrets]") {
  const TempDir dir("tamper");
  const FakeKeySource keys;
  const TokenStore store(keys, dir.path);

  REQUIRE(store.save("acct-1", "enc_token", "session-secret-payload").has_value());

  const fs::path blob_file = dir.path / "acct-1" / "enc_token.enc";

  // Flip one byte inside the ciphertext region (just past the 12-byte IV).
  std::string blob;
  {
    std::ifstream in(blob_file, std::ios::binary);
    blob.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  REQUIRE(blob.size() > 12 + 16);
  blob[13] = static_cast<char>(blob[13] ^ 0x01);
  {
    std::ofstream out(blob_file, std::ios::binary | std::ios::trunc);
    out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
  }

  // The GCM tag no longer authenticates -> fail closed, never garbage plaintext.
  const auto loaded = store.load("acct-1", "enc_token");
  REQUIRE_FALSE(loaded.has_value());
  CHECK(loaded.error().message.find("session-secret-payload") == std::string::npos);
}

TEST_CASE("TokenStore reports a missing token file rather than throwing", "[secrets]") {
  const TempDir dir("missing");
  const FakeKeySource keys;
  const TokenStore store(keys, dir.path);

  const auto loaded = store.load("acct-1", "never_saved");
  REQUIRE_FALSE(loaded.has_value());
  CHECK(loaded.error().category == ErrorCategory::Validation);
}

TEST_CASE("TokenStore persists the blob as 0600 in a 0700 dir (AC-1)", "[secrets]") {
  const TempDir dir("perms");
  const FakeKeySource keys;
  const TokenStore store(keys, dir.path);

  const auto saved = store.save("acct-1", "access_token", "session-secret-payload");
  REQUIRE(saved.has_value());

#ifndef _WIN32
  // POSIX-only by necessity: assert the real mode bits. The account dir must be
  // 0700 (owner rwx) and the encrypted blob 0600 (owner rw) — the AC-1 contract.
  const fs::path account_dir = dir.path / "acct-1";
  const fs::path blob_file = account_dir / "access_token.enc";

  struct stat dir_st {};
  struct stat file_st {};
  REQUIRE(::stat(account_dir.c_str(), &dir_st) == 0);
  REQUIRE(::stat(blob_file.c_str(), &file_st) == 0);
  CHECK((dir_st.st_mode & 0777) == 0700);
  CHECK((file_st.st_mode & 0777) == 0600);
#else
  // Windows has no POSIX owner-only mode bits (permissions.cpp documents this);
  // a successful save() is the most this platform can assert here.
  SUCCEED();
#endif
}

// ── Atomic, durable publish (issue #6) ──────────────────────────────────────
//
// `save()` used to open the live token file with `ios::trunc` and only
// `flush()` it. That destroyed the existing token before the replacement was on
// disk, so a power loss in that window left a zero-length or partial blob and no
// way back to the token that had been working. The implementation now writes a
// temp sibling, fsyncs it, tightens it, renames over the target and fsyncs the
// directory.
//
// What follows pins the observable consequences of that shape. The crash window
// itself is not reproducible in-process — the SIGKILL harness in tests/sigkill
// is where a real kill is applied — so these assert the properties that remain
// checkable: the target is replaced whole, nothing is left behind, and debris
// from a crashed predecessor is inert.

// Count the `.tmp-*` siblings a write may have left behind.
[[nodiscard]] int temp_residue(const fs::path& dir) {
  int n = 0;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.path().filename().string().find(".tmp-") != std::string::npos) {
      ++n;
    }
  }
  return n;
}

TEST_CASE("TokenStore leaves no temp file behind on a successful save", "[secrets]") {
  const TempDir dir("no-residue");
  const FakeKeySource keys;
  const TokenStore store(keys, dir.path);

  REQUIRE(store.save("acct-1", "access_token", "first").has_value());
  REQUIRE(store.save("acct-1", "access_token", "second").has_value());

  const fs::path account_dir = dir.path / "acct-1";
  CHECK(temp_residue(account_dir) == 0);
  // Exactly the published blob, nothing else.
  CHECK(std::distance(fs::directory_iterator(account_dir), fs::directory_iterator{}) == 1);
}

TEST_CASE("TokenStore replaces a token wholesale rather than growing it in place", "[secrets]") {
  const TempDir dir("replace");
  const FakeKeySource keys;
  const TokenStore store(keys, dir.path);

  const std::string long_token(512, 'A');
  const std::string short_token = "B";
  REQUIRE(store.save("acct-1", "access_token", long_token).has_value());
  REQUIRE(store.save("acct-1", "access_token", short_token).has_value());

  const auto loaded = store.load("acct-1", "access_token");
  REQUIRE(loaded.has_value());
  CHECK(loaded.value() == short_token);

  // A shorter token must leave no tail of the longer one: an in-place write
  // without truncation would, and an authenticated blob of the wrong length
  // would fail the GCM tag rather than round-trip.
  const fs::path blob_file = dir.path / "acct-1" / "access_token.enc";
  CHECK(fs::file_size(blob_file) == 12 + short_token.size() + 16);
}

TEST_CASE("TokenStore ignores temp debris left by a crashed predecessor", "[secrets]") {
  const TempDir dir("stale-temp");
  const FakeKeySource keys;
  const TokenStore store(keys, dir.path);

  REQUIRE(store.save("acct-1", "access_token", "good-token").has_value());

  // A predecessor died between writing its temp and renaming it. The debris is
  // not a valid blob, and it is not the published name.
  const fs::path debris = dir.path / "acct-1" / "access_token.enc.tmp-99999";
  {
    std::ofstream out(debris, std::ios::binary);
    out << "torn-and-meaningless";
  }

  // The good token is still what loads — the temp name is never read.
  auto loaded = store.load("acct-1", "access_token");
  REQUIRE(loaded.has_value());
  CHECK(loaded.value() == "good-token");

  // And a later save still publishes, rather than colliding with the debris.
  REQUIRE(store.save("acct-1", "access_token", "newer-token").has_value());
  loaded = store.load("acct-1", "access_token");
  REQUIRE(loaded.has_value());
  CHECK(loaded.value() == "newer-token");
  CHECK(fs::exists(debris));  // untouched: cleaning up others' debris is not save()'s job
}

#ifndef _WIN32
TEST_CASE("TokenStore publishes by replacing the file, never by rewriting it in place",
          "[secrets]") {
  // THIS is the test that separates the two implementations. Everything else
  // about a save looks identical from the outside; the inode does not.
  //
  //   ios::trunc  -> the same inode is emptied and refilled. A crash mid-write
  //                  leaves that inode - the live token - torn.
  //   temp+rename -> a NEW inode is written and fsynced, then linked over the
  //                  name. The old inode stays whole until the instant it is
  //                  replaced, so a crash leaves either token, never a hybrid.
  //
  // POSIX-only: st_ino is the portable way to ask "is this the same file?", and
  // Windows has no equivalent reachable without a Win32 handle call, which does
  // not belong outside src/platform (see docs/conventions.md).
  const TempDir dir("atomic-publish");
  const FakeKeySource keys;
  const TokenStore store(keys, dir.path);

  REQUIRE(store.save("acct-1", "access_token", "first").has_value());
  const fs::path blob_file = dir.path / "acct-1" / "access_token.enc";

  struct stat before {};
  REQUIRE(::stat(blob_file.c_str(), &before) == 0);

  REQUIRE(store.save("acct-1", "access_token", "second").has_value());

  struct stat after {};
  REQUIRE(::stat(blob_file.c_str(), &after) == 0);

  CHECK(before.st_ino != after.st_ino);

  const auto loaded = store.load("acct-1", "access_token");
  REQUIRE(loaded.has_value());
  CHECK(loaded.value() == "second");
}

TEST_CASE("TokenStore publishes the blob already at 0600, never at the umask default",
          "[secrets]") {
  const TempDir dir("publish-mode");
  const FakeKeySource keys;
  const TokenStore store(keys, dir.path);

  REQUIRE(store.save("acct-1", "access_token", "session-secret-payload").has_value());

  // Tightening happens on the temp BEFORE the rename, so the blob has never been
  // reachable at its published name in a wider mode.
  const fs::path blob_file = dir.path / "acct-1" / "access_token.enc";
  struct stat st {};
  REQUIRE(::stat(blob_file.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
}
#endif
