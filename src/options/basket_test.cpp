#include "broker_exec/options/basket.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

using broker_exec::Result;
using broker_exec::options::BasketConfig;
using broker_exec::options::BasketLeg;
using broker_exec::options::BasketOutcome;
using broker_exec::options::BasketResult;
using broker_exec::options::BasketSeams;
using broker_exec::options::execute_basket;
using broker_exec::options::LegFailurePolicy;
using broker_exec::options::LegResult;
using broker_exec::options::LegStatus;
using broker_exec::ports::AlertLevel;

namespace ports = broker_exec::ports;
namespace errors = broker_exec::errors;

namespace {

// Spy AlertSink: records the alert count + last level/message (same shape as the
// hedge_first / health recording sinks). Redefined locally in this TU.
class SpyAlertSink final : public ports::AlertSink {
 public:
  explicit SpyAlertSink(bool fail_send = false) : fail_send_(fail_send) {}

  Result<ports::Ok> send(AlertLevel level, const std::string& message) override {
    ++count_;
    last_level_ = level;
    last_message_ = message;
    if (fail_send_) {
      return broker_exec::fail(
          errors::make_error(errors::ErrorCategory::Network, "alert channel down"));
    }
    return ports::ok();
  }
  Result<ports::Ok> send_test_alert() override { return ports::ok(); }

  [[nodiscard]] std::size_t count() const noexcept { return count_; }
  [[nodiscard]] AlertLevel last_level() const noexcept { return last_level_; }
  [[nodiscard]] const std::string& last_message() const noexcept { return last_message_; }

 private:
  bool fail_send_;
  std::size_t count_ = 0;
  AlertLevel last_level_ = AlertLevel::Info;
  std::string last_message_;
};

// ── Seam result helpers ───────────────────────────────────────────────────
[[nodiscard]] Result<ports::BrokerAck> ok_ack(std::string id) {
  return ports::BrokerAck{std::move(id), "client-ref"};
}
// A DEFINITIVE broker rejection: Validation carries SuggestedAction::DoNotRetry,
// i.e. the broker gave a verdict and the order does NOT exist. This is the ONLY
// kind of placement error that may leave a leg `Failed` and license the unwind.
// KEEP IT DEFINITIVE: Network/Timeout/Unknown are reconcile-first categories, so
// putting one back here would silently retarget every unwind test onto the
// ambiguous path — which is how this suite came to enshrine the unsafe rollback.
[[nodiscard]] Result<ports::BrokerAck> ack_rejected() {
  return broker_exec::fail(errors::make_error(errors::ErrorCategory::Validation, "rejected"));
}
// An AMBIGUOUS placement outcome: the order MAY have reached the exchange, so no
// order_id ever came back and nothing about the leg may be assumed.
[[nodiscard]] Result<ports::BrokerAck> ack_ambiguous(errors::ErrorCategory category) {
  return broker_exec::fail(errors::make_error(category, "placement outcome unknown"));
}
// Ambiguous by ACTION alone: Internal defaults to RaiseAlert, so only the explicit
// SuggestedAction::ReconcileFirst can make this leg ambiguous. Pins the action arm
// of the classifier independently of the category arm.
[[nodiscard]] Result<ports::BrokerAck> ack_reconcile_first_action() {
  return broker_exec::fail(errors::make_error(errors::ErrorCategory::Internal,
                                              errors::SuggestedAction::ReconcileFirst,
                                              "ambiguous by action"));
}
[[nodiscard]] Result<ports::Ok> unwind_error() {
  return broker_exec::fail(
      errors::make_error(errors::ErrorCategory::BrokerRejected, "cancel rejected"));
}

// The deterministic broker order id a leg gets when placed.
[[nodiscard]] std::string order_id_for(const std::string& leg_id) {
  return "ord-" + leg_id;
}

// Count occurrences of a value in a call/placement log.
[[nodiscard]] std::size_t count_of(const std::vector<std::string>& log, const std::string& v) {
  return static_cast<std::size_t>(std::count(log.begin(), log.end(), v));
}

// Find a leg's result by leg_id (nullptr if absent).
[[nodiscard]] const LegResult* find_leg(const BasketResult& r, const std::string& id) {
  for (const LegResult& l : r.legs) {
    if (l.leg_id == id) {
      return &l;
    }
  }
  return nullptr;
}

}  // namespace

// ── AC-1: dependency honored (the never-orphan invariant) ────────────────────

TEST_CASE(
    "AC-1 dependency honored: prereq fails => dependent SkippedUnmetDependency, NEVER placed") {
  std::vector<std::string> place_log;
  std::vector<std::string> unwind_log;
  std::set<std::string> fail_ids = {"A"};  // root A fails
  SpyAlertSink alerts;

  BasketSeams seams;
  seams.place_leg = [&](const BasketLeg& leg) -> Result<ports::BrokerAck> {
    place_log.push_back(leg.leg_id);
    if (fail_ids.count(leg.leg_id) != 0) {
      return ack_rejected();
    }
    return ok_ack(order_id_for(leg.leg_id));
  };
  seams.unwind_leg = [&](const std::string& oid) -> Result<ports::Ok> {
    unwind_log.push_back(oid);
    return ports::ok();
  };

  // A (fails) <- B <- C  : transitive skip.
  const std::vector<BasketLeg> legs = {BasketLeg{"A", {}}, BasketLeg{"B", {"A"}},
                                       BasketLeg{"C", {"B"}}};

  const BasketResult result = execute_basket(legs, BasketConfig{}, seams, alerts);

  // CRITICAL: place_leg was NEVER called for the skipped legs (log assertion).
  CHECK(count_of(place_log, "A") == 1);
  CHECK(count_of(place_log, "B") == 0);
  CHECK(count_of(place_log, "C") == 0);

  CHECK(find_leg(result, "A")->status == LegStatus::Failed);
  CHECK(find_leg(result, "B")->status == LegStatus::SkippedUnmetDependency);
  CHECK(find_leg(result, "C")->status == LegStatus::SkippedUnmetDependency);
  CHECK(result.outcome != BasketOutcome::Complete);  // partial detected
}

TEST_CASE("AC-1 happy path: 3 independent legs all Executed => Complete, no alert, no unwind") {
  std::vector<std::string> place_log;
  std::vector<std::string> unwind_log;
  SpyAlertSink alerts;

  BasketSeams seams;
  seams.place_leg = [&](const BasketLeg& leg) -> Result<ports::BrokerAck> {
    place_log.push_back(leg.leg_id);
    return ok_ack(order_id_for(leg.leg_id));
  };
  seams.unwind_leg = [&](const std::string& oid) -> Result<ports::Ok> {
    unwind_log.push_back(oid);
    return ports::ok();
  };

  const std::vector<BasketLeg> legs = {BasketLeg{"A", {}}, BasketLeg{"B", {}}, BasketLeg{"C", {}}};

  const BasketResult result = execute_basket(legs, BasketConfig{}, seams, alerts);

  CHECK(result.outcome == BasketOutcome::Complete);
  CHECK(find_leg(result, "A")->status == LegStatus::Executed);
  CHECK(find_leg(result, "B")->status == LegStatus::Executed);
  CHECK(find_leg(result, "C")->status == LegStatus::Executed);
  CHECK(find_leg(result, "A")->order_id == "ord-A");
  CHECK(alerts.count() == 0);  // no alert on a whole basket
  CHECK(unwind_log.empty());   // nothing unwound
  CHECK(place_log.size() == 3);
}

TEST_CASE("AC-1 partial detection: 1 of 3 fails => outcome is a partial (not Complete)") {
  std::vector<std::string> place_log;
  std::set<std::string> fail_ids = {"C"};
  SpyAlertSink alerts;

  BasketSeams seams;
  seams.place_leg = [&](const BasketLeg& leg) -> Result<ports::BrokerAck> {
    place_log.push_back(leg.leg_id);
    if (fail_ids.count(leg.leg_id) != 0) {
      return ack_rejected();
    }
    return ok_ack(order_id_for(leg.leg_id));
  };
  seams.unwind_leg = [&](const std::string&) -> Result<ports::Ok> { return ports::ok(); };

  const std::vector<BasketLeg> legs = {BasketLeg{"A", {}}, BasketLeg{"B", {}}, BasketLeg{"C", {}}};

  const BasketResult result = execute_basket(legs, BasketConfig{}, seams, alerts);

  CHECK(result.outcome != BasketOutcome::Complete);
  CHECK(find_leg(result, "C")->status == LegStatus::Failed);
}

// ── AC-2: partial policy ─────────────────────────────────────────────────────

TEST_CASE(
    "AC-2 UnwindExecuted: C fails => A,B unwound NEWEST-FIRST, Critical alert, "
    "PartiallyExecutedUnwound") {
  std::vector<std::string> place_log;
  std::vector<std::string> unwind_log;
  std::set<std::string> fail_ids = {"C"};
  SpyAlertSink alerts;

  BasketSeams seams;
  seams.place_leg = [&](const BasketLeg& leg) -> Result<ports::BrokerAck> {
    place_log.push_back(leg.leg_id);
    if (fail_ids.count(leg.leg_id) != 0) {
      return ack_rejected();
    }
    return ok_ack(order_id_for(leg.leg_id));
  };
  seams.unwind_leg = [&](const std::string& oid) -> Result<ports::Ok> {
    unwind_log.push_back(oid);
    return ports::ok();
  };

  BasketConfig config;
  config.on_leg_failure = LegFailurePolicy::UnwindExecuted;

  // A, B execute in order, then C fails.
  const std::vector<BasketLeg> legs = {BasketLeg{"A", {}}, BasketLeg{"B", {}}, BasketLeg{"C", {}}};

  const BasketResult result = execute_basket(legs, config, seams, alerts);

  CHECK(result.outcome == BasketOutcome::PartiallyExecutedUnwound);
  // NEWEST-FIRST: B (placed last of the executed pair) is unwound before A.
  CHECK(unwind_log == std::vector<std::string>{"ord-B", "ord-A"});
  CHECK(find_leg(result, "A")->status == LegStatus::Unwound);
  CHECK(find_leg(result, "B")->status == LegStatus::Unwound);
  CHECK(find_leg(result, "C")->status == LegStatus::Failed);
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Critical);
}

TEST_CASE(
    "AC-2 unwind failure is VISIBLE: failed unwind leaves leg Executed (not Unwound) + escalated") {
  std::vector<std::string> unwind_log;
  std::set<std::string> place_fail = {"C"};
  std::set<std::string> unwind_fail = {"ord-B"};  // B cannot be unwound
  SpyAlertSink alerts;

  BasketSeams seams;
  seams.place_leg = [&](const BasketLeg& leg) -> Result<ports::BrokerAck> {
    if (place_fail.count(leg.leg_id) != 0) {
      return ack_rejected();
    }
    return ok_ack(order_id_for(leg.leg_id));
  };
  seams.unwind_leg = [&](const std::string& oid) -> Result<ports::Ok> {
    unwind_log.push_back(oid);
    if (unwind_fail.count(oid) != 0) {
      return unwind_error();
    }
    return ports::ok();
  };

  const std::vector<BasketLeg> legs = {BasketLeg{"A", {}}, BasketLeg{"B", {}}, BasketLeg{"C", {}}};

  const BasketResult result = execute_basket(legs, BasketConfig{}, seams, alerts);

  CHECK(result.outcome == BasketOutcome::PartiallyExecutedUnwound);
  // B's unwind errored => it stays Executed (a LIVE orphan) and is escalated.
  CHECK(find_leg(result, "B")->status == LegStatus::Executed);
  CHECK(find_leg(result, "B")->detail.find("CRITICAL") != std::string::npos);
  // A unwound cleanly.
  CHECK(find_leg(result, "A")->status == LegStatus::Unwound);
  // The unwind was attempted for both (never silently skipped).
  CHECK(count_of(unwind_log, "ord-B") == 1);
  CHECK(count_of(unwind_log, "ord-A") == 1);
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Critical);
  // The LOUDEST channel must NAME the still-live orphan, not bury it in per-leg
  // detail: the Critical alert text mentions leg B and flags it LIVE.
  CHECK(alerts.last_message().find("B") != std::string::npos);
  CHECK(alerts.last_message().find("LIVE") != std::string::npos);
}

TEST_CASE(
    "AC-2 LeaveAndAlert: a leg fails => executed legs NOT unwound, Warning alert, "
    "PartiallyExecutedLeft") {
  std::vector<std::string> unwind_log;
  std::set<std::string> fail_ids = {"C"};
  SpyAlertSink alerts;

  BasketSeams seams;
  seams.place_leg = [&](const BasketLeg& leg) -> Result<ports::BrokerAck> {
    if (fail_ids.count(leg.leg_id) != 0) {
      return ack_rejected();
    }
    return ok_ack(order_id_for(leg.leg_id));
  };
  seams.unwind_leg = [&](const std::string& oid) -> Result<ports::Ok> {
    unwind_log.push_back(oid);  // must never be reached
    return ports::ok();
  };

  BasketConfig config;
  config.on_leg_failure = LegFailurePolicy::LeaveAndAlert;

  const std::vector<BasketLeg> legs = {BasketLeg{"A", {}}, BasketLeg{"B", {}}, BasketLeg{"C", {}}};

  const BasketResult result = execute_basket(legs, config, seams, alerts);

  CHECK(result.outcome == BasketOutcome::PartiallyExecutedLeft);
  CHECK(unwind_log.empty());  // unwind_leg NEVER called under LeaveAndAlert
  CHECK(find_leg(result, "A")->status == LegStatus::Executed);
  CHECK(find_leg(result, "B")->status == LegStatus::Executed);
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Warning);
}

// ── AC-3: single-unit tracking ───────────────────────────────────────────────

TEST_CASE(
    "AC-3 single-unit: default true => tracked + basket_id set; explicit false => not tracked") {
  SpyAlertSink alerts;
  BasketSeams seams;
  seams.place_leg = [&](const BasketLeg& leg) -> Result<ports::BrokerAck> {
    return ok_ack(order_id_for(leg.leg_id));
  };

  const std::vector<BasketLeg> legs = {BasketLeg{"A", {}}, BasketLeg{"B", {}}};

  SECTION("default config tracks as one unit and carries the configured basket_id") {
    BasketConfig config;  // track_as_single_unit defaults true
    config.basket_id = "BSK-42";
    const BasketResult result = execute_basket(legs, config, seams, alerts);
    CHECK(result.tracked_as_single_unit);
    CHECK(result.basket_id == "BSK-42");
  }

  SECTION("empty basket_id => deterministic fallback (no clock/random)") {
    BasketConfig config;  // basket_id empty
    const BasketResult a = execute_basket(legs, config, seams, alerts);
    const BasketResult b = execute_basket(legs, config, seams, alerts);
    CHECK_FALSE(a.basket_id.empty());
    CHECK(a.basket_id == b.basket_id);  // deterministic
  }

  SECTION("explicit opt-out => not tracked as a single unit") {
    BasketConfig config;
    config.track_as_single_unit = false;
    const BasketResult result = execute_basket(legs, config, seams, alerts);
    CHECK_FALSE(result.tracked_as_single_unit);
  }
}

// ── Fail-closed pre-flight: nothing is ever placed ───────────────────────────

TEST_CASE("Fail-closed pre-flight: dependency CYCLE => Blocked, place_leg NEVER called") {
  std::vector<std::string> place_log;
  SpyAlertSink alerts;
  BasketSeams seams;
  seams.place_leg = [&](const BasketLeg& leg) -> Result<ports::BrokerAck> {
    place_log.push_back(leg.leg_id);
    return ok_ack(order_id_for(leg.leg_id));
  };

  // A -> B -> A
  const std::vector<BasketLeg> legs = {BasketLeg{"A", {"B"}}, BasketLeg{"B", {"A"}}};

  const BasketResult result = execute_basket(legs, BasketConfig{}, seams, alerts);

  CHECK(result.outcome == BasketOutcome::Blocked);
  CHECK(place_log.empty());  // total place_leg call count == 0
  CHECK(alerts.count() == 0);
}

TEST_CASE("Fail-closed pre-flight: unknown dependency id => Blocked, nothing placed") {
  std::vector<std::string> place_log;
  SpyAlertSink alerts;
  BasketSeams seams;
  seams.place_leg = [&](const BasketLeg& leg) -> Result<ports::BrokerAck> {
    place_log.push_back(leg.leg_id);
    return ok_ack(order_id_for(leg.leg_id));
  };

  const std::vector<BasketLeg> legs = {BasketLeg{"A", {"DOES-NOT-EXIST"}}};

  const BasketResult result = execute_basket(legs, BasketConfig{}, seams, alerts);

  CHECK(result.outcome == BasketOutcome::Blocked);
  CHECK(place_log.empty());
}

TEST_CASE("Fail-closed pre-flight: duplicate leg_id => Blocked, nothing placed") {
  std::vector<std::string> place_log;
  SpyAlertSink alerts;
  BasketSeams seams;
  seams.place_leg = [&](const BasketLeg& leg) -> Result<ports::BrokerAck> {
    place_log.push_back(leg.leg_id);
    return ok_ack(order_id_for(leg.leg_id));
  };

  const std::vector<BasketLeg> legs = {BasketLeg{"A", {}}, BasketLeg{"A", {}}};

  const BasketResult result = execute_basket(legs, BasketConfig{}, seams, alerts);

  CHECK(result.outcome == BasketOutcome::Blocked);
  CHECK(place_log.empty());
}

TEST_CASE("Fail-closed pre-flight: empty leg_id => Blocked, nothing placed") {
  std::vector<std::string> place_log;
  SpyAlertSink alerts;
  BasketSeams seams;
  seams.place_leg = [&](const BasketLeg& leg) -> Result<ports::BrokerAck> {
    place_log.push_back(leg.leg_id);
    return ok_ack(order_id_for(leg.leg_id));
  };

  const std::vector<BasketLeg> legs = {BasketLeg{"", {}}};

  const BasketResult result = execute_basket(legs, BasketConfig{}, seams, alerts);

  CHECK(result.outcome == BasketOutcome::Blocked);
  CHECK(place_log.empty());
}

TEST_CASE("Fail-closed pre-flight: null place_leg seam => Blocked") {
  SpyAlertSink alerts;
  BasketSeams seams;  // place_leg is null

  const std::vector<BasketLeg> legs = {BasketLeg{"A", {}}, BasketLeg{"B", {}}};

  const BasketResult result = execute_basket(legs, BasketConfig{}, seams, alerts);

  CHECK(result.outcome == BasketOutcome::Blocked);
}

// ── Ambiguous placement: the naked-position guard ────────────────────────────

// THE DEFECT THESE PIN: `place_leg` used to record EVERY Error as `Failed` — a
// definitive "the order does not exist" verdict — so an ambiguous outcome on the
// naked-risk leg licensed the UnwindExecuted rollback to cancel the PROTECTIVE leg
// underneath it. Against the old code the basket below unwound "ord-hedge" and
// reported PartiallyExecutedUnwound / "executed legs unwound per policy".

TEST_CASE(
    "AMBIGUOUS risk leg NEVER unwinds the hedge: timeout on the short => nothing cancelled, "
    "ReconcileRequired") {
  std::vector<std::string> place_log;
  std::vector<std::string> unwind_log;
  SpyAlertSink alerts;

  BasketSeams seams;
  seams.place_leg = [&](const BasketLeg& leg) -> Result<ports::BrokerAck> {
    place_log.push_back(leg.leg_id);
    if (leg.leg_id == "short") {
      return ack_ambiguous(errors::ErrorCategory::Timeout);  // may ALREADY be at the exchange
    }
    return ok_ack(order_id_for(leg.leg_id));
  };
  seams.unwind_leg = [&](const std::string& oid) -> Result<ports::Ok> {
    unwind_log.push_back(oid);
    return ports::ok();
  };

  // hedge (protective long, root) <- short (the naked risk) <- tail.
  const std::vector<BasketLeg> legs = {BasketLeg{"hedge", {}}, BasketLeg{"short", {"hedge"}},
                                       BasketLeg{"tail", {"short"}}};

  const BasketResult result = execute_basket(legs, BasketConfig{}, seams, alerts);

  // THE LOAD-BEARING ASSERTION: the protective leg was NOT cancelled. Removing it
  // while the short's existence is unproven is what manufactures a naked short.
  CHECK(unwind_log.empty());
  CHECK(find_leg(result, "hedge")->status == LegStatus::Executed);
  CHECK(find_leg(result, "short")->status == LegStatus::AmbiguousMayBeLive);
  CHECK(result.outcome == BasketOutcome::ReconcileRequired);
  // No order_id was ever acked for the ambiguous leg — this library could not
  // cancel it even if it wanted to, which is why reconciliation is the only exit.
  CHECK(find_leg(result, "short")->order_id.empty());
  // The never-orphan invariant still holds through an AMBIGUOUS prerequisite.
  CHECK(count_of(place_log, "tail") == 0);
  CHECK(find_leg(result, "tail")->status == LegStatus::SkippedUnmetDependency);
  // The loudest channel must NAME the ambiguous leg and say nothing was unwound;
  // the old Critical alert said "all executed legs unwound per policy" instead.
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Critical);
  CHECK(alerts.last_message().find("short") != std::string::npos);
  CHECK(alerts.last_message().find("AMBIGUOUS") != std::string::npos);
  // The suppression is visible on the leg that was deliberately left live.
  CHECK(find_leg(result, "hedge")->detail.find("SUPPRESSED") != std::string::npos);
}

TEST_CASE("Every reconcile-first error is ambiguous; only a definitive verdict may unwind") {
  const auto run = [](const std::function<Result<ports::BrokerAck>()>& fail_c,
                      std::vector<std::string>& unwind_log, SpyAlertSink& alerts) -> BasketResult {
    BasketSeams seams;
    seams.place_leg = [&](const BasketLeg& leg) -> Result<ports::BrokerAck> {
      if (leg.leg_id == "C") {
        return fail_c();
      }
      return ok_ack(order_id_for(leg.leg_id));
    };
    seams.unwind_leg = [&](const std::string& oid) -> Result<ports::Ok> {
      unwind_log.push_back(oid);
      return ports::ok();
    };
    const std::vector<BasketLeg> legs = {BasketLeg{"A", {}}, BasketLeg{"B", {}},
                                         BasketLeg{"C", {}}};
    return execute_basket(legs, BasketConfig{}, seams, alerts);
  };

  SECTION("Timeout / Network / Unknown categories are ALL ambiguous") {
    // Exactly the categories Dispatcher::is_reconcile_first calls "may be live";
    // the basket must agree with that rule category for category, not approximate it.
    const std::vector<errors::ErrorCategory> ambiguous = {errors::ErrorCategory::Timeout,
                                                          errors::ErrorCategory::Network,
                                                          errors::ErrorCategory::Unknown};
    for (const errors::ErrorCategory category : ambiguous) {
      INFO("category: " << errors::to_string(category));
      std::vector<std::string> unwind_log;
      SpyAlertSink alerts;
      const BasketResult result =
          run([category]() { return ack_ambiguous(category); }, unwind_log, alerts);
      CHECK(result.outcome == BasketOutcome::ReconcileRequired);
      CHECK(find_leg(result, "C")->status == LegStatus::AmbiguousMayBeLive);
      CHECK(unwind_log.empty());  // A and B stay LIVE, untouched
    }
  }

  SECTION("SuggestedAction::ReconcileFirst alone is enough, whatever the category") {
    std::vector<std::string> unwind_log;
    SpyAlertSink alerts;
    const BasketResult result = run(ack_reconcile_first_action, unwind_log, alerts);
    CHECK(result.outcome == BasketOutcome::ReconcileRequired);
    CHECK(find_leg(result, "C")->status == LegStatus::AmbiguousMayBeLive);
    CHECK(unwind_log.empty());
  }

  SECTION("a DEFINITIVE DoNotRetry rejection still unwinds (the guard is not blanket)") {
    std::vector<std::string> unwind_log;
    SpyAlertSink alerts;
    const BasketResult result = run(ack_rejected, unwind_log, alerts);
    CHECK(result.outcome == BasketOutcome::PartiallyExecutedUnwound);
    CHECK(find_leg(result, "C")->status == LegStatus::Failed);
    CHECK(unwind_log == std::vector<std::string>{"ord-B", "ord-A"});
  }
}

TEST_CASE("Ambiguity OUTRANKS a definitive failure in the same basket: still no unwind") {
  std::vector<std::string> unwind_log;
  SpyAlertSink alerts;

  BasketSeams seams;
  seams.place_leg = [&](const BasketLeg& leg) -> Result<ports::BrokerAck> {
    if (leg.leg_id == "B") {
      return ack_rejected();  // definitive: on its own this would license the unwind
    }
    if (leg.leg_id == "C") {
      return ack_ambiguous(errors::ErrorCategory::Timeout);
    }
    return ok_ack(order_id_for(leg.leg_id));
  };
  seams.unwind_leg = [&](const std::string& oid) -> Result<ports::Ok> {
    unwind_log.push_back(oid);
    return ports::ok();
  };

  const std::vector<BasketLeg> legs = {BasketLeg{"A", {}}, BasketLeg{"B", {}}, BasketLeg{"C", {}}};

  const BasketResult result = execute_basket(legs, BasketConfig{}, seams, alerts);

  // ONE ambiguous leg vetoes the whole rollback — a clean rejection elsewhere in
  // the basket does not buy back the right to cancel a possibly-protective leg.
  CHECK(result.outcome == BasketOutcome::ReconcileRequired);
  CHECK(unwind_log.empty());
  CHECK(find_leg(result, "A")->status == LegStatus::Executed);
  CHECK(find_leg(result, "B")->status == LegStatus::Failed);
  CHECK(find_leg(result, "C")->status == LegStatus::AmbiguousMayBeLive);
}

TEST_CASE("Ambiguous leg under LeaveAndAlert: ReconcileRequired + CRITICAL, not a plain partial") {
  SpyAlertSink alerts;

  BasketSeams seams;
  seams.place_leg = [&](const BasketLeg& leg) -> Result<ports::BrokerAck> {
    if (leg.leg_id == "unproven") {
      return ack_ambiguous(errors::ErrorCategory::Network);
    }
    return ok_ack(order_id_for(leg.leg_id));
  };

  BasketConfig config;
  config.on_leg_failure = LegFailurePolicy::LeaveAndAlert;

  const std::vector<BasketLeg> legs = {BasketLeg{"settled", {}}, BasketLeg{"unproven", {}}};

  const BasketResult result = execute_basket(legs, config, seams, alerts);

  // LeaveAndAlert already leaves the legs alone, but it reported a Warning-level
  // "partial" — which tells nobody that an order may be LIVE at the broker.
  CHECK(result.outcome == BasketOutcome::ReconcileRequired);
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Critical);
  CHECK(alerts.last_message().find("unproven") != std::string::npos);
}

TEST_CASE("An ambiguous leg can NEVER read as Complete") {
  SpyAlertSink alerts;

  BasketSeams seams;
  seams.place_leg = [&](const BasketLeg&) -> Result<ports::BrokerAck> {
    return ack_ambiguous(errors::ErrorCategory::Timeout);
  };

  const std::vector<BasketLeg> legs = {BasketLeg{"solo", {}}};

  const BasketResult result = execute_basket(legs, BasketConfig{}, seams, alerts);

  // AmbiguousMayBeLive is neither Failed nor Skipped, so a partial scan that looks
  // only for those two would let a possibly-live order pass as a whole basket.
  CHECK(result.outcome == BasketOutcome::ReconcileRequired);
  CHECK(alerts.count() == 1);
}

// ── to_string: stable names ──────────────────────────────────────────────────

TEST_CASE("to_string: stable LegStatus + BasketOutcome names") {
  CHECK(broker_exec::options::to_string(LegStatus::Executed) == "Executed");
  CHECK(broker_exec::options::to_string(LegStatus::Failed) == "Failed");
  CHECK(broker_exec::options::to_string(LegStatus::SkippedUnmetDependency) ==
        "SkippedUnmetDependency");
  CHECK(broker_exec::options::to_string(LegStatus::Unwound) == "Unwound");
  CHECK(broker_exec::options::to_string(LegStatus::AmbiguousMayBeLive) == "AmbiguousMayBeLive");
  CHECK(broker_exec::options::to_string(BasketOutcome::Complete) == "Complete");
  CHECK(broker_exec::options::to_string(BasketOutcome::PartiallyExecutedUnwound) ==
        "PartiallyExecutedUnwound");
  CHECK(broker_exec::options::to_string(BasketOutcome::PartiallyExecutedLeft) ==
        "PartiallyExecutedLeft");
  CHECK(broker_exec::options::to_string(BasketOutcome::Blocked) == "Blocked");
  CHECK(broker_exec::options::to_string(BasketOutcome::ReconcileRequired) == "ReconcileRequired");
}
