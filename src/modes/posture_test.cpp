#include "broker_exec/modes/posture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>
#include <vector>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/health/watchdog.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

using broker_exec::errors::ErrorCategory;
using broker_exec::errors::SuggestedAction;
using broker_exec::health::HealthSignal;
using broker_exec::modes::DetectorSignal;
using broker_exec::modes::Posture;
using broker_exec::modes::posture_for;
using broker_exec::modes::PostureCoordinator;
using broker_exec::modes::to_string;
using broker_exec::ports::AlertLevel;

namespace ports = broker_exec::ports;

namespace {

// Recording AlertSink: counts alerts and tracks the last level/message. Mirrors
// the sink in the health/marketdata/reconcile tests.
class CountingAlertSink final : public ports::AlertSink {
 public:
  broker_exec::Result<ports::Ok> send(AlertLevel level, const std::string& message) override {
    last_message_ = message;
    last_level_ = level;
    ++count_;
    return ports::ok();
  }
  broker_exec::Result<ports::Ok> send_test_alert() override { return ports::ok(); }

  [[nodiscard]] std::size_t count() const noexcept { return count_; }
  [[nodiscard]] AlertLevel last_level() const noexcept { return last_level_; }
  [[nodiscard]] const std::string& last_message() const noexcept { return last_message_; }

 private:
  std::size_t count_ = 0;
  AlertLevel last_level_ = AlertLevel::Info;
  std::string last_message_;
};

[[nodiscard]] int sev(Posture p) {
  return static_cast<int>(p);
}

}  // namespace

TEST_CASE("no active signals with a Normal floor resolves to Normal", "[modes]") {
  const PostureCoordinator coord;
  CHECK(coord.evaluate({}) == Posture::Normal);
  CHECK(coord.evaluate({}, Posture::Normal) == Posture::Normal);
  CHECK(PostureCoordinator::allows_entries(Posture::Normal));
  CHECK(PostureCoordinator::require_entry_allowed(Posture::Normal).has_value());
}

TEST_CASE("posture_for maps each detector signal to its documented posture", "[modes]") {
  // BlockEntries group.
  CHECK(posture_for(DetectorSignal::StaleData) == Posture::BlockEntries);
  CHECK(posture_for(DetectorSignal::MuteFeed) == Posture::BlockEntries);
  CHECK(posture_for(DetectorSignal::Unknown) == Posture::BlockEntries);
  CHECK(posture_for(DetectorSignal::Mismatch) == Posture::BlockEntries);
  CHECK(posture_for(DetectorSignal::SessionExpiry) == Posture::BlockEntries);
  // ExitOnly group.
  CHECK(posture_for(DetectorSignal::BrokerDown) == Posture::ExitOnly);
  CHECK(posture_for(DetectorSignal::ClockSkew) == Posture::ExitOnly);
  CHECK(posture_for(DetectorSignal::ClockStall) == Posture::ExitOnly);
  CHECK(posture_for(DetectorSignal::ResourcePressure) == Posture::ExitOnly);
  // SoftKill (no detector reaches Panic).
  CHECK(posture_for(DetectorSignal::RiskBreach) == Posture::SoftKill);
}

TEST_CASE("evaluate resolves the severest of a contradictory mix (AC-1)", "[modes]") {
  const PostureCoordinator coord;
  CHECK(coord.evaluate({DetectorSignal::StaleData, DetectorSignal::BrokerDown}) ==
        Posture::ExitOnly);
  CHECK(coord.evaluate({DetectorSignal::StaleData, DetectorSignal::RiskBreach}) ==
        Posture::SoftKill);
  // operator_floor lifts everything to Panic even with a mild signal active.
  CHECK(coord.evaluate({DetectorSignal::MuteFeed}, Posture::Panic) == Posture::Panic);
  // operator_floor below an active signal does not lower the verdict.
  CHECK(coord.evaluate({DetectorSignal::RiskBreach}, Posture::BlockEntries) == Posture::SoftKill);
}

TEST_CASE("Posture is a total order by ascending underlying value", "[modes]") {
  CHECK(sev(Posture::Normal) < sev(Posture::BlockEntries));
  CHECK(sev(Posture::BlockEntries) < sev(Posture::ExitOnly));
  CHECK(sev(Posture::ExitOnly) < sev(Posture::SoftKill));
  CHECK(sev(Posture::SoftKill) < sev(Posture::Panic));
}

TEST_CASE("allows_entries is true only for Normal", "[modes]") {
  CHECK(PostureCoordinator::allows_entries(Posture::Normal));
  CHECK_FALSE(PostureCoordinator::allows_entries(Posture::BlockEntries));
  CHECK_FALSE(PostureCoordinator::allows_entries(Posture::ExitOnly));
  CHECK_FALSE(PostureCoordinator::allows_entries(Posture::SoftKill));
  CHECK_FALSE(PostureCoordinator::allows_entries(Posture::Panic));
}

TEST_CASE("allows_risk_reducing_exits is true except under Panic", "[modes]") {
  CHECK(PostureCoordinator::allows_risk_reducing_exits(Posture::Normal));
  CHECK(PostureCoordinator::allows_risk_reducing_exits(Posture::BlockEntries));
  CHECK(PostureCoordinator::allows_risk_reducing_exits(Posture::ExitOnly));
  CHECK(PostureCoordinator::allows_risk_reducing_exits(Posture::SoftKill));
  CHECK_FALSE(PostureCoordinator::allows_risk_reducing_exits(Posture::Panic));
}

TEST_CASE("require_entry_allowed gates entries with a typed RiskRejected error", "[modes]") {
  CHECK(PostureCoordinator::require_entry_allowed(Posture::Normal).has_value());

  for (const Posture p :
       {Posture::BlockEntries, Posture::ExitOnly, Posture::SoftKill, Posture::Panic}) {
    const auto r = PostureCoordinator::require_entry_allowed(p);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().category == ErrorCategory::RiskRejected);
    CHECK(r.error().action == SuggestedAction::BlockStrategy);
    // The message names the posture.
    CHECK(r.error().message.find(std::string(to_string(p))) != std::string::npos);
  }
}

TEST_CASE("from_health maps watchdog signals into the coordinator vocabulary", "[modes]") {
  CHECK(PostureCoordinator::from_health(HealthSignal::MuteFeed) == DetectorSignal::MuteFeed);
  CHECK(PostureCoordinator::from_health(HealthSignal::ClockSkew) == DetectorSignal::ClockSkew);
  CHECK(PostureCoordinator::from_health(HealthSignal::ClockStall) == DetectorSignal::ClockStall);
  CHECK(PostureCoordinator::from_health(HealthSignal::DiskPressure) ==
        DetectorSignal::ResourcePressure);
  CHECK(PostureCoordinator::from_health(HealthSignal::MemoryPressure) ==
        DetectorSignal::ResourcePressure);
  CHECK(PostureCoordinator::from_health(HealthSignal::HandlePressure) ==
        DetectorSignal::ResourcePressure);

  // Fed through evaluate: a DiskPressure breach yields exit-only.
  const PostureCoordinator coord;
  CHECK(coord.evaluate({PostureCoordinator::from_health(HealthSignal::DiskPressure)}) ==
        Posture::ExitOnly);
  CHECK(coord.evaluate({PostureCoordinator::from_health(HealthSignal::MuteFeed)}) ==
        Posture::BlockEntries);
}

TEST_CASE("evaluate_and_alert sends one level-by-severity alert only when degraded", "[modes]") {
  const PostureCoordinator coord;

  SECTION("Normal posture sends no alert") {
    CountingAlertSink alerts;
    CHECK(coord.evaluate_and_alert({}, Posture::Normal, alerts) == Posture::Normal);
    CHECK(alerts.count() == 0);
  }

  SECTION("BlockEntries posture sends one Warning alert") {
    CountingAlertSink alerts;
    CHECK(coord.evaluate_and_alert({DetectorSignal::StaleData}, Posture::Normal, alerts) ==
          Posture::BlockEntries);
    CHECK(alerts.count() == 1);
    CHECK(alerts.last_level() == AlertLevel::Warning);
    CHECK(alerts.last_message().find("BlockEntries") != std::string::npos);
  }

  SECTION("ExitOnly posture sends one Error alert") {
    CountingAlertSink alerts;
    CHECK(coord.evaluate_and_alert({DetectorSignal::BrokerDown}, Posture::Normal, alerts) ==
          Posture::ExitOnly);
    CHECK(alerts.count() == 1);
    CHECK(alerts.last_level() == AlertLevel::Error);
  }

  SECTION("SoftKill posture sends one Error alert") {
    CountingAlertSink alerts;
    CHECK(coord.evaluate_and_alert({DetectorSignal::RiskBreach}, Posture::Normal, alerts) ==
          Posture::SoftKill);
    CHECK(alerts.count() == 1);
    CHECK(alerts.last_level() == AlertLevel::Error);
  }

  SECTION("Panic via operator_floor sends one Critical alert") {
    CountingAlertSink alerts;
    CHECK(coord.evaluate_and_alert({DetectorSignal::MuteFeed}, Posture::Panic, alerts) ==
          Posture::Panic);
    CHECK(alerts.count() == 1);
    CHECK(alerts.last_level() == AlertLevel::Critical);
  }
}

TEST_CASE("no detector signal escalates beyond SoftKill (Panic is operator-only)") {
  const PostureCoordinator coord;
  // The whole detector vocabulary active at once: the severest a detector mix can
  // produce is SoftKill (RiskBreach); Panic is reachable ONLY via operator_floor.
  const std::vector<DetectorSignal> all_signals{
      DetectorSignal::StaleData, DetectorSignal::MuteFeed,      DetectorSignal::Unknown,
      DetectorSignal::Mismatch,  DetectorSignal::SessionExpiry, DetectorSignal::BrokerDown,
      DetectorSignal::ClockSkew, DetectorSignal::ClockStall,    DetectorSignal::ResourcePressure,
      DetectorSignal::RiskBreach};
  CHECK(coord.evaluate(all_signals) == Posture::SoftKill);
  CHECK(coord.evaluate(all_signals, Posture::Panic) == Posture::Panic);  // operator floor wins
}
