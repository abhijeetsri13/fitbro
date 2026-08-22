#include "broker_exec/store/store.hpp"

#include <sqlite3.h>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"

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
