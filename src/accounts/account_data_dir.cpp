#include "broker_exec/accounts/account_data_dir.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <system_error>
#include <utility>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/platform/permissions.hpp"

namespace broker_exec::accounts {

namespace fs = std::filesystem;

namespace {

using errors::ErrorCategory;
using errors::make_error;

// LOWERCASE ONLY — this is a correctness rule, not a style rule. NTFS and the
// default APFS/HFS+ configuration are case-INSENSITIVE: "AB" and "ab" are two
// distinct account ids that resolve to ONE directory. Two processes would then
// share an intent log and a SQLite file, which is precisely the corruption the
// per-account tree exists to prevent. Rejecting A-Z makes the id its own
// canonical form, so "distinct id ⇒ distinct directory" holds on every
// filesystem in the CI matrix instead of only on the case-sensitive ones.
[[nodiscard]] bool is_allowed_id_char(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

// Windows reserves these device names at EVERY directory level, with or without
// an extension. Creating `con` as a directory fails (or worse, silently aliases
// a device), so an account id that spells one is rejected on every platform —
// the layout must be identical across the CI matrix. The comparison is against
// the lowercase spellings because uppercase is already rejected by the charset.
[[nodiscard]] bool is_reserved_device_name(std::string_view id) {
  static constexpr std::array<std::string_view, 22> kReserved{
      "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5", "com6", "com7",
      "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
  return std::find(kReserved.begin(), kReserved.end(), id) != kReserved.end();
}

}  // namespace

Result<ports::Ok> validate_account_id(std::string_view account_id) {
  if (account_id.empty()) {
    return fail(make_error(ErrorCategory::Validation, "account id: must not be empty"));
  }
  if (account_id.size() > kMaxAccountIdLength) {
    return fail(make_error(
        ErrorCategory::Validation,
        "account id: longer than " + std::to_string(kMaxAccountIdLength) + " characters"));
  }
  for (const char c : account_id) {
    if (!is_allowed_id_char(c)) {
      // Deliberately NOT echoing the offending id/char into the message: the id
      // is attacker-influenced text and the message goes to logs. The RULE is
      // what an operator needs.
      return fail(make_error(ErrorCategory::Validation,
                             "account id: only [a-z0-9_-] are allowed - lowercase only, because a "
                             "case-insensitive filesystem would map two ids onto one directory "
                             "(an account id is a name, not a path)"));
    }
  }
  if (is_reserved_device_name(account_id)) {
    return fail(make_error(ErrorCategory::Validation,
                           "account id: reserved device name (CON/PRN/AUX/NUL/COM1-9/LPT1-9)"));
  }
  return ports::ok();
}

DirPermissionFn default_dir_permission_fn() {
  return [](const fs::path& path) { return platform::restrict_to_owner_dir(path); };
}

AccountDataDir::AccountDataDir(fs::path data_root, std::string account_id, DirPermissionFn perms)
    : data_root_(std::move(data_root)),
      account_id_(std::move(account_id)),
      // EXACTLY ONE id-derived directory level, appended via std::filesystem
      // (never a hand-built separator). This is what makes distinct ids disjoint.
      root_(data_root_ / account_id_),
      perms_(std::move(perms)) {}

Result<AccountDataDir> AccountDataDir::create(fs::path data_root, std::string account_id,
                                              DirPermissionFn perms) {
  if (data_root.empty()) {
    return fail(make_error(ErrorCategory::Validation, "account data dir: empty data root"));
  }
  const Result<ports::Ok> valid = validate_account_id(account_id);
  if (!valid) {
    return fail(valid.error());
  }
  if (!perms) {
    perms = default_dir_permission_fn();
  }
  return AccountDataDir(std::move(data_root), std::move(account_id), std::move(perms));
}

fs::path AccountDataDir::intent_log() const {
  return root_ / "intent.log";
}
fs::path AccountDataDir::store_db() const {
  return root_ / "store.sqlite3";
}
fs::path AccountDataDir::token_store() const {
  return root_ / "token_store.enc";
}
fs::path AccountDataDir::ledger() const {
  return root_ / "ledger.jsonl";
}
fs::path AccountDataDir::kill_journal() const {
  return root_ / "kill_journal.jsonl";
}

Result<ports::Ok> AccountDataDir::ensure() const {
  std::error_code ec;

  // create_directories() is already idempotent: it reports false (with no error)
  // when the directory exists. We therefore check the RESULTING STATE, not the
  // return value, so a second ensure() on an existing tree is a clean success.
  // Every query uses the error_code overload — the throwing form would escape
  // this no-throw boundary on a permission error or a concurrent removal.
  fs::create_directories(root_, ec);
  std::error_code dir_ec;
  const bool is_dir = fs::is_directory(root_, dir_ec);
  if (dir_ec || !is_dir) {
    return fail(make_error(ErrorCategory::Internal,
                           "account data dir: cannot create the account directory (a "
                           "non-directory may be in the way, or it is unreadable)"));
  }

  // Owner-only (POSIX 0700) via the platform seam — the account tree holds the
  // encrypted token store. A failure here FAILS the call: running on with loose
  // permissions is not a degraded mode we accept.
  if (!perms_(root_)) {
    return fail(
        make_error(ErrorCategory::Internal,
                   "account data dir: cannot restrict the account directory to owner-only"));
  }

  return ports::ok();
}

}  // namespace broker_exec::accounts
