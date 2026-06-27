#include "broker_exec/idempotency/idempotency.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/idempotency/uuid.hpp"
#include "broker_exec/intentlog/intent_log.hpp"
#include "broker_exec/store/store.hpp"

namespace fs = std::filesystem;
using broker_exec::clock::TestClock;
using broker_exec::domain::OrderIntent;
using broker_exec::domain::OrderState;
using broker_exec::domain::OrderType;
using broker_exec::domain::Price;
using broker_exec::domain::Product;
using broker_exec::domain::Quantity;
using broker_exec::domain::Side;
using broker_exec::intentlog::IntentLog;
using broker_exec::intentlog::IntentOp;
using broker_exec::intentlog::IntentRecord;
using broker_exec::store::Store;

namespace idem = broker_exec::idempotency;

namespace {

// A representative intent the tests reuse. `client_ref` is intentionally left
// empty here — the signature ignores it, and reserve()/make_client_ref mint it.
OrderIntent sample_intent() {
  OrderIntent intent;
  intent.symbol = "NIFTY24JUN24000CE";
  intent.side = Side::Sell;
  intent.quantity = Quantity::of(50);
  intent.price = Price::from_rupees(123, 50);
  intent.order_type = OrderType::Limit;
  intent.product = Product::Intraday;
  intent.strategy = "alpha";
  return intent;
}

// A unique temp path per test, removed on scope exit.
struct TempLog {
  fs::path path;
  explicit TempLog(const std::string& tag)
      : path(fs::temp_directory_path() /
             ("broker_exec_idem_" + tag + "_" +
              std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".log")) {
    std::error_code ec;
    fs::remove(path, ec);
  }
  ~TempLog() {
    std::error_code ec;
    fs::remove(path, ec);
  }
  TempLog(const TempLog&) = delete;
  TempLog& operator=(const TempLog&) = delete;
};

}  // namespace

// ── (a) client-ref / child-ref format ──────────────────────────────────────

TEST_CASE("make_client_ref has the form strategy-<8hex>-<uuid>", "[idempotency][format]") {
  idem::SeededUuidGenerator uuids(0xABCD'1234'5678'9101ULL);
  const std::string uuid = uuids.next();
  const OrderIntent intent = sample_intent();
  const std::string sig = idem::signal_signature(intent);

  const std::string ref = idem::make_client_ref("alpha", sig, uuid);

  // strategy + '-' + 8 hex + '-' + 36-char uuid.
  const std::string prefix = "alpha-";
  REQUIRE(ref.rfind(prefix, 0) == 0);

  // Split on the two structural '-' that bound the sig8 (strategy has no '-').
  const std::size_t first = ref.find('-');
  const std::size_t second = ref.find('-', first + 1);
  REQUIRE(first != std::string::npos);
  REQUIRE(second != std::string::npos);

  const std::string strategy_part = ref.substr(0, first);
  const std::string sig8 = ref.substr(first + 1, second - first - 1);
  const std::string uuid_part = ref.substr(second + 1);

  REQUIRE(strategy_part == "alpha");
  REQUIRE(sig8.size() == 8);
  REQUIRE(sig8 == sig.substr(0, 8));
  for (char c : sig8) {
    REQUIRE(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')));
  }
  REQUIRE(uuid_part == uuid);
}

TEST_CASE("UUID generator yields canonical 8-4-4-4-12 v4", "[idempotency][uuid]") {
  // Deterministic bytes from a fixed seed so the assertion is exact.
  const std::string u = idem::format_uuid_v4(0x0123456789abcdefULL, 0xfedcba9876543210ULL);
  REQUIRE(u.size() == 36);
  REQUIRE(u[8] == '-');
  REQUIRE(u[13] == '-');
  REQUIRE(u[14] == '4');  // version nibble forced to 4
  REQUIRE(u[18] == '-');
  // variant: first nibble of the 4th group is one of 8,9,a,b.
  const char variant = u[19];
  REQUIRE(((variant == '8') || (variant == '9') || (variant == 'a') || (variant == 'b')));
  REQUIRE(u[23] == '-');
  for (std::size_t i = 0; i < u.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      continue;
    }
    const char c = u[i];
    REQUIRE(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')));
  }
}

TEST_CASE("SeededUuidGenerator is deterministic; Random differs", "[idempotency][uuid]") {
  idem::SeededUuidGenerator a(42);
  idem::SeededUuidGenerator b(42);
  REQUIRE(a.next() == b.next());
  REQUIRE(a.next() == b.next());

  idem::RandomUuidGenerator r1;
  idem::RandomUuidGenerator r2;
  // Astronomically unlikely to collide; guards against a constant generator.
  REQUIRE(r1.next() != r2.next());
}

TEST_CASE("child_ref / parent_of round-trip and is_child_ref", "[idempotency][child]") {
  const std::string parent = "alpha-deadbeef-0123";
  REQUIRE_FALSE(idem::is_child_ref(parent));
  REQUIRE(idem::parent_of(parent).empty());

  const std::string c1 = idem::child_ref(parent, 1);
  const std::string c2 = idem::child_ref(parent, 2);
  REQUIRE(c1 == parent + "#1");
  REQUIRE(c2 == parent + "#2");

  REQUIRE(idem::is_child_ref(c1));
  REQUIRE(idem::is_child_ref(c2));
  REQUIRE(idem::parent_of(c1) == parent);
  REQUIRE(idem::parent_of(c2) == parent);

  // Deterministic: same (parent, k) is bit-identical (Story 2.9 re-slice).
  REQUIRE(idem::child_ref(parent, 2) == c2);

  // k < 1 is clamped to 1.
  REQUIRE(idem::child_ref(parent, 0) == parent + "#1");
  REQUIRE(idem::child_ref(parent, -5) == parent + "#1");
}

// ── (b) deterministic signal signature ─────────────────────────────────────

TEST_CASE("signal_signature is deterministic and excludes client_ref",
          "[idempotency][signature]") {
  OrderIntent a = sample_intent();
  OrderIntent b = sample_intent();
  a.client_ref = "alpha-aaaaaaaa-0001";
  b.client_ref = "alpha-bbbbbbbb-9999";  // different ref, same signal

  const std::string sig_a = idem::signal_signature(a);
  REQUIRE(sig_a.size() == 64);  // SHA-256 hex
  REQUIRE(sig_a == idem::signal_signature(a));     // stable across calls
  REQUIRE(sig_a == idem::signal_signature(b));     // client_ref does not change it
}

TEST_CASE("changing any order-defining field changes the signature",
          "[idempotency][signature]") {
  const std::string base = idem::signal_signature(sample_intent());

  {
    OrderIntent v = sample_intent();
    v.symbol = "BANKNIFTY24JUN50000PE";
    REQUIRE(idem::signal_signature(v) != base);
  }
  {
    OrderIntent v = sample_intent();
    v.side = Side::Buy;
    REQUIRE(idem::signal_signature(v) != base);
  }
  {
    OrderIntent v = sample_intent();
    v.quantity = Quantity::of(75);
    REQUIRE(idem::signal_signature(v) != base);
  }
  {
    OrderIntent v = sample_intent();
    v.price = Price::from_rupees(123, 51);
    REQUIRE(idem::signal_signature(v) != base);
  }
  {
    OrderIntent v = sample_intent();
    v.order_type = OrderType::Market;
    REQUIRE(idem::signal_signature(v) != base);
  }
  {
    OrderIntent v = sample_intent();
    v.product = Product::Delivery;
    REQUIRE(idem::signal_signature(v) != base);
  }
  {
    OrderIntent v = sample_intent();
    v.strategy = "beta";
    REQUIRE(idem::signal_signature(v) != base);
  }
}

// ── (c) dedup: reserve twice -> one order ──────────────────────────────────

TEST_CASE("reserve twice with the same intent returns the existing order, one row",
          "[idempotency][dedup]") {
  auto opened = Store::open(":memory:");
  REQUIRE(opened.has_value());
  Store store = std::move(opened.value());

  idem::SeededUuidGenerator uuids(7);
  idem::IdempotencyIndex index;
  const OrderIntent intent = sample_intent();

  // First reserve: a fresh client-ref, is_new == true.
  auto first = idem::reserve(index, store, uuids, intent.strategy, intent);
  REQUIRE(first.has_value());
  REQUIRE(first.value().is_new);
  REQUIRE_FALSE(first.value().existing.has_value());
  const std::string ref = first.value().client_ref;

  // Caller persists the new order (the dispatch path's record step).
  broker_exec::domain::Order order;
  order.intent = intent;
  order.intent.client_ref = ref;
  order.state = OrderState::Sent;
  auto inserted = store.insert_order(order);
  REQUIRE(inserted.has_value());

  // Second reserve of the SAME signal: not new, returns the existing order.
  auto second = idem::reserve(index, store, uuids, intent.strategy, intent);
  REQUIRE(second.has_value());
  REQUIRE_FALSE(second.value().is_new);
  REQUIRE(second.value().client_ref == ref);
  REQUIRE(second.value().existing.has_value());
  REQUIRE(second.value().existing->intent.client_ref == ref);

  // Index holds exactly one mapping; the store holds exactly one order.
  REQUIRE(index.size() == 1);
  auto all = store.all_orders();
  REQUIRE(all.has_value());
  REQUIRE(all.value().size() == 1);
}

TEST_CASE("UNIQUE(client_ref) rejects a duplicate insert (store backstop)",
          "[idempotency][dedup]") {
  auto opened = Store::open(":memory:");
  REQUIRE(opened.has_value());
  Store store = std::move(opened.value());

  broker_exec::domain::Order order;
  order.intent = sample_intent();
  order.intent.client_ref = "alpha-deadbeef-0001";
  REQUIRE(store.insert_order(order).has_value());

  auto dup = store.insert_order(order);
  REQUIRE_FALSE(dup.has_value());
  REQUIRE(dup.error().category == broker_exec::errors::ErrorCategory::DuplicateOrder);
}

TEST_CASE("reserve finds a store-only duplicate even with an empty index",
          "[idempotency][dedup]") {
  // Simulates a torn restart: the order is in the store but the in-memory index
  // was not rebuilt for it. reserve must still return it (UNIQUE backstop path).
  auto opened = Store::open(":memory:");
  REQUIRE(opened.has_value());
  Store store = std::move(opened.value());

  // Use a fixed-seed generator so the minted ref is reproducible, then pre-seed
  // the store with exactly that ref.
  const OrderIntent intent = sample_intent();
  const std::string sig = idem::signal_signature(intent);
  idem::SeededUuidGenerator probe(99);
  const std::string ref = idem::make_client_ref(intent.strategy, sig, probe.next());

  broker_exec::domain::Order order;
  order.intent = intent;
  order.intent.client_ref = ref;
  order.state = OrderState::Acknowledged;
  REQUIRE(store.insert_order(order).has_value());

  idem::IdempotencyIndex empty_index;  // not rebuilt
  idem::SeededUuidGenerator uuids(99);  // same seq -> mints the same ref
  auto r = idem::reserve(empty_index, store, uuids, intent.strategy, intent);
  REQUIRE(r.has_value());
  REQUIRE_FALSE(r.value().is_new);
  REQUIRE(r.value().client_ref == ref);
  REQUIRE(r.value().existing.has_value());
}

// ── (d) restart: rebuild_from_log finds a prior submission ──────────────────

TEST_CASE("rebuild_from_log recovers a prior submission so restart dedups",
          "[idempotency][restart]") {
  TempLog tmp("restart");
  TestClock clock(std::chrono::steady_clock::time_point{},
                  std::chrono::system_clock::time_point{});

  const OrderIntent intent = sample_intent();
  idem::SeededUuidGenerator uuids(123);
  const std::string sig = idem::signal_signature(intent);
  const std::string ref = idem::make_client_ref(intent.strategy, sig, uuids.next());

  // Pre-restart: append the canonical PlaceOrder payload to the intent log.
  {
    auto opened = IntentLog::open(tmp.path, clock);
    REQUIRE(opened.has_value());
    IntentLog log = std::move(opened.value());

    OrderIntent placed = intent;
    placed.client_ref = ref;
    auto appended = log.append(IntentOp::PlaceOrder, ref, idem::intent_payload_json(placed));
    REQUIRE(appended.has_value());

    // A non-PlaceOrder record must be ignored by rebuild.
    REQUIRE(log.append(IntentOp::Result, ref, R"({"status":"FILLED"})").has_value());
  }

  // Post-restart: reopen, replay, rebuild the index from the records.
  std::vector<IntentRecord> records;
  {
    auto opened = IntentLog::open(tmp.path, clock);
    REQUIRE(opened.has_value());
    IntentLog log = std::move(opened.value());
    auto replayed = log.replay();
    REQUIRE(replayed.has_value());
    records = std::move(replayed.value());
  }

  idem::IdempotencyIndex index;
  const std::size_t recovered = index.rebuild_from_log(records);
  REQUIRE(recovered == 1);
  REQUIRE(index.size() == 1);

  // The same signal now resolves to the prior client-ref (no new order).
  auto found = index.existing_ref(intent);
  REQUIRE(found.has_value());
  REQUIRE(*found == ref);
}

TEST_CASE("rebuild_from_log skips opaque/foreign payloads without guessing",
          "[idempotency][restart]") {
  TempLog tmp("opaque");
  TestClock clock(std::chrono::steady_clock::time_point{},
                  std::chrono::system_clock::time_point{});

  std::vector<IntentRecord> records;
  {
    auto opened = IntentLog::open(tmp.path, clock);
    REQUIRE(opened.has_value());
    IntentLog log = std::move(opened.value());
    // Opaque payloads (no canonical schema/sig) — must be skipped.
    REQUIRE(log.append(IntentOp::PlaceOrder, "ref-x", R"({"qty":1,"side":"BUY"})").has_value());
    REQUIRE(log.append(IntentOp::PlaceOrder, "ref-y", "{}").has_value());
    auto replayed = log.replay();
    REQUIRE(replayed.has_value());
    records = std::move(replayed.value());
  }

  idem::IdempotencyIndex index;
  REQUIRE(index.rebuild_from_log(records) == 0);
  REQUIRE(index.size() == 0);
}

TEST_CASE("end-to-end: rebuilt index makes reserve return the prior ref after restart",
          "[idempotency][restart]") {
  TempLog tmp("e2e");
  TestClock clock(std::chrono::steady_clock::time_point{},
                  std::chrono::system_clock::time_point{});
  const OrderIntent intent = sample_intent();

  auto store_opened = Store::open(":memory:");
  REQUIRE(store_opened.has_value());
  Store store = std::move(store_opened.value());

  // First boot: reserve, persist the order, record the intent in the log.
  std::string ref;
  {
    idem::IdempotencyIndex index;
    idem::SeededUuidGenerator uuids(555);
    auto r = idem::reserve(index, store, uuids, intent.strategy, intent);
    REQUIRE(r.has_value());
    REQUIRE(r.value().is_new);
    ref = r.value().client_ref;

    broker_exec::domain::Order order;
    order.intent = intent;
    order.intent.client_ref = ref;
    order.state = OrderState::Sent;
    REQUIRE(store.insert_order(order).has_value());

    auto opened = IntentLog::open(tmp.path, clock);
    REQUIRE(opened.has_value());
    IntentLog log = std::move(opened.value());
    OrderIntent placed = intent;
    placed.client_ref = ref;
    REQUIRE(log.append(IntentOp::PlaceOrder, ref, idem::intent_payload_json(placed)).has_value());
  }

  // Second boot: a brand-new index rebuilt from the log; reserve must dedup.
  {
    auto opened = IntentLog::open(tmp.path, clock);
    REQUIRE(opened.has_value());
    IntentLog log = std::move(opened.value());
    auto replayed = log.replay();
    REQUIRE(replayed.has_value());

    idem::IdempotencyIndex index;
    REQUIRE(index.rebuild_from_log(replayed.value()) == 1);

    idem::SeededUuidGenerator uuids(999);  // a fresh seed — must NOT be used
    auto r = idem::reserve(index, store, uuids, intent.strategy, intent);
    REQUIRE(r.has_value());
    REQUIRE_FALSE(r.value().is_new);
    REQUIRE(r.value().client_ref == ref);
    REQUIRE(r.value().existing.has_value());
  }

  // Still exactly one order across the whole flow.
  auto all = store.all_orders();
  REQUIRE(all.has_value());
  REQUIRE(all.value().size() == 1);
}
