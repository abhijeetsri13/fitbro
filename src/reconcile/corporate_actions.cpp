#include "broker_exec/reconcile/corporate_actions.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace broker_exec::reconcile {

namespace {

// A redaction-safe ratio tag for messages, e.g. "2:1". The integer ratio is not
// a secret.
[[nodiscard]] std::string ratio_tag(std::int64_t num, std::int64_t den) {
  return std::to_string(num) + ":" + std::to_string(den);
}

// True iff `value * num` divides exactly by `den` (no fractional-share result).
// `den == 0` is treated as "exact" because rebase() leaves a zero denominator as
// a no-op (it never divides), so there is no truncation to report.
[[nodiscard]] bool divides_exactly(std::int64_t value, std::int64_t num,
                                   std::int64_t den) noexcept {
  if (den == 0) {
    return true;
  }
  return (value * num) % den == 0;
}

}  // namespace

std::string_view to_string(CorporateActionKind kind) noexcept {
  switch (kind) {
    case CorporateActionKind::Split:
      return "Split";
    case CorporateActionKind::Bonus:
      return "Bonus";
    case CorporateActionKind::SymbolChange:
      return "SymbolChange";
    case CorporateActionKind::FnoAdjustment:
      return "FnoAdjustment";
  }
  return "Unknown";
}

domain::Position CorporateActionClassifier::rebase(const domain::Position& pos,
                                                   const CorporateAction& ca) {
  // A valid corporate action carries a strictly-positive ratio. Apply the
  // quantity multiplier qty' = qty * qty_num / qty_den AND its inverse on price
  // (price' = paise * qty_den / qty_num) TOGETHER so position value is preserved.
  // A malformed ratio (a zero/negative num or den) leaves BOTH factors untouched
  // — a no-op — rather than zeroing the quantity or dividing by zero. Integer
  // division truncates toward zero; a non-divisible result is NEVER a fractional
  // share, and the caller detects the truncation (see classify()).
  std::int64_t qty = pos.net_qty.value();
  std::int64_t paise = pos.avg_price.paise();
  if (ca.qty_num > 0 && ca.qty_den > 0) {
    qty = (qty * ca.qty_num) / ca.qty_den;
    paise = (paise * ca.qty_den) / ca.qty_num;
  }

  // Symbol: the new symbol on a symbol change, else unchanged. The new_token is
  // carried by the CA for the instrument master to re-resolve (Story 2.6); the
  // Position value type holds no token, so nothing to set here.
  const std::string& symbol = ca.new_symbol.empty() ? pos.symbol : ca.new_symbol;

  domain::Position rebased;
  rebased.symbol = symbol;
  rebased.net_qty = domain::Quantity::of(qty);
  rebased.avg_price = domain::Price::from_paise(paise);
  return rebased;
}

CorporateActionOutcome CorporateActionClassifier::classify(
    const domain::Position& believed, const domain::Position& broker_observed) const {
  CorporateActionOutcome outcome;

  // ── No change: believed and broker agree -> nothing to classify ──
  if (believed == broker_observed) {
    return outcome;  // {is_corporate_action=false, source_missing=false}
  }

  // ── A change exists. Source not configured -> SURFACE it (AC-2 fail-visible) ──
  // Never silently assume "no corporate action" and never silently treat the
  // change as a manual intervention: the operator must know the bot cannot reason
  // about the change.
  if (source_ == nullptr) {
    outcome.source_missing = true;
    outcome.detail =
        "corporate-action source not configured; cannot classify position change on " +
        believed.symbol;
    (void)alerts_.send(ports::AlertLevel::Error, outcome.detail);
    return outcome;
  }

  // ── A configured source: does a CA explain this exact change? ──
  const std::optional<CorporateAction> ca = source_->action_for(believed.symbol);
  if (ca.has_value()) {
    const domain::Position rebased = rebase(believed, *ca);

    // The CA only "wins" when re-basing the believed position EXACTLY reproduces
    // the broker-observed position: same symbol, same net_qty AND same avg_price
    // (compared in integer paise). A CA that does NOT match the observed change is
    // NOT force-applied — the caller falls through to the manual / mismatch path.
    const bool matches = rebased.symbol == broker_observed.symbol &&
                         rebased.net_qty == broker_observed.net_qty &&
                         rebased.avg_price.paise() == broker_observed.avg_price.paise();
    if (matches) {
      outcome.is_corporate_action = true;
      outcome.rebased = rebased;
      outcome.detail = std::string(to_string(ca->kind)) + " " +
                       ratio_tag(ca->qty_num, ca->qty_den) + " on " + believed.symbol;
      // A non-divisible re-base was integer-truncated (never a fractional share);
      // note it so the operator/audit sees the rounding.
      if (!divides_exactly(believed.net_qty.value(), ca->qty_num, ca->qty_den)) {
        outcome.detail += " (qty truncated: non-divisible re-base)";
      }
      (void)alerts_.send(ports::AlertLevel::Info, outcome.detail);
      return outcome;
    }
  }

  // ── A change with no CA, or a CA that does not explain it -> not a CA ──
  // The caller treats this as a manual intervention / mismatch. is_corporate_action
  // and source_missing both remain false.
  return outcome;
}

}  // namespace broker_exec::reconcile
