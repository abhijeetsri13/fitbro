#include "broker_exec/reconcile/corporate_actions.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/ports/alert_sink.hpp"

using broker_exec::domain::Position;
using broker_exec::domain::Price;
using broker_exec::domain::Quantity;
using broker_exec::ports::AlertLevel;

namespace rec = broker_exec::reconcile;

namespace {

// Records every alert so a test can assert that (and only that) the expected
// escalation happened. Tracks Error-level alerts separately so a CA-applied Info
// alert is not confused with the AC-2 fail-visible Error. Mirrors the sibling
// reconcile/manual_intervention CountingAlertSink.
class CountingAlertSink final : public broker_exec::ports::AlertSink {
 public:
  broker_exec::Result<broker_exec::ports::Ok> send(AlertLevel level,
                                                   const std::string& message) override {
    ++count_;
    if (level == AlertLevel::Error) {
      ++error_count_;
    }
    last_level_ = level;
    last_message_ = message;
    return broker_exec::ports::ok();
  }
  broker_exec::Result<broker_exec::ports::Ok> send_test_alert() override {
    return broker_exec::ports::ok();
  }

  [[nodiscard]] std::size_t count() const noexcept { return count_; }
  [[nodiscard]] std::size_t error_count() const noexcept { return error_count_; }
  [[nodiscard]] AlertLevel last_level() const noexcept { return last_level_; }
  [[nodiscard]] const std::string& last_message() const noexcept { return last_message_; }

 private:
  std::size_t count_ = 0;
  std::size_t error_count_ = 0;
  AlertLevel last_level_ = AlertLevel::Info;
  std::string last_message_;
};

// A configurable corporate-action source backed by a std::map<symbol, action>.
class FakeCorporateActionSource final : public rec::CorporateActionSource {
 public:
  void add(const std::string& symbol, rec::CorporateAction action) {
    actions_[symbol] = std::move(action);
  }

  [[nodiscard]] std::optional<rec::CorporateAction> action_for(
      std::string_view symbol) const override {
    const auto it = actions_.find(std::string(symbol));
    if (it == actions_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

 private:
  std::map<std::string, rec::CorporateAction> actions_;
};

// A position in a symbol at a signed net quantity and an avg price in paise.
Position position(const std::string& symbol, std::int64_t net_qty, std::int64_t paise) {
  Position p;
  p.symbol = symbol;
  p.net_qty = Quantity::of(net_qty);
  p.avg_price = Price::from_paise(paise);
  return p;
}

}  // namespace

// ── AC-1: a 1:2 split is classified, re-based exactly, value preserved ──

TEST_CASE("classify: a 1:2 split is recognised and re-based (qty doubles, price halves)",
          "[corporate_actions]") {
  CountingAlertSink alerts;
  FakeCorporateActionSource source;
  rec::CorporateAction split;
  split.symbol = "X";
  split.kind = rec::CorporateActionKind::Split;
  split.qty_num = 2;  // qty *= 2
  split.qty_den = 1;
  source.add("X", split);

  const rec::CorporateActionClassifier classifier(&source, alerts);

  const Position believed = position("X", 50, 10000);        // 50 @ ₹100
  const Position broker_observed = position("X", 100, 5000);  // 100 @ ₹50

  const auto outcome = classifier.classify(believed, broker_observed);

  CHECK(outcome.is_corporate_action);
  CHECK_FALSE(outcome.source_missing);
  CHECK(outcome.rebased.net_qty == Quantity::of(100));
  CHECK(outcome.rebased.avg_price.paise() == 5000);
  CHECK(outcome.rebased.symbol == "X");
  // No false-mismatch Error: a CA-applied note is at most an Info.
  CHECK(alerts.error_count() == 0);
  // Value is preserved exactly: 50 * 10000 == 100 * 5000.
  CHECK(believed.net_qty.value() * believed.avg_price.paise() ==
        outcome.rebased.net_qty.value() * outcome.rebased.avg_price.paise());
}

// ── AC-1: a 1:1 bonus (qty doubles, price halves) is classified, re-based ──

TEST_CASE("classify: a 1:1 bonus is recognised and re-based", "[corporate_actions]") {
  CountingAlertSink alerts;
  FakeCorporateActionSource source;
  rec::CorporateAction bonus;
  bonus.symbol = "Y";
  bonus.kind = rec::CorporateActionKind::Bonus;
  bonus.qty_num = 2;  // 1:1 bonus doubles the quantity
  bonus.qty_den = 1;
  source.add("Y", bonus);

  const rec::CorporateActionClassifier classifier(&source, alerts);

  const Position believed = position("Y", 30, 6000);         // 30 @ ₹60
  const Position broker_observed = position("Y", 60, 3000);  // 60 @ ₹30

  const auto outcome = classifier.classify(believed, broker_observed);

  CHECK(outcome.is_corporate_action);
  CHECK(outcome.rebased.net_qty == Quantity::of(60));
  CHECK(outcome.rebased.avg_price.paise() == 3000);
  CHECK(alerts.error_count() == 0);
}

// ── AC-1: a symbol change is recognised, symbol re-based (token noted) ──

TEST_CASE("classify: a symbol change re-bases the symbol", "[corporate_actions]") {
  CountingAlertSink alerts;
  FakeCorporateActionSource source;
  rec::CorporateAction change;
  change.symbol = "OLD";
  change.kind = rec::CorporateActionKind::SymbolChange;
  change.qty_num = 1;  // quantity/price unchanged
  change.qty_den = 1;
  change.new_symbol = "NEW";
  change.new_token = 999;  // re-resolved by the instrument master (carried only)
  source.add("OLD", change);

  const rec::CorporateActionClassifier classifier(&source, alerts);

  const Position believed = position("OLD", 50, 10000);
  const Position broker_observed = position("NEW", 50, 10000);

  const auto outcome = classifier.classify(believed, broker_observed);

  CHECK(outcome.is_corporate_action);
  CHECK(outcome.rebased.symbol == "NEW");
  CHECK(outcome.rebased.net_qty == Quantity::of(50));
  CHECK(alerts.error_count() == 0);
}

// ── NOT force-applied: a CA exists but does not explain the observed change ──

TEST_CASE("classify: a CA that does NOT match the observed change is not applied",
          "[corporate_actions]") {
  CountingAlertSink alerts;
  FakeCorporateActionSource source;
  rec::CorporateAction split;
  split.symbol = "X";
  split.kind = rec::CorporateActionKind::Split;
  split.qty_num = 2;  // the split would re-base 50 -> 100
  split.qty_den = 1;
  source.add("X", split);

  const rec::CorporateActionClassifier classifier(&source, alerts);

  const Position believed = position("X", 50, 10000);
  // Broker shows 30, not the split's 100 -> the CA does not explain it.
  const Position broker_observed = position("X", 30, 10000);

  const auto outcome = classifier.classify(believed, broker_observed);

  CHECK_FALSE(outcome.is_corporate_action);
  CHECK_FALSE(outcome.source_missing);  // a source IS configured
  CHECK(alerts.count() == 0);  // a non-matching CA must not alert (no false signal)
  // The caller treats this as a manual / mismatch (no CA was force-applied).
}

// ── AC-2: a missing source + a change is SURFACED (Error), not silently a CA ──

TEST_CASE("classify: a null source AND a change surfaces an Error (AC-2 fail-visible)",
          "[corporate_actions]") {
  CountingAlertSink alerts;
  // source == nullptr -> NOT configured.
  const rec::CorporateActionClassifier classifier(nullptr, alerts);

  const Position believed = position("X", 50, 10000);
  const Position broker_observed = position("X", 100, 5000);  // a change exists

  const auto outcome = classifier.classify(believed, broker_observed);

  CHECK(outcome.source_missing);
  CHECK_FALSE(outcome.is_corporate_action);  // NOT silently a CA, NOT silently manual
  CHECK(alerts.count() > 0);
  CHECK(alerts.error_count() > 0);
  CHECK(alerts.last_level() == AlertLevel::Error);
}

TEST_CASE("classify: a null source with NO change is a silent no-op (no alert)",
          "[corporate_actions]") {
  CountingAlertSink alerts;
  const rec::CorporateActionClassifier classifier(nullptr, alerts);

  const Position believed = position("X", 50, 10000);
  const auto outcome = classifier.classify(believed, believed);  // agree

  CHECK_FALSE(outcome.is_corporate_action);
  CHECK_FALSE(outcome.source_missing);
  CHECK(alerts.count() == 0);
}

// ── No change: believed == broker -> no-op, no alert ──

TEST_CASE("classify: no change (believed == broker) is a no-op", "[corporate_actions]") {
  CountingAlertSink alerts;
  FakeCorporateActionSource source;  // configured, but irrelevant: nothing changed
  const rec::CorporateActionClassifier classifier(&source, alerts);

  const Position believed = position("X", 50, 10000);
  const auto outcome = classifier.classify(believed, believed);

  CHECK_FALSE(outcome.is_corporate_action);
  CHECK_FALSE(outcome.source_missing);
  CHECK(alerts.count() == 0);
}

// ── Re-base math: exact integer qty/price, value preserved ──

TEST_CASE("rebase: value is preserved exactly with integer math", "[corporate_actions]") {
  rec::CorporateAction split;
  split.qty_num = 2;
  split.qty_den = 1;

  const Position pos = position("X", 50, 10000);
  const Position rebased = rec::CorporateActionClassifier::rebase(pos, split);

  CHECK(rebased.net_qty == Quantity::of(100));
  CHECK(rebased.avg_price.paise() == 5000);
  // value preserved: 50 * 10000 == 100 * 5000.
  CHECK(pos.net_qty.value() * pos.avg_price.paise() ==
        rebased.net_qty.value() * rebased.avg_price.paise());
}

// ── Re-base math: a non-divisible quantity is integer-truncated, never fractional ──

TEST_CASE("rebase: a non-divisible split truncates the quantity (no fractional share)",
          "[corporate_actions]") {
  // qty_num 1 / qty_den 2 would halve an odd quantity (51 -> 25.5): the integer
  // result is truncated toward zero to 25 — NEVER a fractional share.
  rec::CorporateAction halve;
  halve.qty_num = 1;
  halve.qty_den = 2;

  const Position pos = position("X", 51, 10000);
  const Position rebased = rec::CorporateActionClassifier::rebase(pos, halve);

  CHECK(rebased.net_qty == Quantity::of(25));   // 51 / 2 truncated, not 25.5
  CHECK(rebased.avg_price.paise() == 20000);    // price doubles (inverse multiplier)
}

TEST_CASE("classify: a non-divisible matching CA is applied and the truncation is noted",
          "[corporate_actions]") {
  CountingAlertSink alerts;
  FakeCorporateActionSource source;
  rec::CorporateAction halve;
  halve.symbol = "X";
  halve.kind = rec::CorporateActionKind::FnoAdjustment;
  halve.qty_num = 1;
  halve.qty_den = 2;
  source.add("X", halve);

  const rec::CorporateActionClassifier classifier(&source, alerts);

  const Position believed = position("X", 51, 10000);
  // The truncated re-base (25 @ 20000 paise) is what the broker reports.
  const Position broker_observed = position("X", 25, 20000);

  const auto outcome = classifier.classify(believed, broker_observed);

  CHECK(outcome.is_corporate_action);
  CHECK(outcome.rebased.net_qty == Quantity::of(25));
  CHECK(outcome.detail.find("truncated") != std::string::npos);
  CHECK(alerts.error_count() == 0);
}

// ── Short (negative net) position re-bases by sign-correct integer math ──

TEST_CASE("rebase: a short position re-bases with sign preserved", "[corporate_actions]") {
  rec::CorporateAction split;
  split.qty_num = 2;
  split.qty_den = 1;

  const Position pos = position("X", -50, 10000);  // short 50
  const Position rebased = rec::CorporateActionClassifier::rebase(pos, split);

  CHECK(rebased.net_qty == Quantity::of(-100));  // still short, doubled
  CHECK(rebased.avg_price.paise() == 5000);
}
