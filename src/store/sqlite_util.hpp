#pragma once

// Module-private SQLite helpers for broker_exec::store. NOT a public header —
// it includes <sqlite3.h> and lives under src/, so only the store .cpp sees it.
//
// Provides: a RAII wrapper for prepared statements (always finalized, no leak
// under ASan), small typed bind/column helpers that bridge int64 <-> the
// sqlite3_int64 API without narrowing, and a uniform raw-error -> errors::Error
// mapping with safe, fixed messages (never the raw SQL or a row value).

#include <sqlite3.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/store/store.hpp"  // for Ok

namespace broker_exec::store::detail {

// Map a raw SQLite result code into the typed taxonomy with a safe, fixed
// message. We deliberately do NOT embed sqlite3_errmsg()/the SQL text (it could
// echo a bound value); `context` is a short, caller-chosen, log-safe label.
//   * SQLITE_CORRUPT / SQLITE_NOTADB    -> Internal (signals a rebuild upstream)
//   * SQLITE_CONSTRAINT (UNIQUE etc.)   -> Validation (caller refines to
//                                          DuplicateOrder where it applies)
//   * everything else                   -> Internal
[[nodiscard]] inline errors::Error sqlite_error(int code, std::string_view context) {
  using errors::ErrorCategory;
  const int primary = code & 0xff;  // strip extended-result-code high bits
  ErrorCategory category = ErrorCategory::Internal;
  if (primary == SQLITE_CONSTRAINT) {
    category = ErrorCategory::Validation;
  }
  std::string message = "store: ";
  message += context;
  // A short, stable code string for diagnostics (never a body/row value).
  std::string broker_code = "SQLITE_";
  broker_code += std::to_string(code);
  return errors::make_error(category, std::move(message), std::move(broker_code));
}

// True if a raw code is a constraint violation (e.g. UNIQUE(client_ref)).
[[nodiscard]] inline bool is_constraint(int code) noexcept {
  return (code & 0xff) == SQLITE_CONSTRAINT;
}

// RAII wrapper around a prepared statement: prepares on construction (via the
// factory) and always finalizes on destruction. Move-only.
class Statement {
 public:
  Statement() noexcept = default;
  explicit Statement(sqlite3_stmt* stmt) noexcept : stmt_(stmt) {}

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  Statement(Statement&& other) noexcept : stmt_(std::exchange(other.stmt_, nullptr)) {}
  Statement& operator=(Statement&& other) noexcept {
    if (this != &other) {
      reset();
      stmt_ = std::exchange(other.stmt_, nullptr);
    }
    return *this;
  }

  ~Statement() { reset(); }

  [[nodiscard]] sqlite3_stmt* get() const noexcept { return stmt_; }

  // ── Parameter binding (1-based index, the sqlite3 convention) ─────────────
  [[nodiscard]] int bind_text(int index, std::string_view value) noexcept {
    // SQLITE_TRANSIENT: sqlite copies the bytes, so a temporary is safe.
    return sqlite3_bind_text(stmt_, index, value.data(),
                             static_cast<int>(value.size()), SQLITE_TRANSIENT);
  }
  [[nodiscard]] int bind_int64(int index, std::int64_t value) noexcept {
    return sqlite3_bind_int64(stmt_, index, static_cast<sqlite3_int64>(value));
  }

  // ── Column accessors (0-based index) ──────────────────────────────────────
  [[nodiscard]] std::string column_text(int index) const {
    const auto* bytes = sqlite3_column_text(stmt_, index);
    if (bytes == nullptr) {
      return {};
    }
    const int len = sqlite3_column_bytes(stmt_, index);
    // sqlite3_column_text returns const unsigned char*; the cast to char* is the
    // canonical SQLite idiom (the bytes are UTF-8 text we copy immediately).
    return std::string(reinterpret_cast<const char*>(bytes),  // NOLINT(*-reinterpret-cast)
                       static_cast<std::size_t>(len));
  }
  [[nodiscard]] std::int64_t column_int64(int index) const noexcept {
    return static_cast<std::int64_t>(sqlite3_column_int64(stmt_, index));
  }

  // ── Fluent binding ────────────────────────────────────────────────────────
  // text()/i64() bind the next positional parameter (1-based, auto-incrementing)
  // and remember the first non-OK code so a whole bind run can be checked once:
  //   auto rc = stmt.text(a).text(b).i64(n).bind_status();
  // This keeps call sites clang-format-clean (no chains of single-line ifs).
  Statement& text(std::string_view value) noexcept {
    if (status_ == SQLITE_OK) {
      status_ = bind_text(next_++, value);
    }
    return *this;
  }
  Statement& i64(std::int64_t value) noexcept {
    if (status_ == SQLITE_OK) {
      status_ = bind_int64(next_++, value);
    }
    return *this;
  }
  [[nodiscard]] int bind_status() const noexcept { return status_; }

 private:
  void reset() noexcept {
    if (stmt_ != nullptr) {
      sqlite3_finalize(stmt_);
      stmt_ = nullptr;
    }
  }

  sqlite3_stmt* stmt_{nullptr};
  int next_{1};            // next positional bind index for the fluent text()/i64()
  int status_{SQLITE_OK};  // first non-OK code seen during a fluent bind run
};

// Prepare `sql` against `db` into a RAII Statement, or a typed Error.
[[nodiscard]] inline Result<Statement> prepare(sqlite3* db, std::string_view sql) {
  sqlite3_stmt* raw = nullptr;
  const int rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &raw, nullptr);
  if (rc != SQLITE_OK) {
    return fail(sqlite_error(rc, "prepare failed"));
  }
  return Statement(raw);
}

// Run a statement expected to produce no rows (INSERT/UPDATE/DDL). Returns the
// raw SQLite step code on the happy path so the caller can distinguish a
// constraint violation; otherwise a typed Error.
[[nodiscard]] inline Result<int> step_done(Statement& stmt, std::string_view context) {
  const int rc = sqlite3_step(stmt.get());
  if (rc != SQLITE_DONE) {
    return fail(sqlite_error(rc, context));
  }
  return rc;
}

// Execute a literal SQL string with no parameters (used for PRAGMAs and DDL
// batches). Returns a typed Error on failure. `sqlite3_exec` runs multiple
// statements, which is what schema setup needs.
[[nodiscard]] inline Result<Ok> exec(sqlite3* db, const char* sql, std::string_view context) {
  char* errmsg = nullptr;  // we never store this (could echo input); just free it.
  const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errmsg);
  if (errmsg != nullptr) {
    sqlite3_free(errmsg);
  }
  if (rc != SQLITE_OK) {
    return fail(sqlite_error(rc, context));
  }
  return Ok{};
}

}  // namespace broker_exec::store::detail
