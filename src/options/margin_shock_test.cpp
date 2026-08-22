#include "broker_exec/options/margin_shock.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <optional>

#include "broker_exec/capabilities/capabilities.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/result.hpp"

using broker_exec::Result;
using broker_exec::options::evaluate_margin_shock;
using broker_exec::options::MarginShockInputs;
using broker_exec::options::MarginShockOutcome;
using broker_exec::options::MarginShockResult;
using broker_exec::options::MarginShockSeams;
using broker_exec::options::ShockMarginModel;

namespace domain = broker_exec::domain;
namespace errors = broker_exec::errors;
namespace capabilities = broker_exec::capabilities;

namespace {

using Support = capabilities::Support;
using ModelResult = Result<ShockMarginModel>;

// Audit spy: captures the call count + the LAST result handed to it (the same
// recording-seam shape the sibling sliced_leg tests use for the AlertSink). A
// pure value sink — the evaluator hands it the final result on every path.
struct AuditSpy {
  std::size_t count = 0;
  std::optional<MarginShockResult> last;

  // The seam closure to wire into MarginShockSeams::audit.
  [[nodiscard]] auto seam() {
    return [this](const MarginShockResult& result) {
      ++count;
      last = result;
    };
  }
};

// A SPAN source that returns a fixed model (now / under_shock in rupees).
[[nodiscard]] ModelResult model_source(domain::Money now, domain::Money under_shock) {
  return ShockMarginModel{now, under_shock};
}

// A SPAN source that errors — cannot model the shock (=> UNAVAILABLE, fail-closed).
[[nodiscard]] ModelResult source_error() {
  return broker_exec::fail(
      errors::make_error(errors::ErrorCategory::Timeout, "SPAN source timed out"));
}

[[nodiscard]] MarginShockInputs inputs(domain::Money available, bool net_short) {
  return MarginShockInputs{available, net_short};
}

}  // namespace

// ── AC-1: SPAN available, shocked margin within available => allowed ──────────

TEST_CASE(
    "AC-1 within: Supported, shock 400rs < available 500rs => WithinShockLimit, not blocked") {
  AuditSpy audit;
  MarginShockSeams seams;
  seams.span_margin_source = [] {
    return model_source(domain::Money::from_rupees(100), domain::Money::from_rupees(400));
  };
  seams.audit = audit.seam();

  const MarginShockResult result = evaluate_margin_shock(
      Support::Supported, inputs(domain::Money::from_rupees(500), /*net_short=*/true), seams);

  CHECK(result.outcome == MarginShockOutcome::WithinShockLimit);
  CHECK(result.blocked == false);
  CHECK(result.span_available == true);
  CHECK(result.margin_now == domain::Money::from_rupees(100));
  CHECK(result.margin_under_shock == domain::Money::from_rupees(400));
  CHECK(result.available_margin == domain::Money::from_rupees(500));
  // AC-3: audited exactly once with the matching outcome.
  CHECK(audit.count == 1);
  REQUIRE(audit.last.has_value());
  CHECK(audit.last->outcome == MarginShockOutcome::WithinShockLimit);
}

// ── AC-1: SPAN available, shocked margin crosses available => blocked ─────────

TEST_CASE(
    "AC-1 crossing: Supported, shock 600rs > available 500rs => BlockedMarginShock, blocked") {
  AuditSpy audit;
  MarginShockSeams seams;
  seams.span_margin_source = [] {
    return model_source(domain::Money::from_rupees(100), domain::Money::from_rupees(600));
  };
  seams.audit = audit.seam();

  const MarginShockResult result = evaluate_margin_shock(
      Support::Supported, inputs(domain::Money::from_rupees(500), /*net_short=*/true), seams);

  CHECK(result.outcome == MarginShockOutcome::BlockedMarginShock);
  CHECK(result.blocked == true);
  CHECK(result.span_available == true);
  CHECK(result.margin_under_shock == domain::Money::from_rupees(600));
  CHECK(audit.count == 1);
  REQUIRE(audit.last.has_value());
  CHECK(audit.last->outcome == MarginShockOutcome::BlockedMarginShock);
}

// Defense-in-depth: a malformed source reporting under_shock < now must NOT slip
// an already-over-the-line-NOW basket through (FR-18: don't open the force-
// liquidatable). now 600rs > available 500rs, shock 400rs < available => BLOCKED.
TEST_CASE("AC-1 defense-in-depth: margin NOW over available blocks even if shock is under") {
  AuditSpy audit;
  MarginShockSeams seams;
  seams.span_margin_source = [] {
    return model_source(domain::Money::from_rupees(600), domain::Money::from_rupees(400));
  };
  seams.audit = audit.seam();

  const MarginShockResult result = evaluate_margin_shock(
      Support::Supported, inputs(domain::Money::from_rupees(500), /*net_short=*/true), seams);

  CHECK(result.outcome == MarginShockOutcome::BlockedMarginShock);
  CHECK(result.blocked == true);
}

// ── AC-1 boundary: shock == available is within (documented strict `>`) ───────

TEST_CASE("AC-1 boundary: shock 500rs == available 500rs => WithinShockLimit (strict > crossing)") {
  AuditSpy audit;
  MarginShockSeams seams;
  seams.span_margin_source = [] {
    return model_source(domain::Money::from_rupees(200), domain::Money::from_rupees(500));
  };
  seams.audit = audit.seam();

  const MarginShockResult result = evaluate_margin_shock(
      Support::Supported, inputs(domain::Money::from_rupees(500), /*net_short=*/true), seams);

  CHECK(result.outcome == MarginShockOutcome::WithinShockLimit);
  CHECK(result.blocked == false);
  CHECK(result.span_available == true);
}

// ── AC-2: SPAN unavailable + net-short => fail closed (every unavailable trigger) ─

TEST_CASE(
    "AC-2 unavailable + net-short: Support::Unsupported => BlockedUnavailableNetShort, blocked") {
  AuditSpy audit;
  MarginShockSeams seams;
  // A source is wired in, but Unsupported gates SPAN off regardless: it must NOT
  // even be consulted. (If it were, this would model within-limit.)
  seams.span_margin_source = [] {
    return model_source(domain::Money::from_rupees(0), domain::Money::from_rupees(0));
  };
  seams.audit = audit.seam();

  const MarginShockResult result = evaluate_margin_shock(
      Support::Unsupported, inputs(domain::Money::from_rupees(500), /*net_short=*/true), seams);

  CHECK(result.outcome == MarginShockOutcome::BlockedUnavailableNetShort);
  CHECK(result.blocked == true);
  CHECK(result.span_available == false);
  // Money fields are zero on the unavailable path (documented not-modeled).
  CHECK(result.margin_now == domain::Money::from_paise(0));
  CHECK(result.margin_under_shock == domain::Money::from_paise(0));
  CHECK(result.available_margin == domain::Money::from_rupees(500));
  CHECK(audit.count == 1);
  CHECK(audit.last->outcome == MarginShockOutcome::BlockedUnavailableNetShort);
}

TEST_CASE(
    "AC-2 unavailable + net-short: Support::Unknown (fail-closed default) => "
    "BlockedUnavailableNetShort") {
  AuditSpy audit;
  MarginShockSeams seams;
  seams.span_margin_source = [] {
    return model_source(domain::Money::from_rupees(0), domain::Money::from_rupees(0));
  };
  seams.audit = audit.seam();

  const MarginShockResult result = evaluate_margin_shock(
      Support::Unknown, inputs(domain::Money::from_rupees(500), /*net_short=*/true), seams);

  CHECK(result.outcome == MarginShockOutcome::BlockedUnavailableNetShort);
  CHECK(result.blocked == true);
  CHECK(result.span_available == false);
  CHECK(audit.count == 1);
}

TEST_CASE(
    "AC-2 unavailable + net-short: Supported but source returns Error => fail-closed block (cannot "
    "model)") {
  AuditSpy audit;
  MarginShockSeams seams;
  seams.span_margin_source = [] { return source_error(); };
  seams.audit = audit.seam();

  const MarginShockResult result = evaluate_margin_shock(
      Support::Supported, inputs(domain::Money::from_rupees(500), /*net_short=*/true), seams);

  CHECK(result.outcome == MarginShockOutcome::BlockedUnavailableNetShort);
  CHECK(result.blocked == true);
  CHECK(result.span_available == false);
  CHECK(audit.count == 1);
  CHECK(audit.last->outcome == MarginShockOutcome::BlockedUnavailableNetShort);
}

TEST_CASE("AC-2 unavailable + net-short: null span_margin_source => fail-closed block") {
  AuditSpy audit;
  MarginShockSeams seams;  // span_margin_source is null
  seams.audit = audit.seam();

  const MarginShockResult result = evaluate_margin_shock(
      Support::Supported, inputs(domain::Money::from_rupees(500), /*net_short=*/true), seams);

  CHECK(result.outcome == MarginShockOutcome::BlockedUnavailableNetShort);
  CHECK(result.blocked == true);
  CHECK(result.span_available == false);
  CHECK(audit.count == 1);
}

// ── AC-2: SPAN unavailable + NOT net-short => allowed (bounded risk) ──────────

TEST_CASE(
    "AC-2 unavailable + not net-short: Unsupported, net_short=false => AllowedUnavailableBounded, "
    "not blocked") {
  AuditSpy audit;
  MarginShockSeams seams;  // null source => unavailable
  seams.audit = audit.seam();

  const MarginShockResult result = evaluate_margin_shock(
      Support::Unsupported, inputs(domain::Money::from_rupees(500), /*net_short=*/false), seams);

  CHECK(result.outcome == MarginShockOutcome::AllowedUnavailableBounded);
  CHECK(result.blocked == false);
  CHECK(result.span_available == false);
  CHECK(audit.count == 1);
  CHECK(audit.last->outcome == MarginShockOutcome::AllowedUnavailableBounded);
}

// ── AC-2: NEVER summed legs ───────────────────────────────────────────────────

TEST_CASE("AC-2 never summed-legs: unavailable net-short block consults NO per-leg margin sum") {
  // Intent: the ONLY available-margin model input is the SPAN source seam. There
  // is NO per-leg margin parameter or summing fallback in the API surface, so an
  // unavailable net-short basket can ONLY block — it can never be approximated.
  // The MarginShockSeams struct exposes exactly two seams: span_margin_source and
  // audit. The block below is reached with span_margin_source absent entirely.
  AuditSpy audit;
  MarginShockSeams seams;  // no margin source of ANY kind
  seams.audit = audit.seam();

  const MarginShockResult result = evaluate_margin_shock(
      Support::Unknown, inputs(domain::Money::from_rupees(1'000'000), /*net_short=*/true), seams);

  // Despite huge available margin, with no SPAN source a net-short basket blocks
  // (it is NOT compared against any summed-legs number — there is none).
  CHECK(result.outcome == MarginShockOutcome::BlockedUnavailableNetShort);
  CHECK(result.blocked == true);
}

// ── AC-3: audit invoked exactly once on EVERY outcome path ───────────────────

TEST_CASE("AC-3 audited: audit invoked exactly once with matching outcome on all four paths") {
  // WithinShockLimit.
  {
    AuditSpy audit;
    MarginShockSeams seams;
    seams.span_margin_source = [] {
      return model_source(domain::Money::from_rupees(100), domain::Money::from_rupees(400));
    };
    seams.audit = audit.seam();
    const MarginShockResult r = evaluate_margin_shock(
        Support::Supported, inputs(domain::Money::from_rupees(500), true), seams);
    CHECK(audit.count == 1);
    CHECK(audit.last->outcome == MarginShockOutcome::WithinShockLimit);
    CHECK(audit.last->outcome == r.outcome);
  }
  // BlockedMarginShock.
  {
    AuditSpy audit;
    MarginShockSeams seams;
    seams.span_margin_source = [] {
      return model_source(domain::Money::from_rupees(100), domain::Money::from_rupees(600));
    };
    seams.audit = audit.seam();
    const MarginShockResult r = evaluate_margin_shock(
        Support::Supported, inputs(domain::Money::from_rupees(500), true), seams);
    CHECK(audit.count == 1);
    CHECK(audit.last->outcome == MarginShockOutcome::BlockedMarginShock);
    CHECK(audit.last->outcome == r.outcome);
  }
  // BlockedUnavailableNetShort.
  {
    AuditSpy audit;
    MarginShockSeams seams;
    seams.audit = audit.seam();
    const MarginShockResult r = evaluate_margin_shock(
        Support::Unsupported, inputs(domain::Money::from_rupees(500), true), seams);
    CHECK(audit.count == 1);
    CHECK(audit.last->outcome == MarginShockOutcome::BlockedUnavailableNetShort);
    CHECK(audit.last->outcome == r.outcome);
  }
  // AllowedUnavailableBounded.
  {
    AuditSpy audit;
    MarginShockSeams seams;
    seams.audit = audit.seam();
    const MarginShockResult r = evaluate_margin_shock(
        Support::Unsupported, inputs(domain::Money::from_rupees(500), false), seams);
    CHECK(audit.count == 1);
    CHECK(audit.last->outcome == MarginShockOutcome::AllowedUnavailableBounded);
    CHECK(audit.last->outcome == r.outcome);
  }
}

TEST_CASE("AC-3 null audit seam does not crash on any path") {
  MarginShockSeams seams;  // audit is null
  // Available + within.
  seams.span_margin_source = [] {
    return model_source(domain::Money::from_rupees(100), domain::Money::from_rupees(400));
  };
  const MarginShockResult within = evaluate_margin_shock(
      Support::Supported, inputs(domain::Money::from_rupees(500), true), seams);
  CHECK(within.outcome == MarginShockOutcome::WithinShockLimit);

  // Available + crossing.
  seams.span_margin_source = [] {
    return model_source(domain::Money::from_rupees(100), domain::Money::from_rupees(600));
  };
  const MarginShockResult crossing = evaluate_margin_shock(
      Support::Supported, inputs(domain::Money::from_rupees(500), true), seams);
  CHECK(crossing.outcome == MarginShockOutcome::BlockedMarginShock);

  // Unavailable + net-short.
  seams.span_margin_source = nullptr;
  const MarginShockResult blocked =
      evaluate_margin_shock(Support::Unknown, inputs(domain::Money::from_rupees(500), true), seams);
  CHECK(blocked.outcome == MarginShockOutcome::BlockedUnavailableNetShort);

  // Unavailable + bounded.
  const MarginShockResult bounded = evaluate_margin_shock(
      Support::Unsupported, inputs(domain::Money::from_rupees(500), false), seams);
  CHECK(bounded.outcome == MarginShockOutcome::AllowedUnavailableBounded);
}

// ── to_string: stable names ───────────────────────────────────────────────────

TEST_CASE("to_string: stable MarginShockOutcome names") {
  CHECK(broker_exec::options::to_string(MarginShockOutcome::WithinShockLimit) ==
        "WithinShockLimit");
  CHECK(broker_exec::options::to_string(MarginShockOutcome::BlockedMarginShock) ==
        "BlockedMarginShock");
  CHECK(broker_exec::options::to_string(MarginShockOutcome::BlockedUnavailableNetShort) ==
        "BlockedUnavailableNetShort");
  CHECK(broker_exec::options::to_string(MarginShockOutcome::AllowedUnavailableBounded) ==
        "AllowedUnavailableBounded");
}
