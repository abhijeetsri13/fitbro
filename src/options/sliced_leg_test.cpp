#include "broker_exec/options/sliced_leg.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

using broker_exec::Result;
using broker_exec::options::ChildPlacement;
using broker_exec::options::ChildResult;
using broker_exec::options::execute_sliced_leg;
using broker_exec::options::SlicedLegOutcome;
using broker_exec::options::SlicedLegResult;
using broker_exec::options::SlicedLegSeams;
using broker_exec::ports::AlertLevel;

namespace domain = broker_exec::domain;
namespace ports = broker_exec::ports;
namespace errors = broker_exec::errors;

namespace {

using PlaceResult = Result<std::pair<ChildPlacement, std::string>>;

// Spy AlertSink: records the alert count + last level/message (same shape as the
// basket / hedge_first recording sinks). Redefined locally in this TU.
class SpyAlertSink final : public ports::AlertSink {
 public:
  explicit SpyAlertSink(bool fail_send = false) : fail_send_(fail_send) {}

  Result<ports::Ok> send(AlertLevel level, const std::string& message) override {
    ++count_;
    last_level_ = level;
    last_message_ = message;
    last_provenance_ = ports::AlertContext{};
    if (fail_send_) {
      return broker_exec::fail(
          errors::make_error(errors::ErrorCategory::Network, "alert channel down"));
    }
    return ports::ok();
  }
  // IMP-16: the child ref now rides in the TYPED context instead of being
  // interpolated into the free-form body (a sink scrubs the body, and a slicer
  // child ref `<parent>#<k>` is one long token-shaped run, so the interpolated
  // form reached the operator as `child ***REDACTED***`). Overriding this — rather
  // than inheriting the base default, which drops the context — is what keeps the
  // "the alert names the child" assertion meaningful.
  Result<ports::Ok> send_with_context(AlertLevel level, const std::string& message,
                                      const ports::AlertContext& provenance) override {
    const Result<ports::Ok> out = send(level, message);
    last_provenance_ = provenance;
    return out;
  }
  Result<ports::Ok> send_test_alert() override { return ports::ok(); }

  [[nodiscard]] std::size_t count() const noexcept { return count_; }
  [[nodiscard]] AlertLevel last_level() const noexcept { return last_level_; }
  [[nodiscard]] const std::string& last_message() const noexcept { return last_message_; }
  [[nodiscard]] const ports::AlertContext& last_provenance() const noexcept {
    return last_provenance_;
  }

 private:
  bool fail_send_;
  std::size_t count_ = 0;
  AlertLevel last_level_ = AlertLevel::Info;
  std::string last_message_;
  ports::AlertContext last_provenance_;
};

// The owning strategy. ALL-LETTERS ON PURPOSE — see kParent.
const std::string kStrategy = "alpha";

// The parent client_ref the slicer fans into "<parent>#<k>" children.
//
// THIS MUST BE A REAL MINTED REF, NOT A SHORT STAND-IN. It is exactly what
// idempotency::make_client_ref spells — `<strategy>-<8 hex sig8>-<canonical
// RFC-4122 v4 uuid>` — because the provenance assertions below are only
// meaningful against the shape the library actually produces. The previous
// fixture ("S1-deadbeef-0001") was NOT that shape: "S1" is a HETEROGENEOUS
// segment (a letter and a digit in one unbroken run), which fails the homogeneity
// half of domain::is_provenance_id_shape, so the whole ref took the SCRUB FALLBACK
// instead of the allowlist. The test then proved the fallback, not the exemption —
// and since the stand-in was short enough to survive scrub() anyway, it would have
// passed even if the allowlist were deleted. A real 51-char minted ref is one
// token-shaped run that scrub() destroys, so the assertion now has teeth.
const std::string kParent = kStrategy + "-1a2b3c4d-deadbeef-cafe-4bab-8abe-0123456789ab";

// The deterministic child ref for k (mirrors slicing::child_ref's binding format).
[[nodiscard]] std::string ref(std::int64_t k) { return kParent + "#" + std::to_string(k); }

// An over-freeze parent: qty 30, lot 1, freeze 10 => the REAL FreezeSlicer makes
// chunk = (10/1)*1 = 10, full = 30/10 = 3, rem = 0 => EXACTLY 3 children of qty 10
// with refs "<parent>#1".."<parent>#3". (See freeze_slicer.cpp chunking.)
[[nodiscard]] domain::OrderIntent over_freeze_parent(std::string client_ref = kParent) {
  domain::OrderIntent parent;
  parent.client_ref = std::move(client_ref);
  parent.symbol = "NIFTY24JUN24000CE";
  parent.quantity = domain::Quantity::of(30);
  parent.price = domain::Price::from_rupees(100);
  parent.strategy = kStrategy;
  return parent;
}

[[nodiscard]] domain::Instrument instrument(std::int64_t lot = 1, std::int64_t freeze = 10) {
  domain::Instrument inst;
  inst.symbol = "NIFTY24JUN24000CE";
  inst.token = 12345;
  inst.exchange = "NFO";
  inst.lot_size = domain::Quantity::of(lot);
  inst.tick_size = domain::Price::from_paise(5);
  inst.freeze_qty = domain::Quantity::of(freeze);
  inst.expiry = "2024-06-27";
  return inst;
}

// ── Seam result helpers ───────────────────────────────────────────────────
[[nodiscard]] PlaceResult ack_place(std::string broker_order_id) {
  return std::pair<ChildPlacement, std::string>{ChildPlacement::Acked, std::move(broker_order_id)};
}
[[nodiscard]] PlaceResult unknown_place() {
  return std::pair<ChildPlacement, std::string>{ChildPlacement::Unknown, ""};
}
[[nodiscard]] PlaceResult place_error() {
  return broker_exec::fail(errors::make_error(errors::ErrorCategory::Network, "place failed"));
}
[[nodiscard]] Result<bool> already_error() {
  return broker_exec::fail(errors::make_error(errors::ErrorCategory::Timeout, "probe timed out"));
}

// The deterministic broker order id a child gets when placed.
[[nodiscard]] std::string order_id_for(const std::string& child_ref) { return "ord-" + child_ref; }

// Count occurrences of a value in a call/placement log.
[[nodiscard]] std::size_t count_of(const std::vector<std::string>& log, const std::string& v) {
  return static_cast<std::size_t>(std::count(log.begin(), log.end(), v));
}

}  // namespace

// ── AC-1: deterministic slice, all children placed ───────────────────────────

TEST_CASE("AC-1 deterministic slice: over-freeze parent => refs #1..#3 in order, all Acked, FilledSliced") {
  std::vector<std::string> place_log;
  SpyAlertSink alerts;

  SlicedLegSeams seams;
  seams.place_child = [&](const domain::OrderIntent& child) -> PlaceResult {
    place_log.push_back(child.client_ref);
    return ack_place(order_id_for(child.client_ref));
  };

  const SlicedLegResult result =
      execute_sliced_leg(over_freeze_parent(), instrument(), seams, alerts);

  CHECK(result.outcome == SlicedLegOutcome::FilledSliced);
  CHECK(result.placed_count == 3);
  CHECK(result.deduped_count == 0);
  // The REAL FreezeSlicer's deterministic refs, in order.
  REQUIRE(result.children.size() == 3);
  CHECK(result.children[0].client_ref == ref(1));
  CHECK(result.children[1].client_ref == ref(2));
  CHECK(result.children[2].client_ref == ref(3));
  // place_child was called once per child, in slice order.
  CHECK(place_log == std::vector<std::string>{ref(1), ref(2), ref(3)});
  // Each Acked child carries its broker order id; no alert on a whole leg.
  CHECK(result.children[0].placement == ChildPlacement::Acked);
  CHECK(result.children[0].broker_order_id == order_id_for(ref(1)));
  CHECK(alerts.count() == 0);
}

// ── IMP-11 AC-4: the trigger reaches the CHILDREN the executor actually places ─

TEST_CASE("IMP-11: a sliced STOP leg places children that are still stops",
          "[options][slicing][IMP-11]") {
  // freeze_slicer_test pins that the SLICER copies the trigger. This pins the
  // thing that actually matters end-to-end: the intents handed to `place_child`
  // — i.e. what would go on the wire — are still armed stops. An executor that
  // rebuilt a child intent instead of forwarding the slicer's would silently
  // place plain orders here, leaving the leg unprotected.
  std::vector<domain::OrderIntent> placed;
  SpyAlertSink alerts;

  domain::OrderIntent parent = over_freeze_parent();
  parent.order_type = domain::OrderType::StopLoss;
  // A well-formed BUY stop (over_freeze_parent leaves the default Buy side):
  // arms at 100, then works UP to 101 — limit >= trigger, as the gate requires.
  parent.trigger_price = domain::Price::from_rupees(100);
  parent.price = domain::Price::from_rupees(101);

  SlicedLegSeams seams;
  seams.place_child = [&](const domain::OrderIntent& child) -> PlaceResult {
    placed.push_back(child);
    return ack_place(order_id_for(child.client_ref));
  };

  const SlicedLegResult result = execute_sliced_leg(parent, instrument(), seams, alerts);

  CHECK(result.outcome == SlicedLegOutcome::FilledSliced);
  REQUIRE(placed.size() == 3);
  for (const domain::OrderIntent& child : placed) {
    CHECK(child.order_type == domain::OrderType::StopLoss);
    REQUIRE(child.trigger_price.has_value());
    CHECK(*child.trigger_price == domain::Price::from_rupees(100));
    CHECK(child.price == domain::Price::from_rupees(101));
  }
}

// ── AC-2: SIGKILL recovery / no duplicate ────────────────────────────────────

TEST_CASE("AC-2 SIGKILL recovery: #1,#2 already placed => only #3 re-sent, deduped 2, FilledSliced") {
  std::vector<std::string> place_log;
  std::set<std::string> already = {ref(1), ref(2)};  // crash after the first two
  SpyAlertSink alerts;

  SlicedLegSeams seams;
  seams.already_placed = [&](const std::string& client_ref) -> Result<bool> {
    return already.count(client_ref) != 0;
  };
  seams.place_child = [&](const domain::OrderIntent& child) -> PlaceResult {
    place_log.push_back(child.client_ref);
    return ack_place(order_id_for(child.client_ref));
  };

  const SlicedLegResult result =
      execute_sliced_leg(over_freeze_parent(), instrument(), seams, alerts);

  CHECK(result.outcome == SlicedLegOutcome::FilledSliced);
  // CRITICAL: the already-placed children were NEVER re-sent (placement-log proof).
  CHECK(place_log == std::vector<std::string>{ref(3)});
  CHECK(count_of(place_log, ref(1)) == 0);
  CHECK(count_of(place_log, ref(2)) == 0);
  CHECK(result.deduped_count == 2);
  CHECK(result.placed_count == 1);
  CHECK(alerts.count() == 0);
}

TEST_CASE("AC-2 all-already-placed replay is a no-op: place_child never called, deduped N, FilledSliced") {
  std::vector<std::string> place_log;
  SpyAlertSink alerts;

  SlicedLegSeams seams;
  seams.already_placed = [&](const std::string&) -> Result<bool> { return true; };
  seams.place_child = [&](const domain::OrderIntent& child) -> PlaceResult {
    place_log.push_back(child.client_ref);
    return ack_place(order_id_for(child.client_ref));
  };

  const SlicedLegResult result =
      execute_sliced_leg(over_freeze_parent(), instrument(), seams, alerts);

  CHECK(result.outcome == SlicedLegOutcome::FilledSliced);
  CHECK(place_log.empty());  // idempotent replay sends NOTHING
  CHECK(result.deduped_count == 3);
  CHECK(result.placed_count == 0);
  CHECK(alerts.count() == 0);
}

// ── AC-3: a child UNKNOWN pauses the whole leg ───────────────────────────────

TEST_CASE("AC-3 child Unknown at #2 => STOP (no #3), UnknownPaused, paused_at #2, Critical alert") {
  std::vector<std::string> place_log;
  SpyAlertSink alerts;

  SlicedLegSeams seams;
  seams.place_child = [&](const domain::OrderIntent& child) -> PlaceResult {
    place_log.push_back(child.client_ref);
    if (child.client_ref == ref(2)) {
      return unknown_place();
    }
    return ack_place(order_id_for(child.client_ref));
  };

  const SlicedLegResult result =
      execute_sliced_leg(over_freeze_parent(), instrument(), seams, alerts);

  CHECK(result.outcome == SlicedLegOutcome::UnknownPaused);
  CHECK(result.paused_at_ref == ref(2));
  // #1 placed, #2 attempted (came back Unknown), #3 NEVER reached.
  CHECK(count_of(place_log, ref(1)) == 1);
  CHECK(count_of(place_log, ref(2)) == 1);
  CHECK(count_of(place_log, ref(3)) == 0);
  CHECK(result.placed_count == 1);
  // ONE Critical alert, redaction-safe, and it NAMES THE CHILD (IMP-16): the ref
  // rides in the TYPED provenance context, not interpolated into the free-form
  // body (where a sink's scrub would destroy it).
  //
  // SCOPE OF WHAT THIS PROVES, STATED HONESTLY: SpyAlertSink does not scrub, so
  // these two lines prove the CALLER's half of the contract (the ref is handed
  // over as a typed column and is absent from the body) and nothing about the
  // renderer. The end-to-end half — that this exact `<minted>#2` child ref
  // survives a REAL scrubbing sink verbatim — is asserted in alerting_test.cpp
  // ("IMP-16: a minted CHILD slice ref survives a real sink verbatim").
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Critical);
  CHECK(alerts.last_provenance().client_ref == ref(2));
  CHECK(alerts.last_message().find(ref(2)) == std::string::npos);
}

TEST_CASE("AC-3 place Error at #2 => UnknownPaused (ambiguous mutating failure, no blind retry)") {
  std::vector<std::string> place_log;
  SpyAlertSink alerts;

  SlicedLegSeams seams;
  seams.place_child = [&](const domain::OrderIntent& child) -> PlaceResult {
    place_log.push_back(child.client_ref);
    if (child.client_ref == ref(2)) {
      return place_error();
    }
    return ack_place(order_id_for(child.client_ref));
  };

  const SlicedLegResult result =
      execute_sliced_leg(over_freeze_parent(), instrument(), seams, alerts);

  CHECK(result.outcome == SlicedLegOutcome::UnknownPaused);
  CHECK(result.paused_at_ref == ref(2));
  CHECK(count_of(place_log, ref(3)) == 0);  // STOP — no remaining child sent
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Critical);
}

TEST_CASE("AC-3 already_placed Error at #2 => UnknownPaused, place_child NEVER called for that child") {
  std::vector<std::string> place_log;
  SpyAlertSink alerts;

  SlicedLegSeams seams;
  seams.already_placed = [&](const std::string& client_ref) -> Result<bool> {
    if (client_ref == ref(2)) {
      return already_error();  // cannot prove it is safe to (re)send
    }
    return false;
  };
  seams.place_child = [&](const domain::OrderIntent& child) -> PlaceResult {
    place_log.push_back(child.client_ref);
    return ack_place(order_id_for(child.client_ref));
  };

  const SlicedLegResult result =
      execute_sliced_leg(over_freeze_parent(), instrument(), seams, alerts);

  CHECK(result.outcome == SlicedLegOutcome::UnknownPaused);
  CHECK(result.paused_at_ref == ref(2));
  // #1 placed; #2 paused BEFORE placement; #3 never reached.
  CHECK(count_of(place_log, ref(1)) == 1);
  CHECK(count_of(place_log, ref(2)) == 0);
  CHECK(count_of(place_log, ref(3)) == 0);
  CHECK(alerts.count() == 1);
  CHECK(alerts.last_level() == AlertLevel::Critical);
}

// ── Fail-closed: nothing is ever placed ──────────────────────────────────────

TEST_CASE("Fail-closed: slicer Validation Error (freeze < lot) => SliceRejected, place_child never called") {
  std::vector<std::string> place_log;
  SpyAlertSink alerts;

  SlicedLegSeams seams;
  seams.place_child = [&](const domain::OrderIntent& child) -> PlaceResult {
    place_log.push_back(child.client_ref);
    return ack_place(order_id_for(child.client_ref));
  };

  // freeze_qty (1) below one lot (2) => the slicer fails closed with Validation.
  const SlicedLegResult result =
      execute_sliced_leg(over_freeze_parent(), instrument(/*lot=*/2, /*freeze=*/1), seams, alerts);

  CHECK(result.outcome == SlicedLegOutcome::SliceRejected);
  CHECK(place_log.empty());
  CHECK(alerts.count() == 0);
}

TEST_CASE("Fail-closed: a '#'-child parent ref cannot be re-sliced => SliceRejected, nothing placed") {
  std::vector<std::string> place_log;
  SpyAlertSink alerts;

  SlicedLegSeams seams;
  seams.place_child = [&](const domain::OrderIntent& child) -> PlaceResult {
    place_log.push_back(child.client_ref);
    return ack_place(order_id_for(child.client_ref));
  };

  // A parent ref that already contains '#' is a child ref => slicer rejects it.
  const SlicedLegResult result =
      execute_sliced_leg(over_freeze_parent(kParent + "#1"), instrument(), seams, alerts);

  CHECK(result.outcome == SlicedLegOutcome::SliceRejected);
  CHECK(place_log.empty());
}

TEST_CASE("Fail-closed: null place_child seam => SliceRejected") {
  SpyAlertSink alerts;
  SlicedLegSeams seams;  // place_child is null

  const SlicedLegResult result =
      execute_sliced_leg(over_freeze_parent(), instrument(), seams, alerts);

  CHECK(result.outcome == SlicedLegOutcome::SliceRejected);
  CHECK(alerts.count() == 0);
}

// ── to_string: stable names ──────────────────────────────────────────────────

TEST_CASE("to_string: stable ChildPlacement + SlicedLegOutcome names") {
  CHECK(broker_exec::options::to_string(ChildPlacement::Acked) == "Acked");
  CHECK(broker_exec::options::to_string(ChildPlacement::AlreadyPlaced) == "AlreadyPlaced");
  CHECK(broker_exec::options::to_string(ChildPlacement::Unknown) == "Unknown");
  CHECK(broker_exec::options::to_string(SlicedLegOutcome::FilledSliced) == "FilledSliced");
  CHECK(broker_exec::options::to_string(SlicedLegOutcome::UnknownPaused) == "UnknownPaused");
  CHECK(broker_exec::options::to_string(SlicedLegOutcome::SliceRejected) == "SliceRejected");
}
