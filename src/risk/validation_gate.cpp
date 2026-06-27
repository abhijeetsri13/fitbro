#include "broker_exec/risk/validation_gate.hpp"

#include <algorithm>
#include <cstdint>
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

// Every non-Market order carries a price that must be tick-aligned: Limit and
// StopLoss carry a limit price, and StopLossMarket carries a TRIGGER price (also
// exchange-tick-constrained). Only a plain Market order has no price to check.
// Missing StopLossMarket here would let a non-tick-aligned SL-M trigger reach
// the broker.
[[nodiscard]] bool has_price_to_tick_check(domain::OrderType type) {
  return type != domain::OrderType::Market;
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

  // ── 7. tick (limit / SL-with-price only; Market / SL-M skip) ─────────────
  if (has_price_to_tick_check(ctx.intent.order_type)) {
    const std::int64_t price = ctx.intent.price.paise();
    const std::int64_t tick = ctx.instrument.tick_size.paise();
    if (price <= 0) {
      return fail(gate_error(ErrorCategory::Validation, "tick",
                             "price " + std::to_string(price) + " paise must be positive"));
    }
    if (tick <= 0) {
      return fail(gate_error(ErrorCategory::Validation, "tick",
                             "instrument tick size " + std::to_string(tick) + " is invalid"));
    }
    if (price % tick != 0) {
      return fail(gate_error(ErrorCategory::Validation, "tick",
                             "price " + std::to_string(price) +
                                 " paise is not aligned to tick size " + std::to_string(tick) +
                                 " paise"));
    }
  }

  // ── 8. freeze (over-freeze => slice in slice-mode; reject otherwise) ─────
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

  // ── 9. time-window (entry-only; propagate the calendar's MarketClosed) ───
  if (is_entry && ctx.calendar != nullptr) {
    if (auto allowed = ctx.calendar->require_entry_allowed(); !allowed) {
      return fail(wrap_error("time-window", std::move(allowed.error())));
    }
  }

  // ── 10. funds (entry-only; fail-closed on stale DataStale) ───────────────
  if (is_entry && ctx.funds_check) {
    if (auto funds = ctx.funds_check(); !funds) {
      return fail(wrap_error("funds", std::move(funds.error())));
    }
  }

  // ── 11. risk (applies to exits too) ──────────────────────────────────────
  if (ctx.risk_check) {
    if (auto risk = ctx.risk_check(); !risk) {
      return fail(wrap_error("risk", std::move(risk.error())));
    }
  }

  // ── 12. hedge (applies to exits too) ─────────────────────────────────────
  if (ctx.hedge_check) {
    if (auto hedge = ctx.hedge_check(); !hedge) {
      return fail(wrap_error("hedge", std::move(hedge.error())));
    }
  }

  return needs_slicing ? GateOutcome::AllowWithSlicing : GateOutcome::Allow;
}

}  // namespace broker_exec::risk
