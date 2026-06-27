#include "broker_exec/store/store.hpp"

#include <sqlite3.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "sqlite_util.hpp"

namespace broker_exec::store {
namespace {

using detail::exec;
using detail::is_constraint;
using detail::prepare;
using detail::Statement;
using detail::step_done;

// ── Schema versioning (NFR-4) ──────────────────────────────────────────────
//
// `kCurrentSchemaVersion` is the highest migration this build knows. The on-disk
// `schema_version` row records how far a database has been migrated.
//   * on-disk < current  -> apply the missing migrations forward (supported).
//   * on-disk == current  -> nothing to do.
//   * on-disk > current   -> NEWER than we understand -> refuse to start.
//
// Migrations are an ordered, append-only list. Migration[i] takes a database
// from version i to version i+1. NEVER edit a shipped migration's SQL — add a
// new one. Each migration runs inside the same transaction that bumps the
// version row, so a crash leaves the database either fully at N or fully at N-1.
constexpr int kCurrentSchemaVersion = 1;

// Migration 1: the initial schema. Tables orders/trades/positions/funds/
// risk_events/audit, with UNIQUE(client_ref) on orders (the idempotency
// backstop). Enums are stored as their stable to_string() text (NFR-8 names);
// money/price/quantity as integer paise/units (no float anywhere).
constexpr std::string_view kMigration1 = R"sql(
CREATE TABLE orders (
  client_ref      TEXT NOT NULL UNIQUE,
  symbol          TEXT NOT NULL,
  side            TEXT NOT NULL,
  quantity        INTEGER NOT NULL,
  price_paise     INTEGER NOT NULL,
  order_type      TEXT NOT NULL,
  product         TEXT NOT NULL,
  strategy        TEXT NOT NULL,
  state           TEXT NOT NULL,
  broker_order_id TEXT NOT NULL,
  filled_qty      INTEGER NOT NULL,
  avg_price_paise INTEGER NOT NULL
);
CREATE TABLE trades (
  trade_id        TEXT NOT NULL UNIQUE,
  client_ref      TEXT NOT NULL,
  broker_order_id TEXT NOT NULL,
  quantity        INTEGER NOT NULL,
  price_paise     INTEGER NOT NULL
);
CREATE TABLE positions (
  symbol          TEXT NOT NULL UNIQUE,
  net_qty         INTEGER NOT NULL,
  avg_price_paise INTEGER NOT NULL
);
CREATE TABLE funds (
  account            TEXT NOT NULL UNIQUE,
  available_paise    INTEGER NOT NULL,
  used_margin_paise  INTEGER NOT NULL,
  fetched_at_epoch_ms INTEGER NOT NULL
);
CREATE TABLE risk_events (
  id          INTEGER PRIMARY KEY,
  client_ref  TEXT NOT NULL,
  rule        TEXT NOT NULL,
  detail      TEXT NOT NULL,
  at_epoch_ms INTEGER NOT NULL
);
CREATE TABLE audit (
  id          INTEGER PRIMARY KEY,
  seq         INTEGER NOT NULL,
  client_ref  TEXT NOT NULL,
  event       TEXT NOT NULL,
  payload     TEXT NOT NULL,
  at_epoch_ms INTEGER NOT NULL
);
)sql";

// Ordered, append-only. Index i migrates version i -> i+1.
constexpr std::array<std::string_view, kCurrentSchemaVersion> kMigrations = {kMigration1};

// Every table the schema owns, for reset()/rebuild (drop in any order — no FKs).
constexpr std::array<std::string_view, 6> kTableNames = {"orders", "trades",     "positions",
                                                         "funds",  "risk_events", "audit"};

// ── Enum <-> stable text (the store cannot edit domain, so parse here) ──────
// Encodes via domain::to_string (the NFR-8 contract names) and decodes back.

std::string_view encode(domain::Side side) noexcept { return domain::to_string(side); }
std::string_view encode(domain::OrderType type) noexcept { return domain::to_string(type); }
std::string_view encode(domain::Product product) noexcept { return domain::to_string(product); }
std::string_view encode(domain::OrderState state) noexcept { return domain::to_string(state); }

domain::Side decode_side(std::string_view text) noexcept {
  return text == "SELL" ? domain::Side::Sell : domain::Side::Buy;
}

domain::OrderType decode_order_type(std::string_view text) noexcept {
  if (text == "LIMIT") {
    return domain::OrderType::Limit;
  }
  if (text == "SL") {
    return domain::OrderType::StopLoss;
  }
  if (text == "SL-M") {
    return domain::OrderType::StopLossMarket;
  }
  return domain::OrderType::Market;
}

domain::Product decode_product(std::string_view text) noexcept {
  if (text == "DELIVERY") {
    return domain::Product::Delivery;
  }
  if (text == "MARGIN") {
    return domain::Product::Margin;
  }
  if (text == "NORMAL") {
    return domain::Product::Normal;
  }
  return domain::Product::Intraday;
}

domain::OrderState decode_state(std::string_view text) noexcept {
  if (text == "VALIDATED") {
    return domain::OrderState::Validated;
  }
  if (text == "PENDING_SEND") {
    return domain::OrderState::PendingSend;
  }
  if (text == "SENT") {
    return domain::OrderState::Sent;
  }
  if (text == "ACKNOWLEDGED") {
    return domain::OrderState::Acknowledged;
  }
  if (text == "PARTIALLY_FILLED") {
    return domain::OrderState::PartiallyFilled;
  }
  if (text == "FILLED") {
    return domain::OrderState::Filled;
  }
  if (text == "REJECTED") {
    return domain::OrderState::Rejected;
  }
  if (text == "CANCELLED") {
    return domain::OrderState::Cancelled;
  }
  if (text == "UNKNOWN") {
    return domain::OrderState::Unknown;
  }
  if (text == "RECONCILED") {
    return domain::OrderState::Reconciled;
  }
  if (text == "PARTIALLY_PLACED") {
    return domain::OrderState::PartiallyPlaced;
  }
  if (text == "MANUAL_INTERVENTION_REQUIRED") {
    return domain::OrderState::ManualInterventionRequired;
  }
  return domain::OrderState::Created;
}

// ── schema_version: stored via SQLite's `user_version` pragma ──────────────
// `PRAGMA user_version` is a 32-bit int reserved for exactly this — an
// application schema stamp in the database header. It avoids a bootstrap table
// (no "does the version table exist yet?" problem) and reads atomically.

[[nodiscard]] Result<int> read_user_version(sqlite3* db) {
  auto stmt = prepare(db, "PRAGMA user_version;");
  if (!stmt) {
    return fail(stmt.error());
  }
  const int rc = sqlite3_step(stmt.value().get());
  if (rc != SQLITE_ROW) {
    return fail(detail::sqlite_error(rc, "read schema_version"));
  }
  return static_cast<int>(stmt.value().column_int64(0));
}

[[nodiscard]] Result<Ok> write_user_version(sqlite3* db, int version) {
  // PRAGMA user_version does not accept a bound parameter, so format the literal
  // ourselves. `version` is an int we control (never user input) so this is safe.
  const std::string sql = "PRAGMA user_version = " + std::to_string(version) + ";";
  return exec(db, sql.c_str(), "write schema_version");
}

// Apply migrations [from, kCurrentSchemaVersion) transactionally. Each step runs
// inside BEGIN/COMMIT together with its user_version bump, so a crash mid-apply
// leaves the database at a clean prior version (no half-migration on disk).
[[nodiscard]] Result<Ok> apply_migrations(sqlite3* db, int from) {
  for (int v = from; v < kCurrentSchemaVersion; ++v) {
    if (auto begun = exec(db, "BEGIN IMMEDIATE;", "begin migration"); !begun) {
      return begun;
    }
    if (auto applied =
            exec(db, std::string(kMigrations[static_cast<std::size_t>(v)]).c_str(), "migration");
        !applied) {
      (void)exec(db, "ROLLBACK;", "rollback migration");  // best-effort cleanup
      return applied;
    }
    if (auto bumped = write_user_version(db, v + 1); !bumped) {
      (void)exec(db, "ROLLBACK;", "rollback migration");
      return bumped;
    }
    if (auto committed = exec(db, "COMMIT;", "commit migration"); !committed) {
      (void)exec(db, "ROLLBACK;", "rollback migration");
      return committed;
    }
  }
  return Ok{};
}

// A half-migrated projection is one whose user_version claims a version but
// whose tables do not match it. We detect this cheaply: at the current version
// every expected table must exist. (A clean fresh db has version 0 and no
// tables, which is NOT half-migrated — apply_migrations builds it.)
[[nodiscard]] Result<bool> tables_consistent(sqlite3* db, int version) {
  if (version == 0) {
    return true;  // unmigrated; apply_migrations will build the schema.
  }
  for (const std::string_view name : kTableNames) {
    auto stmt = prepare(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1;");
    if (!stmt) {
      return fail(stmt.error());
    }
    if (const int rc = stmt.value().bind_text(1, name); rc != SQLITE_OK) {
      return fail(detail::sqlite_error(rc, "bind table name"));
    }
    const int rc = sqlite3_step(stmt.value().get());
    if (rc == SQLITE_DONE) {
      return false;  // a table the version promises is missing -> half-migrated.
    }
    if (rc != SQLITE_ROW) {
      return fail(detail::sqlite_error(rc, "check table"));
    }
  }
  return true;
}

// Set the connection-level PRAGMAs the projection requires. WAL + FULL sync give
// crash safety; foreign_keys is on for correctness if relations are added later.
// An in-memory database silently ignores WAL (it has no journal file), which is
// fine — :memory: is a test-only fast path.
[[nodiscard]] Result<Ok> configure_connection(sqlite3* db) {
  if (auto r = exec(db, "PRAGMA journal_mode = WAL;", "set WAL"); !r) {
    return r;
  }
  if (auto r = exec(db, "PRAGMA synchronous = FULL;", "set synchronous"); !r) {
    return r;
  }
  if (auto r = exec(db, "PRAGMA foreign_keys = ON;", "set foreign_keys"); !r) {
    return r;
  }
  return Ok{};
}

// A corruption probe that forces SQLite to actually read pages: a fresh open of
// a corrupt file does not fail until a page is touched. `PRAGMA quick_check`
// returns the single row "ok" on a healthy database.
[[nodiscard]] Result<bool> integrity_ok(sqlite3* db) {
  auto stmt = prepare(db, "PRAGMA quick_check;");
  if (!stmt) {
    // A prepare/read failure that is corruption-shaped counts as not-ok.
    return fail(stmt.error());
  }
  const int rc = sqlite3_step(stmt.value().get());
  if (rc != SQLITE_ROW) {
    return fail(detail::sqlite_error(rc, "integrity check"));
  }
  return stmt.value().column_text(0) == "ok";
}

}  // namespace

// ── Connection RAII deleter (defined here so the header avoids <sqlite3.h>) ──
void Store::ConnectionDeleter::operator()(sqlite3* db) const noexcept {
  if (db != nullptr) {
    sqlite3_close_v2(db);  // _v2 tolerates outstanding statements; we finalize ours
  }
}

Store::Store(Connection db, int version) noexcept
    : db_(std::move(db)), schema_version_(version) {}

Store::Store(Store&&) noexcept = default;
Store& Store::operator=(Store&&) noexcept = default;
Store::~Store() = default;

int Store::schema_version() const noexcept { return schema_version_; }

// Open the raw connection and apply the connection PRAGMAs. Shared by open(),
// open_or_rebuild(), and reset()'s drop-recreate path. Private static member so
// it may name the private Store::Connection type.
Result<Store::Connection> Store::open_connection(const std::filesystem::path& path) {
  sqlite3* raw = nullptr;
  // path.string() yields the UTF-8 (or active-codepage) filename; ":memory:" and
  // ordinary paths both flow through unchanged. SQLITE_OPEN_CREATE makes a fresh
  // file when absent (the cold-start case).
  const std::string filename = path.string();
  const int rc = sqlite3_open_v2(filename.c_str(), &raw,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
  Store::Connection db(raw);  // owns `raw` even on failure (sqlite3_open_v2 contract)
  if (rc != SQLITE_OK) {
    return fail(detail::sqlite_error(rc, "open database"));
  }
  if (auto configured = configure_connection(db.get()); !configured) {
    return fail(configured.error());
  }
  return db;
}

Result<Store> Store::open(std::filesystem::path path) {
  auto db = open_connection(path);
  if (!db) {
    return fail(db.error());
  }
  sqlite3* handle = db.value().get();

  // Corruption is a hard error on the strict open() path (the caller has not
  // asked us to rebuild). open_or_rebuild() turns the same condition into a
  // recoverable signal instead.
  auto healthy = integrity_ok(handle);
  if (!healthy) {
    return fail(healthy.error());
  }
  if (!healthy.value()) {
    return fail(errors::make_error(errors::ErrorCategory::Internal,
                                   "store: projection failed integrity check"));
  }

  auto version = read_user_version(handle);
  if (!version) {
    return fail(version.error());
  }
  if (version.value() > kCurrentSchemaVersion) {
    // NEWER/unknown schema: refuse to start (NFR-4). DoNotRetry — this is a
    // deployment/version fault, not something a retry fixes.
    return fail(errors::Error{
        .category = errors::ErrorCategory::Internal,
        .action = errors::SuggestedAction::DoNotRetry,
        .message = "store: database schema is newer than this build supports — refusing to start"});
  }

  auto consistent = tables_consistent(handle, version.value());
  if (!consistent) {
    return fail(consistent.error());
  }
  if (!consistent.value()) {
    // Half-migrated on the strict path: hard error (use open_or_rebuild to recover).
    return fail(errors::make_error(errors::ErrorCategory::Internal,
                                   "store: projection is half-migrated"));
  }

  if (auto migrated = apply_migrations(handle, version.value()); !migrated) {
    return fail(migrated.error());
  }

  return Store(std::move(db.value()), kCurrentSchemaVersion);
}

Result<Store::OpenOutcome> Store::open_or_rebuild(std::filesystem::path path) {
  auto db = open_connection(path);
  if (!db) {
    return fail(db.error());
  }
  sqlite3* handle = db.value().get();

  bool needs_rebuild = false;

  // 1) Physical integrity. A corruption-shaped failure (or a "not ok" verdict)
  //    is recoverable: we will drop+recreate and signal a rebuild.
  auto healthy = integrity_ok(handle);
  bool corrupt = false;
  if (!healthy) {
    // integrity_ok failed to even read — treat a corruption-shaped error as
    // recoverable; anything else is a genuine failure.
    corrupt = true;  // a failed quick_check read is corruption-shaped by nature
  } else {
    corrupt = !healthy.value();
  }

  // 2) Schema version. NEWER is still a hard refuse-to-start (not corruption).
  if (!corrupt) {
    auto version = read_user_version(handle);
    if (!version) {
      return fail(version.error());
    }
    if (version.value() > kCurrentSchemaVersion) {
      return fail(errors::Error{
          .category = errors::ErrorCategory::Internal,
          .action = errors::SuggestedAction::DoNotRetry,
          .message =
              "store: database schema is newer than this build supports — refusing to start"});
    }

    // 3) Half-migration is recoverable (rebuild from the intent log).
    auto consistent = tables_consistent(handle, version.value());
    if (!consistent) {
      // a read failure here is corruption-shaped -> recover
      corrupt = true;
    } else if (!consistent.value()) {
      needs_rebuild = true;  // half-migrated: rebuild
    } else if (auto migrated = apply_migrations(handle, version.value()); !migrated) {
      // a migration that fails to apply on a structurally-sound db is a real error
      return fail(migrated.error());
    }
  }

  Store store(std::move(db.value()), kCurrentSchemaVersion);

  if (corrupt || needs_rebuild) {
    // Drop everything and rebuild an empty current-schema db. The caller then
    // re-applies the intent log (replay->apply is Story 1.7/runtime).
    if (auto cleared = store.reset(); !cleared) {
      return fail(cleared.error());
    }
    needs_rebuild = true;
  }

  return OpenOutcome{std::move(store), needs_rebuild};
}

Result<Ok> Store::reset() {
  sqlite3* handle = db_.get();
  if (auto begun = exec(handle, "BEGIN IMMEDIATE;", "begin reset"); !begun) {
    return begun;
  }
  for (const std::string_view name : kTableNames) {
    const std::string sql = "DROP TABLE IF EXISTS " + std::string(name) + ";";
    if (auto dropped = exec(handle, sql.c_str(), "drop table"); !dropped) {
      (void)exec(handle, "ROLLBACK;", "rollback reset");
      return dropped;
    }
  }
  if (auto zeroed = write_user_version(handle, 0); !zeroed) {
    (void)exec(handle, "ROLLBACK;", "rollback reset");
    return zeroed;
  }
  if (auto committed = exec(handle, "COMMIT;", "commit reset"); !committed) {
    (void)exec(handle, "ROLLBACK;", "rollback reset");
    return committed;
  }
  // Rebuild the schema forward from scratch.
  if (auto migrated = apply_migrations(handle, 0); !migrated) {
    return migrated;
  }
  schema_version_ = kCurrentSchemaVersion;
  return Ok{};
}

// ── Orders ─────────────────────────────────────────────────────────────────

namespace {

// Bind a domain::Order's columns to a prepared INSERT/UPSERT statement. The
// statement must declare the parameters in this exact order (1-based).
[[nodiscard]] Result<Ok> bind_order(Statement& stmt, const domain::Order& o) {
  const domain::OrderIntent& in = o.intent;
  const int rc = stmt.text(in.client_ref)
                     .text(in.symbol)
                     .text(encode(in.side))
                     .i64(in.quantity.value())
                     .i64(in.price.paise())
                     .text(encode(in.order_type))
                     .text(encode(in.product))
                     .text(in.strategy)
                     .text(encode(o.state))
                     .text(o.broker_order_id)
                     .i64(o.filled_qty.value())
                     .i64(o.avg_price.paise())
                     .bind_status();
  if (rc != SQLITE_OK) {
    return fail(detail::sqlite_error(rc, "bind order"));
  }
  return Ok{};
}

// Read a domain::Order out of a stepped statement whose columns are, in order:
// client_ref, symbol, side, quantity, price_paise, order_type, product,
// strategy, state, broker_order_id, filled_qty, avg_price_paise.
[[nodiscard]] domain::Order read_order(const Statement& stmt) {
  domain::Order o;
  o.intent.client_ref = stmt.column_text(0);
  o.intent.symbol = stmt.column_text(1);
  o.intent.side = decode_side(stmt.column_text(2));
  o.intent.quantity = domain::Quantity::of(stmt.column_int64(3));
  o.intent.price = domain::Price::from_paise(stmt.column_int64(4));
  o.intent.order_type = decode_order_type(stmt.column_text(5));
  o.intent.product = decode_product(stmt.column_text(6));
  o.intent.strategy = stmt.column_text(7);
  o.state = decode_state(stmt.column_text(8));
  o.broker_order_id = stmt.column_text(9);
  o.filled_qty = domain::Quantity::of(stmt.column_int64(10));
  o.avg_price = domain::Price::from_paise(stmt.column_int64(11));
  return o;
}

constexpr std::string_view kOrderColumns =
    "client_ref, symbol, side, quantity, price_paise, order_type, product, "
    "strategy, state, broker_order_id, filled_qty, avg_price_paise";

}  // namespace

Result<Ok> Store::insert_order(const domain::Order& order) {
  auto stmt = prepare(db_.get(),
                      "INSERT INTO orders (" + std::string(kOrderColumns) +
                          ") VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12);");
  if (!stmt) {
    return fail(stmt.error());
  }
  if (auto bound = bind_order(stmt.value(), order); !bound) {
    return bound;
  }
  const int rc = sqlite3_step(stmt.value().get());
  if (rc == SQLITE_DONE) {
    return Ok{};
  }
  if (is_constraint(rc)) {
    // UNIQUE(client_ref) tripped: the idempotency backstop (Story 1.7).
    return fail(errors::make_error(errors::ErrorCategory::DuplicateOrder,
                                   "store: order with this client_ref already exists"));
  }
  return fail(detail::sqlite_error(rc, "insert order"));
}

Result<Ok> Store::upsert_order(const domain::Order& order) {
  auto stmt = prepare(
      db_.get(),
      "INSERT INTO orders (" + std::string(kOrderColumns) +
          ") VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12) "
          "ON CONFLICT(client_ref) DO UPDATE SET "
          "symbol=excluded.symbol, side=excluded.side, quantity=excluded.quantity, "
          "price_paise=excluded.price_paise, order_type=excluded.order_type, "
          "product=excluded.product, strategy=excluded.strategy, state=excluded.state, "
          "broker_order_id=excluded.broker_order_id, filled_qty=excluded.filled_qty, "
          "avg_price_paise=excluded.avg_price_paise;");
  if (!stmt) {
    return fail(stmt.error());
  }
  if (auto bound = bind_order(stmt.value(), order); !bound) {
    return bound;
  }
  if (auto done = step_done(stmt.value(), "upsert order"); !done) {
    return fail(done.error());
  }
  return Ok{};
}

Result<std::optional<domain::Order>> Store::find_order(std::string_view client_ref) const {
  auto stmt = prepare(db_.get(),
                      "SELECT " + std::string(kOrderColumns) +
                          " FROM orders WHERE client_ref = ?1;");
  if (!stmt) {
    return fail(stmt.error());
  }
  if (const int rc = stmt.value().bind_text(1, client_ref); rc != SQLITE_OK) {
    return fail(detail::sqlite_error(rc, "bind client_ref"));
  }
  const int rc = sqlite3_step(stmt.value().get());
  if (rc == SQLITE_DONE) {
    return std::optional<domain::Order>{};
  }
  if (rc != SQLITE_ROW) {
    return fail(detail::sqlite_error(rc, "find order"));
  }
  return std::optional<domain::Order>(read_order(stmt.value()));
}

Result<std::vector<domain::Order>> Store::all_orders() const {
  auto stmt = prepare(db_.get(),
                      "SELECT " + std::string(kOrderColumns) +
                          " FROM orders ORDER BY client_ref;");
  if (!stmt) {
    return fail(stmt.error());
  }
  std::vector<domain::Order> out;
  for (;;) {
    const int rc = sqlite3_step(stmt.value().get());
    if (rc == SQLITE_DONE) {
      break;
    }
    if (rc != SQLITE_ROW) {
      return fail(detail::sqlite_error(rc, "list orders"));
    }
    out.push_back(read_order(stmt.value()));
  }
  return out;
}

// ── Trades ───────────────────────────────────────────────────────────────

Result<Ok> Store::insert_trade(const domain::Trade& trade) {
  // INSERT OR IGNORE on the UNIQUE trade_id makes a reconcile replay idempotent
  // (re-seeing the same fill does not double-insert).
  auto stmt = prepare(db_.get(),
                      "INSERT OR IGNORE INTO trades "
                      "(trade_id, client_ref, broker_order_id, quantity, price_paise) "
                      "VALUES (?1,?2,?3,?4,?5);");
  if (!stmt) {
    return fail(stmt.error());
  }
  Statement& s = stmt.value();
  const int rc = s.text(trade.trade_id)
                     .text(trade.client_ref)
                     .text(trade.broker_order_id)
                     .i64(trade.quantity.value())
                     .i64(trade.price.paise())
                     .bind_status();
  if (rc != SQLITE_OK) {
    return fail(detail::sqlite_error(rc, "bind trade"));
  }
  if (auto done = step_done(s, "insert trade"); !done) {
    return fail(done.error());
  }
  return Ok{};
}

Result<std::vector<domain::Trade>> Store::all_trades() const {
  auto stmt = prepare(db_.get(),
                      "SELECT trade_id, client_ref, broker_order_id, quantity, price_paise "
                      "FROM trades ORDER BY trade_id;");
  if (!stmt) {
    return fail(stmt.error());
  }
  std::vector<domain::Trade> out;
  for (;;) {
    const int rc = sqlite3_step(stmt.value().get());
    if (rc == SQLITE_DONE) {
      break;
    }
    if (rc != SQLITE_ROW) {
      return fail(detail::sqlite_error(rc, "list trades"));
    }
    const Statement& s = stmt.value();
    domain::Trade t;
    t.trade_id = s.column_text(0);
    t.client_ref = s.column_text(1);
    t.broker_order_id = s.column_text(2);
    t.quantity = domain::Quantity::of(s.column_int64(3));
    t.price = domain::Price::from_paise(s.column_int64(4));
    out.push_back(std::move(t));
  }
  return out;
}

// ── Positions ──────────────────────────────────────────────────────────────

Result<Ok> Store::upsert_position(const domain::Position& position) {
  auto stmt = prepare(db_.get(),
                      "INSERT INTO positions (symbol, net_qty, avg_price_paise) "
                      "VALUES (?1,?2,?3) "
                      "ON CONFLICT(symbol) DO UPDATE SET "
                      "net_qty=excluded.net_qty, avg_price_paise=excluded.avg_price_paise;");
  if (!stmt) {
    return fail(stmt.error());
  }
  Statement& s = stmt.value();
  const int rc = s.text(position.symbol)
                     .i64(position.net_qty.value())
                     .i64(position.avg_price.paise())
                     .bind_status();
  if (rc != SQLITE_OK) {
    return fail(detail::sqlite_error(rc, "bind position"));
  }
  if (auto done = step_done(s, "upsert position"); !done) {
    return fail(done.error());
  }
  return Ok{};
}

namespace {
[[nodiscard]] domain::Position read_position(const Statement& s) {
  domain::Position p;
  p.symbol = s.column_text(0);
  p.net_qty = domain::Quantity::of(s.column_int64(1));
  p.avg_price = domain::Price::from_paise(s.column_int64(2));
  return p;
}
}  // namespace

Result<std::optional<domain::Position>> Store::find_position(std::string_view symbol) const {
  auto stmt = prepare(db_.get(),
                      "SELECT symbol, net_qty, avg_price_paise FROM positions WHERE symbol = ?1;");
  if (!stmt) {
    return fail(stmt.error());
  }
  if (const int rc = stmt.value().bind_text(1, symbol); rc != SQLITE_OK) {
    return fail(detail::sqlite_error(rc, "bind symbol"));
  }
  const int rc = sqlite3_step(stmt.value().get());
  if (rc == SQLITE_DONE) {
    return std::optional<domain::Position>{};
  }
  if (rc != SQLITE_ROW) {
    return fail(detail::sqlite_error(rc, "find position"));
  }
  return std::optional<domain::Position>(read_position(stmt.value()));
}

Result<std::vector<domain::Position>> Store::all_positions() const {
  auto stmt = prepare(db_.get(),
                      "SELECT symbol, net_qty, avg_price_paise FROM positions ORDER BY symbol;");
  if (!stmt) {
    return fail(stmt.error());
  }
  std::vector<domain::Position> out;
  for (;;) {
    const int rc = sqlite3_step(stmt.value().get());
    if (rc == SQLITE_DONE) {
      break;
    }
    if (rc != SQLITE_ROW) {
      return fail(detail::sqlite_error(rc, "list positions"));
    }
    out.push_back(read_position(stmt.value()));
  }
  return out;
}

// ── Funds ────────────────────────────────────────────────────────────────

Result<Ok> Store::upsert_funds(const Funds& funds) {
  auto stmt = prepare(
      db_.get(),
      "INSERT INTO funds (account, available_paise, used_margin_paise, fetched_at_epoch_ms) "
      "VALUES (?1,?2,?3,?4) "
      "ON CONFLICT(account) DO UPDATE SET "
      "available_paise=excluded.available_paise, used_margin_paise=excluded.used_margin_paise, "
      "fetched_at_epoch_ms=excluded.fetched_at_epoch_ms;");
  if (!stmt) {
    return fail(stmt.error());
  }
  Statement& s = stmt.value();
  const int rc = s.text(funds.account)
                     .i64(funds.available.paise())
                     .i64(funds.used_margin.paise())
                     .i64(funds.fetched_at_epoch_ms)
                     .bind_status();
  if (rc != SQLITE_OK) {
    return fail(detail::sqlite_error(rc, "bind funds"));
  }
  if (auto done = step_done(s, "upsert funds"); !done) {
    return fail(done.error());
  }
  return Ok{};
}

Result<std::optional<Funds>> Store::find_funds(std::string_view account) const {
  auto stmt = prepare(
      db_.get(),
      "SELECT account, available_paise, used_margin_paise, fetched_at_epoch_ms "
      "FROM funds WHERE account = ?1;");
  if (!stmt) {
    return fail(stmt.error());
  }
  if (const int rc = stmt.value().bind_text(1, account); rc != SQLITE_OK) {
    return fail(detail::sqlite_error(rc, "bind account"));
  }
  const int rc = sqlite3_step(stmt.value().get());
  if (rc == SQLITE_DONE) {
    return std::optional<Funds>{};
  }
  if (rc != SQLITE_ROW) {
    return fail(detail::sqlite_error(rc, "find funds"));
  }
  const Statement& s = stmt.value();
  Funds f;
  f.account = s.column_text(0);
  f.available = domain::Money::from_paise(s.column_int64(1));
  f.used_margin = domain::Money::from_paise(s.column_int64(2));
  f.fetched_at_epoch_ms = s.column_int64(3);
  return std::optional<Funds>(std::move(f));
}

// ── Risk events ────────────────────────────────────────────────────────────

Result<Ok> Store::insert_risk_event(const RiskEvent& event) {
  auto stmt = prepare(db_.get(),
                      "INSERT INTO risk_events (client_ref, rule, detail, at_epoch_ms) "
                      "VALUES (?1,?2,?3,?4);");
  if (!stmt) {
    return fail(stmt.error());
  }
  Statement& s = stmt.value();
  const int rc = s.text(event.client_ref)
                     .text(event.rule)
                     .text(event.detail)
                     .i64(event.at_epoch_ms)
                     .bind_status();
  if (rc != SQLITE_OK) {
    return fail(detail::sqlite_error(rc, "bind risk_event"));
  }
  if (auto done = step_done(s, "insert risk_event"); !done) {
    return fail(done.error());
  }
  return Ok{};
}

Result<std::vector<RiskEvent>> Store::all_risk_events() const {
  auto stmt = prepare(db_.get(),
                      "SELECT id, client_ref, rule, detail, at_epoch_ms "
                      "FROM risk_events ORDER BY id;");
  if (!stmt) {
    return fail(stmt.error());
  }
  std::vector<RiskEvent> out;
  for (;;) {
    const int rc = sqlite3_step(stmt.value().get());
    if (rc == SQLITE_DONE) {
      break;
    }
    if (rc != SQLITE_ROW) {
      return fail(detail::sqlite_error(rc, "list risk_events"));
    }
    const Statement& s = stmt.value();
    RiskEvent e;
    e.id = s.column_int64(0);
    e.client_ref = s.column_text(1);
    e.rule = s.column_text(2);
    e.detail = s.column_text(3);
    e.at_epoch_ms = s.column_int64(4);
    out.push_back(std::move(e));
  }
  return out;
}

// ── Audit ────────────────────────────────────────────────────────────────

Result<Ok> Store::insert_audit(const AuditRecord& record) {
  auto stmt = prepare(db_.get(),
                      "INSERT INTO audit (seq, client_ref, event, payload, at_epoch_ms) "
                      "VALUES (?1,?2,?3,?4,?5);");
  if (!stmt) {
    return fail(stmt.error());
  }
  Statement& s = stmt.value();
  const int rc = s.i64(record.seq)
                     .text(record.client_ref)
                     .text(record.event)
                     .text(record.payload)
                     .i64(record.at_epoch_ms)
                     .bind_status();
  if (rc != SQLITE_OK) {
    return fail(detail::sqlite_error(rc, "bind audit"));
  }
  if (auto done = step_done(s, "insert audit"); !done) {
    return fail(done.error());
  }
  return Ok{};
}

Result<std::vector<AuditRecord>> Store::all_audit() const {
  auto stmt = prepare(db_.get(),
                      "SELECT id, seq, client_ref, event, payload, at_epoch_ms "
                      "FROM audit ORDER BY id;");
  if (!stmt) {
    return fail(stmt.error());
  }
  std::vector<AuditRecord> out;
  for (;;) {
    const int rc = sqlite3_step(stmt.value().get());
    if (rc == SQLITE_DONE) {
      break;
    }
    if (rc != SQLITE_ROW) {
      return fail(detail::sqlite_error(rc, "list audit"));
    }
    const Statement& s = stmt.value();
    AuditRecord a;
    a.id = s.column_int64(0);
    a.seq = s.column_int64(1);
    a.client_ref = s.column_text(2);
    a.event = s.column_text(3);
    a.payload = s.column_text(4);
    a.at_epoch_ms = s.column_int64(5);
    out.push_back(std::move(a));
  }
  return out;
}

}  // namespace broker_exec::store
