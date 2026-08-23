#include "broker_exec/marginsafety/margin_buffer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>

#include "broker_exec/domain/money.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/result.hpp"

using broker_exec::Result;
using broker_exec::marginsafety::evaluate_margin;
using broker_exec::marginsafety::MarginInputs;
using broker_exec::marginsafety::MarginSafetyConfig;
using broker_exec::marginsafety::MarginSafetyResult;
using broker_exec::marginsafety::MarginVerdict;
using broker_exec::marginsafety::require_margin_ok;
using broker_exec::marginsafety::to_string;

namespace domain = broker_exec::domain;
namespace errors = broker_exec::errors;
namespace marginsafety = broker_exec::marginsafety;

namespace {

// A config with an explicit bps cushion and zero flat cushion (the common case
// the assertions reason about). Default buffer_bps is 500 (5%).
[[nodiscard]] MarginSafetyConfig cfg_bps(int bps) {
  MarginSafetyConfig cfg;
  cfg.buffer_bps = bps;
  cfg.flat_buffer = domain::Money::from_paise(0);
  return cfg;
}

}  // namespace

// ── (a) single-leg buffer: the broker figure plus a fail-closed cushion ───────

TEST_CASE("single-leg 5% buffer: api 100rs => effective 105rs; 110rs sufficient, 104rs blocked") {
  const MarginSafetyConfig cfg = cfg_bps(500);  // 5%

  // available 110rs comfortably covers the 105rs buffered requirement.
  MarginInputs in;
  in.api_required = domain::Money::from_rupees(100);
  in.summed_leg_margin = domain::Money::from_rupees(100);
  in.available = domain::Money::from_rupees(110);
  in.is_multi_leg = false;
  in.benefit_trusted = true;

  const MarginSafetyResult ok = evaluate_margin(in, cfg);
  CHECK(ok.verdict == MarginVerdict::Sufficient);
  CHECK(ok.blocked == false);
  CHECK(ok.base_required == domain::Money::from_rupees(100));
  CHECK(ok.effective_required == domain::Money::from_rupees(105));
  CHECK(ok.used_worst_case == false);
  CHECK(ok.boundary_flagged == false);

  // available 104rs < the 105rs buffered requirement => BLOCKED (would have been
  // "fine" against the raw 100rs API figure — the buffer is the whole point).
  in.available = domain::Money::from_rupees(104);
  const MarginSafetyResult blocked = evaluate_margin(in, cfg);
  CHECK(blocked.verdict == MarginVerdict::InsufficientBlocked);
  CHECK(blocked.blocked == true);
}

// ── buffer rounds UP (fail-closed): the bps cushion is the CEIL, never the floor ─

TEST_CASE(
    "bps cushion rounds UP: api 1.01rs (101 paise) @ 500bps => effective 107 paise, not 106") {
  const MarginSafetyConfig cfg = cfg_bps(500);

  // 101 paise * 500 / 10000 = 5.05 paise. Floor would give 5 (effective 106);
  // fail-closed CEIL gives 6 (effective 107). Assert the exact paise.
  MarginInputs in;
  in.api_required = domain::Money::from_paise(101);
  in.summed_leg_margin = domain::Money::from_paise(101);
  in.available = domain::Money::from_paise(1'000'000);
  in.is_multi_leg = false;
  in.benefit_trusted = true;

  const MarginSafetyResult r = evaluate_margin(in, cfg);
  CHECK(r.base_required.paise() == 101);
  CHECK(r.effective_required.paise() == 107);  // 101 + ceil(5.05)=6
}

// ── (b) multi-leg, benefit TRUSTED: use the netted API figure ─────────────────

TEST_CASE("multi-leg trusted: base == api (netted figure), used_worst_case == false") {
  const MarginSafetyConfig cfg = cfg_bps(500);

  MarginInputs in;
  in.api_required = domain::Money::from_rupees(120);       // netted basket quote
  in.summed_leg_margin = domain::Money::from_rupees(200);  // summed (ignored here)
  in.available = domain::Money::from_rupees(1000);
  in.is_multi_leg = true;
  in.benefit_trusted = true;

  const MarginSafetyResult r = evaluate_margin(in, cfg);
  CHECK(r.base_required == domain::Money::from_rupees(120));  // the netted figure
  CHECK(r.used_worst_case == false);
  CHECK(r.boundary_flagged == false);
}

// ── (b)+(c) multi-leg, benefit UNtrusted (near 9:20/expiry): WORST CASE ────────

TEST_CASE("multi-leg untrusted: base == summed worst case, worst_case+boundary flagged, blocks") {
  const MarginSafetyConfig cfg = cfg_bps(500);

  // The account "looks" fine against the 120rs netted API figure (130rs available
  // covers it), but the leg benefit cannot be trusted near the boundary, so we
  // size against the 200rs summed worst case + 5% = 210rs => BLOCKED.
  MarginInputs in;
  in.api_required = domain::Money::from_rupees(120);
  in.summed_leg_margin = domain::Money::from_rupees(200);
  in.available = domain::Money::from_rupees(130);
  in.is_multi_leg = true;
  in.benefit_trusted = false;

  const MarginSafetyResult r = evaluate_margin(in, cfg);
  CHECK(r.base_required == domain::Money::from_rupees(200));       // worst case
  CHECK(r.effective_required == domain::Money::from_rupees(210));  // +5%
  CHECK(r.used_worst_case == true);
  CHECK(r.boundary_flagged == true);
  CHECK(r.verdict == MarginVerdict::InsufficientBlocked);
  CHECK(r.blocked == true);
}

// ── DEFENSIVE FLOOR: base never below the broker's reported api_required ───────

TEST_CASE("base never below api: untrusted multi-leg with summed < api still floors at api") {
  const MarginSafetyConfig cfg = cfg_bps(500);

  // Odd input: summed (50rs) is BELOW the api figure (120rs). The defensive floor
  // keeps base at api — the buffer must never enforce less than the broker states.
  MarginInputs in;
  in.api_required = domain::Money::from_rupees(120);
  in.summed_leg_margin = domain::Money::from_rupees(50);
  in.available = domain::Money::from_rupees(1000);
  in.is_multi_leg = true;
  in.benefit_trusted = false;

  const MarginSafetyResult r = evaluate_margin(in, cfg);
  CHECK(r.base_required >= in.api_required);
  CHECK(r.base_required == domain::Money::from_rupees(120));
}

// ── flat cushion stacks on top of the bps cushion ─────────────────────────────

TEST_CASE("flat cushion adds on top of bps cushion") {
  MarginSafetyConfig cfg;
  cfg.buffer_bps = 500;                              // +5%
  cfg.flat_buffer = domain::Money::from_rupees(10);  // +10rs flat

  MarginInputs in;
  in.api_required = domain::Money::from_rupees(100);
  in.summed_leg_margin = domain::Money::from_rupees(100);
  in.available = domain::Money::from_rupees(1000);
  in.is_multi_leg = false;
  in.benefit_trusted = true;

  const MarginSafetyResult r = evaluate_margin(in, cfg);
  // 100 + 5% (5rs) + 10rs flat = 115rs.
  CHECK(r.effective_required == domain::Money::from_rupees(115));
}

// ── require_margin_ok: gate adapter, redaction-safe ───────────────────────────

TEST_CASE(
    "require_margin_ok: Sufficient => ok(); InsufficientBlocked => RiskRejected/BlockStrategy") {
  const MarginSafetyConfig cfg = cfg_bps(500);

  MarginInputs in;
  in.api_required = domain::Money::from_rupees(100);
  in.summed_leg_margin = domain::Money::from_rupees(100);
  in.available = domain::Money::from_rupees(200);
  in.is_multi_leg = false;
  in.benefit_trusted = true;

  const MarginSafetyResult sufficient = evaluate_margin(in, cfg);
  const Result<broker_exec::ports::Ok> ok = require_margin_ok(sufficient);
  CHECK(ok.has_value());

  // Now starve the account so it blocks.
  in.available = domain::Money::from_rupees(10);
  const MarginSafetyResult blocked = evaluate_margin(in, cfg);
  REQUIRE(blocked.blocked == true);
  const Result<broker_exec::ports::Ok> err = require_margin_ok(blocked);
  REQUIRE_FALSE(err.has_value());
  CHECK(err.error().category == errors::ErrorCategory::RiskRejected);
  CHECK(err.error().action == errors::SuggestedAction::BlockStrategy);
  // Redaction-safe: a non-empty, stable message; no secret material.
  CHECK_FALSE(err.error().message.empty());
}

// ── overflow guard: a near-INT64_MAX margin never wraps below base ────────────

TEST_CASE("overflow guard: near-INT64_MAX api does not wrap effective below base") {
  const MarginSafetyConfig cfg = cfg_bps(500);

  MarginInputs in;
  in.api_required = domain::Money::from_paise(INT64_MAX);
  in.summed_leg_margin = domain::Money::from_paise(INT64_MAX);
  in.available = domain::Money::from_rupees(1);  // irrelevant; it will block
  in.is_multi_leg = false;
  in.benefit_trusted = true;

  const MarginSafetyResult r = evaluate_margin(in, cfg);
  // The bps product would overflow int64; the cushion saturates UP and the adds
  // saturate at INT64_MAX, so effective_required can never wrap to a SMALLER value.
  CHECK(r.effective_required >= r.base_required);
  CHECK(r.effective_required.paise() == INT64_MAX);
}

// ── fail-closed default: a default-constructed result BLOCKS ──────────────────

TEST_CASE("default-constructed MarginSafetyResult is fail-closed (InsufficientBlocked/blocked)") {
  const MarginSafetyResult def;
  CHECK(def.verdict == MarginVerdict::InsufficientBlocked);
  CHECK(def.blocked == true);
  // The gate adapter agrees: a default (forgotten) result is rejected, not approved.
  CHECK_FALSE(require_margin_ok(def).has_value());
}

// ── negative buffer_bps is clamped to zero (never shrinks the requirement) ────

TEST_CASE("negative buffer_bps clamps to 0: effective == base (no negative cushion)") {
  const MarginSafetyConfig cfg = cfg_bps(-100);

  MarginInputs in;
  in.api_required = domain::Money::from_rupees(100);
  in.summed_leg_margin = domain::Money::from_rupees(100);
  in.available = domain::Money::from_rupees(1000);
  in.is_multi_leg = false;
  in.benefit_trusted = true;

  const MarginSafetyResult r = evaluate_margin(in, cfg);
  CHECK(r.effective_required == r.base_required);
  CHECK(r.effective_required == domain::Money::from_rupees(100));
}

// ── to_string: stable names ───────────────────────────────────────────────────

TEST_CASE("to_string: stable MarginVerdict names") {
  CHECK(to_string(MarginVerdict::Sufficient) == "Sufficient");
  CHECK(to_string(MarginVerdict::InsufficientBlocked) == "InsufficientBlocked");
}

TEST_CASE("a NEGATIVE flat_buffer is clamped to 0 (never shrinks the requirement)") {
  MarginSafetyConfig cfg;
  cfg.buffer_bps = 0;                                 // isolate the flat cushion
  cfg.flat_buffer = domain::Money::from_paise(-100);  // a negative cushion must NOT shrink

  MarginInputs in;
  in.api_required = domain::Money::from_rupees(100);
  in.summed_leg_margin = domain::Money::from_rupees(100);
  in.available = domain::Money::from_rupees(100);

  const MarginSafetyResult r = evaluate_margin(in, cfg);
  // effective must not drop below base (== api): the negative flat is floored to 0.
  CHECK(r.effective_required == domain::Money::from_rupees(100));
  CHECK(r.effective_required >= r.base_required);
}

TEST_CASE("multi-leg with DEFAULTED benefit_trusted uses the worst case (fail-closed default)") {
  const MarginSafetyConfig cfg = cfg_bps(0);

  MarginInputs in;  // benefit_trusted is left DEFAULT (now false)
  in.is_multi_leg = true;
  in.api_required = domain::Money::from_rupees(120);       // optimistic netted quote
  in.summed_leg_margin = domain::Money::from_rupees(200);  // worst case, no leg benefit
  in.available = domain::Money::from_rupees(130);          // covers 120 but NOT 200

  const MarginSafetyResult r = evaluate_margin(in, cfg);
  CHECK(r.used_worst_case);  // defaulted to worst case
  CHECK(r.base_required == domain::Money::from_rupees(200));
  CHECK(r.blocked);  // 130 < 200 -> blocked
}
