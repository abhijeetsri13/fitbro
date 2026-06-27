#pragma once

// broker_exec::reconcile — corporate-action awareness (Story 3.3, FR-5).
//
// WHAT THIS IS: the CLASSIFIER that decides whether an observed change between
// the bot's believed position and the broker-observed position is EXPLAINED by a
// corporate action (split / bonus / symbol-change / F&O adjustment) rather than a
// genuine mismatch (3.1) or a manual intervention (3.2). The main loop consults
// THIS first: a change explained by a CA is re-based + token re-resolved, NOT
// flagged — so a split that doubles the quantity and halves the price never pages
// the operator and never triggers a corrective order.
//
// THE AC-2 CRUX — fail-visible absence: a NULL/absent corporate-action source
// PLUS an unexplained change is SURFACED (an Error alert + `source_missing`),
// never silently assumed to be "no corporate action" and never silently treated
// as a manual intervention. The operator must know the bot cannot reason about
// the change.
//
// NOT FORCE-APPLIED: a CA that exists for the symbol but does NOT explain the
// observed change (e.g. the re-based quantity/symbol does not match what the
// broker now reports) is NOT applied — the caller falls through to the manual /
// mismatch path. A CA only "wins" when it exactly explains the observed state.
//
// VALUE-PRESERVING RE-BASE, INTEGER MATH, NO FLOAT: post-action quantity is
// `qty * qty_num / qty_den`; the price multiplier is the INVERSE
// (`paise * qty_den / qty_num`) so the position VALUE (qty * price) is preserved.
// A 1:2 split is qty_num 2 / qty_den 1 (qty doubles, price halves); a 1:1 bonus is
// likewise qty_num 2 / qty_den 1. All arithmetic is integer paise / integer
// quantity — never floating point. A non-divisible re-base (a fractional-share
// result) is integer-truncated, NEVER turned into a fractional share, and the
// truncation is noted in the outcome detail (see corporate_actions.cpp).
//
// TOKEN RE-RESOLUTION is the instrument-master refresh's job (Story 2.6): the CA
// merely CARRIES `new_symbol` / `new_token`; the classifier re-bases the symbol
// and the master consumes the token. The Position value type holds no token.
//
// CROSS-PLATFORM: C++20 standard library only. No OS APIs, no `#ifdef`, no
// floating point. No throw: a failing AlertSink send is swallowed via `(void)`.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "broker_exec/domain/types.hpp"
#include "broker_exec/ports/alert_sink.hpp"

namespace broker_exec::reconcile {

// The kinds of corporate action this classifier understands. Split/Bonus adjust
// only quantity + price (via the num/den ratio); SymbolChange/FnoAdjustment
// additionally carry a new symbol/token the instrument master re-resolves.
enum class CorporateActionKind { Split, Bonus, SymbolChange, FnoAdjustment };

// Stable, log/serialization-friendly name for a CorporateActionKind.
[[nodiscard]] std::string_view to_string(CorporateActionKind kind) noexcept;

// A corporate action for a symbol, as supplied by a CorporateActionSource. A
// plain value type. Semantics: post-action quantity = `net_qty * qty_num /
// qty_den`; the price multiplier is the INVERSE (`paise * qty_den / qty_num`) so
// the position value is preserved. (Split 1:2 -> qty_num 2, qty_den 1: qty
// doubles, price halves. Bonus 1:1 -> qty_num 2, qty_den 1.) SymbolChange /
// FnoAdjustment additionally carry `new_symbol` / `new_token` (the token is for
// the instrument master to re-resolve; the Position type holds no token).
struct CorporateAction {
  std::string symbol;
  CorporateActionKind kind = CorporateActionKind::Split;
  std::int64_t qty_num = 1;     // Numerator of the quantity multiplier.
  std::int64_t qty_den = 1;     // Denominator of the quantity multiplier (!= 0).
  std::string new_symbol;       // Non-empty for a symbol change.
  std::int64_t new_token = 0;   // The re-resolved instrument token (master's job).
};

// Abstract port: the source of corporate-action reference data for a symbol. The
// "as-of" date is the source's own concern; a concrete refdata-backed source
// lands in a later story. Returns std::nullopt when the symbol has no known
// corporate action.
class CorporateActionSource {
 public:
  virtual ~CorporateActionSource() = default;

  [[nodiscard]] virtual std::optional<CorporateAction> action_for(
      std::string_view symbol) const = 0;
};

// The result of classifying an observed believed-vs-broker change. A plain value.
// `is_corporate_action` true means the change is fully explained by a CA and
// `rebased` is the re-based position the caller should adopt (no alert/order).
// `source_missing` true means the source was not configured AND a change existed
// — the operator was alerted (Error) and the caller must NOT silently proceed.
struct CorporateActionOutcome {
  bool is_corporate_action = false;
  bool source_missing = false;
  domain::Position rebased;  // Valid only when is_corporate_action == true.
  std::string detail;        // Redaction-safe context (kind + ratio / reason).
};

// The classifier. Holds ONLY the source port (nullptr == NOT configured) and the
// AlertSink. Pure classification: it never mutates state, never places orders.
class CorporateActionClassifier {
 public:
  // `source == nullptr` means the corporate-action source is NOT configured: a
  // change is then surfaced (Error alert) rather than silently assumed.
  CorporateActionClassifier(const CorporateActionSource* source,
                            ports::AlertSink& alerts) noexcept
      : source_(source), alerts_(alerts) {}

  // Re-base a position by a corporate action (value-preserving, integer math):
  //   qty'    = pos.net_qty.value() * ca.qty_num / ca.qty_den   (qty_den guarded)
  //   price'  = pos.avg_price.paise() * ca.qty_den / ca.qty_num (qty_num guarded)
  //   symbol' = ca.new_symbol if non-empty, else pos.symbol
  // A non-divisible quantity result is integer-truncated (toward zero) — it never
  // becomes a fractional share; the truncation is detectable by the caller (see
  // classify()'s detail). A zero qty_den / qty_num guard leaves that factor at 1
  // (no-op) so the function is always total and never divides by zero. new_token
  // re-resolution is the instrument master's job; the CA carries it.
  [[nodiscard]] static domain::Position rebase(const domain::Position& pos,
                                               const CorporateAction& ca);

  // Classify the change between `believed` and `broker_observed`:
  //  - AGREE (believed == broker_observed) -> no-op {is_corporate_action=false,
  //    source_missing=false}.
  //  - a change AND source not configured -> Error alert + {source_missing=true}
  //    (AC-2 fail-visible). NOT silently a CA, NOT silently a manual change.
  //  - a change AND a CA that, re-based, EXACTLY explains the observed position
  //    (symbol + net_qty + avg_price all match) -> {is_corporate_action=true,
  //    rebased, detail naming the kind+ratio} (an Info alert notes it).
  //  - a change with no CA, or a CA that does NOT match the observed change ->
  //    {is_corporate_action=false, source_missing=false} (caller falls through to
  //    the manual / mismatch path; the CA is NOT force-applied).
  [[nodiscard]] CorporateActionOutcome classify(
      const domain::Position& believed, const domain::Position& broker_observed) const;

 private:
  const CorporateActionSource* source_;  // nullptr == not configured.
  ports::AlertSink& alerts_;
};

}  // namespace broker_exec::reconcile
