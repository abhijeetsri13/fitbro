#include "broker_exec/risk/validation_gate.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "broker_exec/errors/error.hpp"

namespace broker_exec::risk {

namespace {

using errors::Error;
using errors::ErrorCategory;
using errors::make_error;

// Every gate Error message NAMES the failing check (AC-1) with a stable prefix.
[[nodiscard]] std::string named(const char* check, const std::string& detail) {
  return std::string("gate: ") + check + " check failed: " + detail;
}

// Build a fresh, gate-owned Error that names the check.
[[nodiscard]] Error gate_error(ErrorCategory category, const char* check, const std::string& detail) {
  return make_error(category, named(check, detail));
}

// Wrap a propagated inner Error (from the calendar / an injected predicate) so
// the message names the check, while PRESERVING its category, action and
// broker_code (e.g. a stale-funds DataStale, a window MarketClosed, a risk
// RiskRejected stay intact for the runtime's deterministic switch).
[[nodiscard]] Error wrap_error(const char* check, Error inner) {
  Error wrapped = std::move(inner);
  wrapped.message = named(check, wrapped.message);
  return wrapped;
}

// True iff `product` is present in `allowed` (allow-list membership).
[[nodiscard]] bool contains_product(const std::vector<domain::Product>& allowed,
                                    domain::Product product) {
  return std::find(allowed.begin(), allowed.end(), product) != allowed.end();
}

// True iff `exchange` is present in `allowed` (allow-list membership).
[[nodiscard]] bool contains_exchange(const std::vector<std::string>& allowed,
                                     const std::string& exchange) {
  return std::find(allowed.begin(), allowed.end(), exchange) != allowed.end();
}

// ── The (order_type x price fields) SHAPE MATRIX ────────────────────────────
//
// Since OrderIntent carries a DISTINCT `trigger_price` (IMP-11), each order type
// has an exact required shape. These two predicates are the single statement of
// it and everything below (the shape check AND the tick check) reads them, so the
// two can never disagree about which numbers a given type actually uses:
//
//   type            limit price          trigger price
//   ────────────    ─────────────────    ───────────────────
//   Market          ignored              FORBIDDEN
//   Limit           REQUIRED             FORBIDDEN
//   StopLoss (SL)   REQUIRED             REQUIRED
//   SL-M            ignored              REQUIRED
//
// WHY "ignored" AND NOT "forbidden" FOR THE MARKET-STYLE LIMIT: `price` is a
// non-optional field that ALWAYS holds a value, so a Market/SL-M intent built
// from a position snapshot, a store row, or a broker read inevitably carries some
// leftover number in it. Rejecting that would refuse a protective SL-M exit for a
// field the adapters do not even transmit — a fail-closed check that costs
// safety instead of buying it. The TRIGGER is different: it is an optional whose
// absence is expressible, so a trigger on a Limit/Market order is an unambiguous
// caller error (the adapters would silently drop it) and IS refused.

// True iff `type` works at a limit price that must be present and tick-aligned.
[[nodiscard]] bool uses_limit_price(domain::OrderType type) noexcept {
  return type == domain::OrderType::Limit || type == domain::OrderType::StopLoss;
}

// True iff `type` is armed by a trigger price that must be present and
// tick-aligned. (This is the check that used to run on the SYNTHESIZED value: the
// 2-8 gate tick-checked SL-M's `price` because the domain had nowhere else to put
// a trigger. It now reads the real field.)
[[nodiscard]] bool uses_trigger_price(domain::OrderType type) noexcept {
  return type == domain::OrderType::StopLoss || type == domain::OrderType::StopLossMarket;
}

// Tick-alignment of ONE price, as a named-check Error or nothing. `label` names
// the price in the message ("price" / "trigger price") so a failure says WHICH of
// an SL's two numbers is misaligned.
[[nodiscard]] std::optional<Error> tick_violation(std::int64_t price, std::int64_t tick,
                                                  const char* label) {
  if (price <= 0) {
    return gate_error(ErrorCategory::Validation, "tick",
                      std::string(label) + " " + std::to_string(price) +
                          " paise must be positive");
  }
  if (tick <= 0) {
    return gate_error(ErrorCategory::Validation, "tick",
                      "instrument tick size " + std::to_string(tick) + " is invalid");
  }
  if (price % tick != 0) {
    return gate_error(ErrorCategory::Validation, "tick",
                      std::string(label) + " " + std::to_string(price) +
                          " paise is not aligned to tick size " + std::to_string(tick) +
                          " paise");
  }
  return std::nullopt;
}

}  // namespace

Result<GateOutcome> ValidationGate::validate(const GateContext& ctx) const {
  const bool is_entry = !ctx.is_risk_reducing;

  // ── 1. kill-switch (entry-only) ──────────────────────────────────────────
  if (is_entry && ctx.kill_entry_block) {
    Error e = gate_error(ErrorCategory::RiskRejected, "kill-switch",
                         "entries are blocked by an active kill-switch");
    e.action = errors::SuggestedAction::BlockStrategy;  // not "reconcile"
    return fail(std::move(e));
  }

  // ── 2. UNKNOWN-pause (entry-only) ────────────────────────────────────────
  if (is_entry && ctx.unknown_pause_active) {
    Error e = gate_error(ErrorCategory::RiskRejected, "UNKNOWN-pause",
                         "entries are paused while UNKNOWN orders are unresolved");
    e.action = errors::SuggestedAction::BlockStrategy;  // not "reconcile"
    return fail(std::move(e));
  }

  // ── 3. duplicate (entry-only) ────────────────────────────────────────────
  if (is_entry && ctx.is_duplicate && ctx.is_duplicate()) {
    return fail(gate_error(ErrorCategory::DuplicateOrder, "duplicate",
                           "client_ref '" + ctx.intent.client_ref + "' has already been seen"));
  }

  // ── 4. exchange (applies to exits too) ───────────────────────────────────
  if (ctx.instrument.exchange.empty()) {
    return fail(gate_error(ErrorCategory::Validation, "exchange",
                           "instrument has no exchange"));
  }
  if (!ctx.allowed_exchanges.empty() &&
      !contains_exchange(ctx.allowed_exchanges, ctx.instrument.exchange)) {
    return fail(gate_error(ErrorCategory::Validation, "exchange",
                           "exchange '" + ctx.instrument.exchange + "' is not allowed"));
  }

  // ── 5. product (applies to exits too) ────────────────────────────────────
  if (!ctx.allowed_products.empty() &&
      !contains_product(ctx.allowed_products, ctx.intent.product)) {
    return fail(gate_error(ErrorCategory::Validation, "product",
                           std::string("product '") +
                               std::string(domain::to_string(ctx.intent.product)) +
                               "' is not allowed for this instrument"));
  }

  // ── 6. lot (applies to exits too) ────────────────────────────────────────
  const std::int64_t qty = ctx.intent.quantity.value();
  const std::int64_t lot = ctx.instrument.lot_size.value();
  if (qty <= 0) {
    return fail(gate_error(ErrorCategory::Validation, "lot",
                           "quantity " + std::to_string(qty) + " must be positive"));
  }
  if (lot <= 0) {
    return fail(gate_error(ErrorCategory::Validation, "lot",
                           "instrument lot size " + std::to_string(lot) + " is invalid"));
  }
  if (qty < lot || qty % lot != 0) {
    return fail(gate_error(ErrorCategory::Validation, "lot",
                           "quantity " + std::to_string(qty) +
                               " is not a multiple of lot size " + std::to_string(lot)));
  }

  // ── 7. order-shape (applies to exits too — see below) ────────────────────
  // The (order_type x price fields) matrix, FAIL-CLOSED. A risk-reducing exit is
  // exempt from the ENTRY-ONLY blocks above, never from SHAPE validation: a
  // malformed protective order is not protection, it is a broker rejection at the
  // worst possible moment. So this check runs for exits exactly as for entries.
  const bool needs_trigger = uses_trigger_price(ctx.intent.order_type);
  const bool has_trigger = ctx.intent.trigger_price.has_value();
  if (needs_trigger && !has_trigger) {
    return fail(gate_error(ErrorCategory::Validation, "order-shape",
                           std::string("order type '") +
                               std::string(domain::to_string(ctx.intent.order_type)) +
                               "' requires a trigger price"));
  }
  if (!needs_trigger && has_trigger) {
    // Refused rather than dropped: the adapters do not transmit a trigger for a
    // Limit/Market order, so accepting one would silently place an UNPROTECTED
    // order for a caller who believes a stop is armed.
    return fail(gate_error(ErrorCategory::Validation, "order-shape",
                           std::string("order type '") +
                               std::string(domain::to_string(ctx.intent.order_type)) +
                               "' must not carry a trigger price"));
  }
  // SIDE-RELATIVE ORDERING of an SL's two prices. A stop-loss LIMIT only works if
  // its limit sits on the far side of its trigger:
  //
  //   SELL SL  arms as the market FALLS through the trigger, then works down to
  //            the limit  =>  price <= trigger.
  //   BUY  SL  arms as the market RISES through the trigger, then works up to the
  //            limit      =>  price >= trigger.
  //
  // A SWAPPED PAIR IS A SHAPE BUG, NOT A PRICING CHOICE. A sell stop whose limit
  // sits ABOVE its trigger arms only once the market has already fallen past a
  // level it then refuses to sell at, so it sits unfillable through the entire
  // move it existed to escape — the caller believes they are protected and they
  // are not. The broker accepts it happily, which is exactly why it has to be
  // caught here. EQUAL IS ALLOWED: price == trigger is the common "stop at the
  // touch" order.
  //
  // Enforced for EXITS TOO. This is shape validation, and an exit's malformed
  // stop is the dangerous one — it is the leg standing between a live position
  // and an unbounded loss.
  if (ctx.intent.order_type == domain::OrderType::StopLoss) {
    const domain::Price limit = ctx.intent.price;
    const domain::Price trigger = *ctx.intent.trigger_price;  // presence proven above
    const bool ordered = ctx.intent.side == domain::Side::Sell ? limit <= trigger : limit >= trigger;
    if (!ordered) {
      return fail(gate_error(
          ErrorCategory::Validation, "order-shape",
          std::string("a ") + std::string(domain::to_string(ctx.intent.side)) +
              " stop-loss limit price " + std::to_string(limit.paise()) +
              " paise is on the wrong side of its trigger " + std::to_string(trigger.paise()) +
              " paise (SELL requires limit <= trigger, BUY requires limit >= trigger); "
              "the order would arm and then never fill"));
    }
  }

  // ── 8. tick (every price the type actually USES; see the shape matrix) ───
  // Limit -> the limit. SL -> BOTH the limit and the trigger. SL-M -> the trigger
  // only (its limit is ignored, so tick-checking it would reject a well-formed
  // order). Market -> nothing.
  {
    const std::int64_t tick = ctx.instrument.tick_size.paise();
    if (uses_limit_price(ctx.intent.order_type)) {
      if (auto bad = tick_violation(ctx.intent.price.paise(), tick, "price")) {
        return fail(std::move(*bad));
      }
    }
    if (needs_trigger) {
      // has_trigger is guaranteed by the shape check above.
      if (auto bad = tick_violation(ctx.intent.trigger_price->paise(), tick, "trigger price")) {
        return fail(std::move(*bad));
      }
    }
  }

  // ── 9. freeze (over-freeze => slice in slice-mode; reject otherwise) ─────
  // In slice-mode an over-freeze order is NOT an error: it becomes
  // AllowWithSlicing. We REMEMBER that decision and keep running the remaining
  // checks (time-window / funds / risk / hedge) so a later failure still wins.
  bool needs_slicing = false;
  const std::int64_t freeze = ctx.instrument.freeze_qty.value();
  if (freeze > 0 && qty > freeze) {
    if (ctx.slice_mode) {
      needs_slicing = true;
    } else {
      return fail(gate_error(ErrorCategory::Validation, "freeze",
                             "quantity " + std::to_string(qty) +
                                 " exceeds the freeze ceiling " + std::to_string(freeze) +
                                 " and slicing is disabled"));
    }
  }

  // ── 10. time-window (entry-only; propagate the calendar's MarketClosed) ──
  if (is_entry && ctx.calendar != nullptr) {
    if (auto allowed = ctx.calendar->require_entry_allowed(); !allowed) {
      return fail(wrap_error("time-window", std::move(allowed.error())));
    }
  }

  // ── 11. funds (entry-only; fail-closed on stale DataStale) ───────────────
  if (is_entry && ctx.funds_check) {
    if (auto funds = ctx.funds_check(); !funds) {
      return fail(wrap_error("funds", std::move(funds.error())));
    }
  }

  // ── 12. risk (applies to exits too) ──────────────────────────────────────
  if (ctx.risk_check) {
    if (auto risk = ctx.risk_check(); !risk) {
      return fail(wrap_error("risk", std::move(risk.error())));
    }
  }

  // ── 13. hedge (applies to exits too) ─────────────────────────────────────
  if (ctx.hedge_check) {
    if (auto hedge = ctx.hedge_check(); !hedge) {
      return fail(wrap_error("hedge", std::move(hedge.error())));
    }
  }

  return needs_slicing ? GateOutcome::AllowWithSlicing : GateOutcome::Allow;
}

}  // namespace broker_exec::risk
