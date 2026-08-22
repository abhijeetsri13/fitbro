#include "broker_exec/store/store.hpp"

#include <sqlite3.h>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "sqlite_util.hpp"

namespace fs = std::filesystem;
using broker_exec::store::AuditRecord;
using broker_exec::store::Funds;
using broker_exec::store::RiskEvent;
using broker_exec::store::Store;
namespace domain = broker_exec::domain;
using broker_exec::errors::ErrorCategory;

namespace {

// A unique temp-file path per test (WAL needs a real file). Removed by RAII so a
// failing assertion does not leak the db / its -wal / -shm sidecars.
class TempDb {
 public:
  explicit TempDb(const std::string& tag) {
    path_ = fs::temp_directory_path() /
            ("broker_exec_store_" + tag + "_" + std::to_string(counter_++) + ".db");
    cleanup();
  }
  ~TempDb() { cleanup(); }

  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;

  [[nodiscard]] const fs::path& path() const noexcept { return path_; }

 private:
  void cleanup() {
    std::error_code ec;
    fs::remove(path_, ec);
    fs::remove(fs::path(path_).concat("-wal"), ec);
    fs::remove(fs::path(path_).concat("-shm"), ec);
  }

  static inline int counter_ = 0;
  fs::path path_;
};

// A unique temp DIRECTORY, removed by RAII. Distinct from TempDb because the
// non-ASCII path test needs the non-ASCII bytes in a directory COMPONENT: it is
// the PARENT lookup that fails when SQLite decodes a name differently from the
// filesystem call that created it (SQLITE_OPEN_CREATE creates the file, never
// the directory above it).
class TempDir {
 public:
  explicit TempDir(const fs::path& prefix) {
    path_ = fs::temp_directory_path() / prefix;
    path_ += std::to_string(counter_++);
    std::error_code ec;
    fs::remove_all(path_, ec);
    fs::create_directories(path_, ec);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const fs::path& path() const noexcept { return path_; }

 private:
  static inline int counter_ = 0;
  fs::path path_;
};

// A representative populated order for round-trip tests.
domain::Order sample_order(const std::string& client_ref) {
  domain::Order o;
  o.intent.client_ref = client_ref;
  o.intent.symbol = "NIFTY24JUN24000CE";
  o.intent.side = domain::Side::Sell;
  o.intent.quantity = domain::Quantity::of(50);
  o.intent.price = domain::Price::from_rupees(123, 45);
  o.intent.order_type = domain::OrderType::Limit;
  o.intent.product = domain::Product::Normal;
  o.intent.strategy = "iron_condor";
  o.state = domain::OrderState::Acknowledged;
  o.broker_order_id = "BRK-1";
  o.filled_qty = domain::Quantity::of(25);
  o.avg_price = domain::Price::from_rupees(123, 40);
  return o;
}

// Force the on-disk schema version stamp without going through the Store API,
// to simulate a database written by a future build.
void set_user_version(const fs::path& path, int version) {
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
  const std::string sql = "PRAGMA user_version = " + std::to_string(version) + ";";
  REQUIRE(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(db);
}

void drop_a_table(const fs::path& path) {
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
  REQUIRE(sqlite3_exec(db, "DROP TABLE positions;", nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(db);
}

}  // namespace

TEST_CASE("fresh open creates all tables at the current schema version", "[store]") {
  TempDb tmp("fresh");
  auto opened = Store::open(tmp.path());
  REQUIRE(opened.has_value());
  Store& store = opened.value();

  REQUIRE(store.schema_version() == 2);

  // All six tables empty but present (queries succeed, return nothing).
  REQUIRE(store.all_orders().has_value());
  REQUIRE(store.all_orders().value().empty());
  REQUIRE(store.all_trades().has_value());
  REQUIRE(store.all_positions().has_value());
  REQUIRE(store.all_risk_events().has_value());
  REQUIRE(store.all_audit().has_value());
  REQUIRE(store.find_funds("ACC1").has_value());
  REQUIRE_FALSE(store.find_funds("ACC1").value().has_value());
}

TEST_CASE("re-open is idempotent — no re-migration, data preserved", "[store]") {
  TempDb tmp("reopen");
  {
    auto opened = Store::open(tmp.path());
    REQUIRE(opened.has_value());
    REQUIRE(opened.value().insert_order(sample_order("S-1")).has_value());
  }
  // Second open over the same file: still at the current version, the row survives.
  auto reopened = Store::open(tmp.path());
  REQUIRE(reopened.has_value());
  REQUIRE(reopened.value().schema_version() == 2);
  auto found = reopened.value().find_order("S-1");
  REQUIRE(found.has_value());
  REQUIRE(found.value().has_value());
}

TEST_CASE("a future schema_version refuses to start", "[store]") {
  TempDb tmp("future");
  {
    auto opened = Store::open(tmp.path());  // create at the current version
    REQUIRE(opened.has_value());
  }
  set_user_version(tmp.path(), 999);  // pretend a newer build wrote it

  auto reopened = Store::open(tmp.path());
  REQUIRE_FALSE(reopened.has_value());
  REQUIRE(reopened.error().category == ErrorCategory::Internal);
  REQUIRE(reopened.error().action == broker_exec::errors::SuggestedAction::DoNotRetry);

  // open_or_rebuild must ALSO refuse: a newer schema is not corruption.
  auto rebuild = Store::open_or_rebuild(tmp.path());
  REQUIRE_FALSE(rebuild.has_value());
  REQUIRE(rebuild.error().category == ErrorCategory::Internal);
}

TEST_CASE("a NEGATIVE schema_version refuses to start on both open paths", "[store]") {
  TempDb tmp("negver");
  {
    auto opened = Store::open(tmp.path());  // create at the current version
    REQUIRE(opened.has_value());
    REQUIRE(opened.value().insert_order(sample_order("S-1")).has_value());
  }
  // One flipped bit in the 4-byte user_version field at offset 60 of the file
  // header turns 2 into a negative number — exactly the damage this store exists
  // to survive. PRAGMA quick_check still reports "ok" (it validates b-tree pages,
  // not this application-defined field) and all six tables are present, so
  // nothing upstream stops the value: apply_migrations() used to begin its loop
  // at v = -1 and evaluate kMigrations[static_cast<std::size_t>(-1)] — i.e.
  // kMigrations[SIZE_MAX] on a 2-element array — then copy that wild
  // string_view into a std::string and hand it to sqlite3_exec as SQL.
  set_user_version(tmp.path(), -1);

  auto strict = Store::open(tmp.path());
  REQUIRE_FALSE(strict.has_value());
  REQUIRE(strict.error().category == ErrorCategory::Internal);
  // DoNotRetry, NOT Internal's default of RaiseAlert: the same typed refusal a
  // too-new schema gets. Anything that escaped the old out-of-bounds loop came
  // from sqlite_error(), which carries the category default instead — so this
  // line is what separates "refused the stamp" from "tripped over it".
  REQUIRE(strict.error().action == broker_exec::errors::SuggestedAction::DoNotRetry);

  // open_or_rebuild must ALSO refuse. An uninterpretable stamp is doubt, and the
  // recovery path's answer to doubt is reset() — DROP TABLE on all six tables.
  auto rebuild = Store::open_or_rebuild(tmp.path());
  REQUIRE_FALSE(rebuild.has_value());
  REQUIRE(rebuild.error().action == broker_exec::errors::SuggestedAction::DoNotRetry);

  // Nothing was dropped on the way out: restore a sane stamp, the row is there.
  set_user_version(tmp.path(), 2);
  auto reopened = Store::open(tmp.path());
  REQUIRE(reopened.has_value());
  auto found = reopened.value().find_order("S-1");
  REQUIRE(found.has_value());
  REQUIRE(found.value().has_value());
}

TEST_CASE("insert_order enforces UNIQUE(client_ref); upsert updates; find round-trips", "[store]") {
  auto opened = Store::open(":memory:");  // in-memory is fine here (no reopen)
  REQUIRE(opened.has_value());
  Store& store = opened.value();

  const domain::Order original = sample_order("DUP-1");
  REQUIRE(store.insert_order(original).has_value());

  // Second insert on the same client_ref => DuplicateOrder.
  auto dup = store.insert_order(original);
  REQUIRE_FALSE(dup.has_value());
  REQUIRE(dup.error().category == ErrorCategory::DuplicateOrder);

  // The stored row round-trips byte-for-byte to the domain value.
  auto found = store.find_order("DUP-1");
  REQUIRE(found.has_value());
  REQUIRE(found.value().has_value());
  REQUIRE(found.value().value() == original);

  // upsert updates in place (no duplicate-key error), and the change is visible.
  domain::Order updated = original;
  updated.state = domain::OrderState::Filled;
  updated.filled_qty = domain::Quantity::of(50);
  updated.broker_order_id = "BRK-2";
  REQUIRE(store.upsert_order(updated).has_value());

  auto refound = store.find_order("DUP-1");
  REQUIRE(refound.has_value());
  REQUIRE(refound.value().has_value());
  REQUIRE(refound.value().value() == updated);

  // Still exactly one order.
  REQUIRE(store.all_orders().value().size() == 1);

  // A missing client_ref yields nullopt, not an error.
  auto missing = store.find_order("NOPE");
  REQUIRE(missing.has_value());
  REQUIRE_FALSE(missing.value().has_value());
}

// ── IMP-11: the stop TRIGGER survives the projection, and ABSENT stays absent ─

TEST_CASE("a stop order's trigger price round-trips through the store", "[store][IMP-11]") {
  auto opened = Store::open(":memory:");
  REQUIRE(opened.has_value());
  Store& store = opened.value();

  // A stop-loss LIMIT: two DISTINCT numbers. If the projection persisted only
  // `price_paise`, reloading this order after a restart would hand the engine a
  // stop armed at its limit — or no stop at all.
  domain::Order stop = sample_order("SL-1");
  stop.intent.order_type = domain::OrderType::StopLoss;
  stop.intent.price = domain::Price::from_rupees(119);
  stop.intent.trigger_price = domain::Price::from_rupees(120, 50);
  REQUIRE(store.insert_order(stop).has_value());

  auto found = store.find_order("SL-1");
  REQUIRE(found.has_value());
  REQUIRE(found.value().has_value());
  const domain::Order& back = found.value().value();
  REQUIRE(back == stop);  // whole-value equality: both prices, distinctly
  REQUIRE(back.intent.trigger_price.has_value());
  CHECK(*back.intent.trigger_price == domain::Price::from_rupees(120, 50));
  CHECK(back.intent.price == domain::Price::from_rupees(119));

  // An UPSERT can disarm the stop, and the column goes back to NULL — an update
  // that could only ever write a number would leave a phantom trigger behind.
  domain::Order disarmed = stop;
  disarmed.intent.order_type = domain::OrderType::Limit;
  disarmed.intent.trigger_price.reset();
  REQUIRE(store.upsert_order(disarmed).has_value());
  auto refound = store.find_order("SL-1");
  REQUIRE(refound.has_value());
  REQUIRE(refound.value().has_value());
  CHECK_FALSE(refound.value().value().intent.trigger_price.has_value());
  CHECK(refound.value().value() == disarmed);
}

TEST_CASE("a non-stop order reads back with NO trigger, not a zero one", "[store][IMP-11]") {
  // NULL decodes to nullopt. A sentinel column would read back as an ENGAGED
  // trigger of 0 here — exactly the absent-vs-zero confusion this change removes,
  // and it would make every reloaded limit order fail the gate's shape check.
  auto opened = Store::open(":memory:");
  REQUIRE(opened.has_value());
  Store& store = opened.value();

  const domain::Order plain = sample_order("LIM-1");  // Limit, no trigger
  REQUIRE_FALSE(plain.intent.trigger_price.has_value());
  REQUIRE(store.insert_order(plain).has_value());

  auto found = store.find_order("LIM-1");
  REQUIRE(found.has_value());
  REQUIRE(found.value().has_value());
  CHECK_FALSE(found.value().value().intent.trigger_price.has_value());
  CHECK(found.value().value() == plain);
}

TEST_CASE("trades insert idempotently; positions/funds upsert and read back", "[store]") {
  auto opened = Store::open(":memory:");
  REQUIRE(opened.has_value());
  Store& store = opened.value();

  domain::Trade t;
  t.trade_id = "T-1";
  t.client_ref = "S-1";
  t.broker_order_id = "BRK-1";
  t.quantity = domain::Quantity::of(50);
  t.price = domain::Price::from_rupees(100);
  REQUIRE(store.insert_trade(t).has_value());
  REQUIRE(store.insert_trade(t).has_value());  // same trade_id: idempotent no-op
  REQUIRE(store.all_trades().value().size() == 1);
  REQUIRE(store.all_trades().value().front() == t);

  domain::Position p;
  p.symbol = "NIFTY24JUN24000CE";
  p.net_qty = domain::Quantity::of(-50);  // short
  p.avg_price = domain::Price::from_rupees(123, 45);
  REQUIRE(store.upsert_position(p).has_value());
  p.net_qty = domain::Quantity::of(-100);  // grow the short
  REQUIRE(store.upsert_position(p).has_value());
  auto found_pos = store.find_position("NIFTY24JUN24000CE");
  REQUIRE(found_pos.value().has_value());
  REQUIRE(found_pos.value().value() == p);
  REQUIRE(store.all_positions().value().size() == 1);

  Funds f;
  f.account = "ACC1";
  f.available = domain::Money::from_rupees(50000);
  f.used_margin = domain::Money::from_rupees(12000, 50);
  f.fetched_at_epoch_ms = 1718900000000;
  REQUIRE(store.upsert_funds(f).has_value());
  f.available = domain::Money::from_rupees(48000);
  REQUIRE(store.upsert_funds(f).has_value());
  auto found_funds = store.find_funds("ACC1");
  REQUIRE(found_funds.value().has_value());
  REQUIRE(found_funds.value().value() == f);
}

TEST_CASE("risk_events and audit append and read back", "[store]") {
  auto opened = Store::open(":memory:");
  REQUIRE(opened.has_value());
  Store& store = opened.value();

  RiskEvent e;
  e.client_ref = "S-1";
  e.rule = "max_lots";
  e.detail = "requested 200 > limit 100";
  e.at_epoch_ms = 1718900000001;
  REQUIRE(store.insert_risk_event(e).has_value());
  auto events = store.all_risk_events();
  REQUIRE(events.value().size() == 1);
  REQUIRE(events.value().front().rule == "max_lots");
  REQUIRE(events.value().front().id > 0);  // store assigned a rowid

  AuditRecord a;
  a.seq = 42;
  a.client_ref = "S-1";
  a.event = "ORDER_SENT";
  a.payload = R"({"k":"v"})";
  a.at_epoch_ms = 1718900000002;
  REQUIRE(store.insert_audit(a).has_value());
  auto audit = store.all_audit();
  REQUIRE(audit.value().size() == 1);
  REQUIRE(audit.value().front().seq == 42);
  REQUIRE(audit.value().front().event == "ORDER_SENT");
}

TEST_CASE("reset() empties and re-creates the schema", "[store]") {
  auto opened = Store::open(":memory:");
  REQUIRE(opened.has_value());
  Store& store = opened.value();

  REQUIRE(store.insert_order(sample_order("S-1")).has_value());
  REQUIRE(store.all_orders().value().size() == 1);

  REQUIRE(store.reset().has_value());
  REQUIRE(store.schema_version() == 2);
  REQUIRE(store.all_orders().value().empty());  // schema present, data gone

  // The schema is fully usable again after reset.
  REQUIRE(store.insert_order(sample_order("S-2")).has_value());
  REQUIRE(store.all_orders().value().size() == 1);
}

TEST_CASE("open_or_rebuild recovers a half-migrated projection and flags needs_rebuild",
          "[store]") {
  TempDb tmp("halfmig");
  {
    auto opened = Store::open(tmp.path());
    REQUIRE(opened.has_value());
    REQUIRE(opened.value().insert_order(sample_order("S-1")).has_value());
  }
  // Simulate a half-migration: a table the version-1 schema promises is gone,
  // but user_version still reads 1.
  drop_a_table(tmp.path());

  // Strict open refuses (half-migrated).
  auto strict = Store::open(tmp.path());
  REQUIRE_FALSE(strict.has_value());
  REQUIRE(strict.error().category == ErrorCategory::Internal);

  // open_or_rebuild recovers: empty current-schema db + needs_rebuild=true.
  auto rebuilt = Store::open_or_rebuild(tmp.path());
  REQUIRE(rebuilt.has_value());
  REQUIRE(rebuilt.value().needs_rebuild);
  Store& store = rebuilt.value().store;
  REQUIRE(store.schema_version() == 2);
  REQUIRE(store.all_orders().value().empty());    // rebuilt clean; caller replays the log
  REQUIRE(store.find_position("X").has_value());  // the dropped table is back
}

TEST_CASE("open_or_rebuild on a clean fresh db does not request a rebuild", "[store]") {
  TempDb tmp("cleanopen");
  auto outcome = Store::open_or_rebuild(tmp.path());
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome.value().needs_rebuild);
  REQUIRE(outcome.value().store.schema_version() == 2);
}

// ── Only real damage may licence the destructive rebuild ───────────────────

TEST_CASE("is_corruption admits file damage only, never a transient failure", "[store]") {
  using broker_exec::store::detail::is_corruption;

  // The two codes that actually say "this file's contents are wrong".
  CHECK(is_corruption(SQLITE_CORRUPT));
  CHECK(is_corruption(SQLITE_NOTADB));
  // Extended codes carry the primary code in the low byte. Missing them would
  // make open_or_rebuild refuse to start on a genuinely corrupt projection —
  // the opposite failure, and just as wrong.
  CHECK(is_corruption(SQLITE_CORRUPT_VTAB));

  // Everything below is transient or environmental and says NOTHING about the
  // file's contents. Each of these used to arrive at open_or_rebuild as
  // `corrupt = true`, which runs reset(): DROP TABLE on orders, trades,
  // positions, funds, risk_events and audit. The first four can be replayed from
  // the intent log; `audit` and `risk_events` cannot — the log records intents,
  // not audit records — so one momentary lock at boot destroyed the FR-27 trail
  // for good. SQLITE_BUSY_RECOVERY is the concrete one: restarting after a
  // SIGKILL while another process still holds the db yields exactly that.
  CHECK_FALSE(is_corruption(SQLITE_BUSY));
  CHECK_FALSE(is_corruption(SQLITE_BUSY_RECOVERY));
  CHECK_FALSE(is_corruption(SQLITE_LOCKED));
  CHECK_FALSE(is_corruption(SQLITE_IOERR));
  CHECK_FALSE(is_corruption(SQLITE_IOERR_READ));
  CHECK_FALSE(is_corruption(SQLITE_NOMEM));
  CHECK_FALSE(is_corruption(SQLITE_INTERRUPT));
  CHECK_FALSE(is_corruption(SQLITE_CANTOPEN));
  CHECK_FALSE(is_corruption(SQLITE_READONLY));
  CHECK_FALSE(is_corruption(SQLITE_ERROR));
}

TEST_CASE("a store write waits out another connection's write lock", "[store]") {
  TempDb tmp("busy");
  auto opened = Store::open(tmp.path());
  REQUIRE(opened.has_value());
  Store& store = opened.value();

  // A second connection holds the write lock for a moment — an operator's
  // `sqlite3` shell, a backup, a checkpointer. Nothing called
  // sqlite3_busy_timeout() before this fix, so SQLite's default handler gave up
  // on the FIRST conflict and this insert came straight back SQLITE_BUSY.
  // Catch2's assertion macros are not thread-safe, so the holder only records
  // into atomics and the main thread asserts after the join.
  std::atomic<bool> lock_held{false};
  std::atomic<bool> lock_taken_ok{false};
  std::thread holder([&] {
    sqlite3* db = nullptr;
    const bool opened_ok = sqlite3_open(tmp.path().string().c_str(), &db) == SQLITE_OK;
    const bool begun_ok =
        opened_ok && sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK;
    lock_taken_ok.store(begun_ok);
    lock_held.store(true);
    if (begun_ok) {
      // Well inside the store's patience, so the wait always resolves; the test
      // never depends on an upper bound, only on the store not giving up at once.
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      (void)sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    }
    sqlite3_close(db);
  });

  while (!lock_held.load()) {
    std::this_thread::yield();
  }
  auto inserted = store.insert_order(sample_order("BUSY-1"));
  holder.join();

  REQUIRE(lock_taken_ok.load());  // the lock really was held; else this proves nothing
  REQUIRE(inserted.has_value());
  REQUIRE(store.all_orders().value().size() == 1);
}

TEST_CASE("the projection opens under a non-ASCII data root", "[store]") {
  // sqlite3_open_v2()'s filename is contractually UTF-8 on every platform, while
  // path::string() is the implementation's NATIVE NARROW encoding — on MSVC the
  // CRT code page. Under a non-ASCII root the two are different byte strings, so
  // the store asked the OS for a file whose name SQLite had mis-decoded: it
  // failed to open at boot, or (where the mangled parent happens to exist)
  // CREATEd a second, empty projection beside the populated intent log.
  //
  // The name is spelled with universal-character-names inside a u8"" literal, so
  // the bytes are UTF-8 whatever encoding the compiler reads this file in, and
  // fs::path treats a char8_t source as UTF-8 on every platform — the directory
  // really is created under this name and not an ACP transliteration of it.
  TempDir dir(fs::path(u8"broker_exec_store_\u00fcn\u00efc\u00f6d\u00e9_"));
  REQUIRE(fs::is_directory(dir.path()));
  const fs::path db_path = dir.path() / fs::path(u8"st\u00f6re.db");

  {
    auto opened = Store::open(db_path);
    REQUIRE(opened.has_value());
    REQUIRE(opened.value().insert_order(sample_order("U-1")).has_value());
  }
  // The bytes SQLite was handed named THIS file, not a transliteration of it.
  REQUIRE(fs::exists(db_path));

  auto reopened = Store::open(db_path);
  REQUIRE(reopened.has_value());
  auto found = reopened.value().find_order("U-1");
  REQUIRE(found.has_value());
  REQUIRE(found.value().has_value());
}
