// Contract test for broker_exec::ports: a tiny in-memory mock implements EVERY
// port interface. If this compiles and links, the abstractions are complete and
// implementable (correct signatures, the Ok/void substitute works, Result<T>
// returns are well-formed). It also smoke-tests the happy paths through the
// abstract base-class references the core will actually hold.

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/ports.hpp"
#include "broker_exec/result.hpp"

namespace {

using namespace broker_exec;
using broker_exec::ports::Ok;

// One class implementing all ports — the simplest proof they are implementable.
class MockBackend final : public ports::BrokerPort,
                          public ports::StorePort,
                          public ports::AlertSink,
                          public ports::SecretProvider,
                          public ports::RefDataPort {
 public:
  // ── BrokerPort ──
  Result<ports::BrokerAck> place(const domain::OrderIntent& intent) override {
    return ports::BrokerAck{"B-1", intent.client_ref};
  }
  Result<ports::BrokerAck> modify(const std::string& id,
                                  const domain::OrderIntent& intent) override {
    return ports::BrokerAck{id, intent.client_ref};
  }
  Result<Ok> cancel(const std::string&) override { return ports::ok(); }
  Result<Ok> square_off(const std::string&) override { return ports::ok(); }
  Result<std::vector<domain::Order>> fetch_orders() override {
    return std::vector<domain::Order>{};
  }
  Result<std::vector<domain::Trade>> fetch_trades() override {
    return std::vector<domain::Trade>{};
  }
  Result<std::vector<domain::Position>> fetch_positions() override {
    return std::vector<domain::Position>{};
  }
  Result<ports::FundsSnapshot> fetch_funds() override {
    return ports::FundsSnapshot{domain::Money::from_rupees(1000), domain::Money::from_rupees(0)};
  }

  // ── StorePort ──
  Result<Ok> append(const std::string& record) override {
    records_.push_back(record);
    return ports::ok();
  }
  Result<Ok> replay_all(
      const std::function<void(const std::string&)>& on_record) override {
    for (const auto& r : records_) {
      on_record(r);
    }
    return ports::ok();
  }
  Result<std::optional<std::string>> find_by_client_ref(
      const std::string& client_ref) const override {
    for (const auto& r : records_) {
      if (r == client_ref) {
        return std::optional<std::string>{r};
      }
    }
    return std::optional<std::string>{std::nullopt};
  }

  // ── AlertSink ──
  Result<Ok> send(ports::AlertLevel, const std::string&) override { return ports::ok(); }
  Result<Ok> send_test_alert() override { return ports::ok(); }

  // ── SecretProvider ──
  Result<std::string> get(std::string_view key) const override {
    if (key == "known") {
      return std::string{"value"};
    }
    return fail(errors::make_error(errors::ErrorCategory::Validation, "unknown key"));
  }

  // ── RefDataPort ──
  Result<domain::Instrument> resolve(std::string_view symbol) const override {
    domain::Instrument inst;
    inst.symbol = std::string{symbol};
    inst.token = 42;
    return inst;
  }
  std::chrono::system_clock::time_point as_of() const override { return as_of_; }

 private:
  std::vector<std::string> records_;
  std::chrono::system_clock::time_point as_of_{};
};

}  // namespace

TEST_CASE("ports are implementable and callable through abstract references", "[ports]") {
  MockBackend backend;

  // Hold each port through its abstract base — exactly how the core sees them.
  ports::BrokerPort& broker = backend;
  ports::StorePort& store = backend;
  ports::AlertSink& alerts = backend;
  ports::SecretProvider& secrets = backend;
  ports::RefDataPort& refdata = backend;

  domain::OrderIntent intent;
  intent.client_ref = "ref-1";

  SECTION("BrokerPort place/cancel and the Ok void-substitute") {
    auto ack = broker.place(intent);
    REQUIRE(ack);
    REQUIRE(ack.value().client_ref == "ref-1");

    auto cancelled = broker.cancel("B-1");
    REQUIRE(cancelled);  // Result<Ok>: success carries no value, just "ok"

    auto funds = broker.fetch_funds();
    REQUIRE(funds);
    REQUIRE(funds.value().available_margin == domain::Money::from_rupees(1000));
  }

  SECTION("StorePort append/replay/lookup") {
    REQUIRE(store.append("ref-1"));
    int seen = 0;
    REQUIRE(store.replay_all([&](const std::string&) { ++seen; }));
    REQUIRE(seen == 1);

    auto found = store.find_by_client_ref("ref-1");
    REQUIRE(found);
    REQUIRE(found.value().has_value());

    auto missing = store.find_by_client_ref("nope");
    REQUIRE(missing);
    REQUIRE_FALSE(missing.value().has_value());
  }

  SECTION("AlertSink send + self-test") {
    REQUIRE(alerts.send(ports::AlertLevel::Critical, "halt"));
    REQUIRE(alerts.send_test_alert());
  }

  SECTION("SecretProvider success and typed failure") {
    auto ok_secret = secrets.get("known");
    REQUIRE(ok_secret);
    REQUIRE(ok_secret.value() == "value");

    auto bad = secrets.get("missing");
    REQUIRE_FALSE(bad);
    REQUIRE(bad.error().category == errors::ErrorCategory::Validation);
  }

  SECTION("RefDataPort resolve + as_of") {
    auto inst = refdata.resolve("NIFTY");
    REQUIRE(inst);
    REQUIRE(inst.value().symbol == "NIFTY");
    (void)refdata.as_of();
  }
}
