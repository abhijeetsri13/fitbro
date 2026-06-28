#include "broker_exec/fillnorm/fill_normalizer.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

#include "broker_exec/brokerreason/rejection_classifier.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/redaction.hpp"

namespace broker_exec::fillnorm {

namespace {

// ASCII-lowercase a copy so status matching is case-insensitive. We cast to
// `unsigned char` before std::tolower (passing a negative char is UB) and never
// touch locale/OS — the normalizer must behave identically on every platform.
[[nodiscard]] std::string to_lower_ascii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

// Clamp a possibly-negative broker quantity to a non-negative count. A garbage
// negative qty must never propagate as a real fill/pending — fail closed to 0.
[[nodiscard]] std::int64_t clamp_non_negative(std::int64_t q) noexcept {
  return q > 0 ? q : 0;
}

// Build the redaction-safe detail. By construction it embeds ONLY the canonical
// state name + the integer filled/pending counts — never any part of the raw
// broker status text — and is then run through domain::scrub as defense in depth,
// so even a future change that tried to echo raw text could not leak a
// token-shaped secret.
[[nodiscard]] std::string make_detail(domain::OrderState state, std::int64_t filled,
                                      std::int64_t pending) {
  std::string detail = "fill normalized to '";
  detail += std::string(domain::to_string(state));
  detail += "' (filled=";
  detail += std::to_string(filled);
  detail += ", pending=";
  detail += std::to_string(pending);
  detail += ")";
  return domain::scrub(detail);
}

}  // namespace

FillSnapshot normalize_fill(std::string_view raw_status, std::int64_t filled_qty,
                            std::int64_t total_qty, bool from_reconcile) {
  // ── Quantities first: clamp non-negative, derive pending without underflow ──
  // pending can NEVER go negative even when a broker reports filled > total.
  const std::int64_t filled = clamp_non_negative(filled_qty);
  const std::int64_t total = clamp_non_negative(total_qty);
  const std::int64_t pending = total > filled ? total - filled : 0;

  const std::string status = to_lower_ascii(raw_status);

  // ── Canonical state: DRIVE OFF QUANTITY FIRST, THEN STATUS (the core fix) ──
  // The event TYPE must never mask a real fill: a Kite "UPDATE" carrying filled>0
  // is a PartiallyFilled, not "not filled".
  domain::OrderState state;

  // Terminal-by-status takes precedence: a reject/cancel is terminal regardless of
  // quantity. A cancel can leave a partial fill behind — we carry whatever
  // filled/pending we were given; the filled_qty is what downstream sizing uses.
  if (contains(status, "reject")) {
    state = domain::OrderState::Rejected;
  } else if (contains(status, "cancel")) {
    state = domain::OrderState::Cancelled;
  } else if (filled > 0 && pending > 0) {
    // The PARTIAL-FILL fix: filled AND still-outstanding ⇒ PartiallyFilled, no
    // matter whether the broker labelled the event UPDATE / OPEN / COMPLETE.
    state = domain::OrderState::PartiallyFilled;
  } else if (filled > 0 && pending == 0) {
    // Fully executed: a positive fill with nothing outstanding. We do NOT require
    // total > 0 here — a broker that omitted/zeroed the order total (the exact
    // "the event may lie or be missing" case) must NOT cause a real fill to fall
    // through to a not-filled state. filled>0 IS a fill: drive off the quantity.
    state = domain::OrderState::Filled;
  } else if (filled == 0 && pending > 0) {
    // A live working order with no fill yet — covers OPEN / TRIGGER PENDING /
    // UPDATE-with-no-fill. Acknowledged, not Unknown: we DO know it is working.
    state = domain::OrderState::Acknowledged;
  } else {
    // No usable quantity signal at all (filled==0 && pending==0 && total==0). Lean
    // on brokerreason::classify_status — the broker-neutral unknown-status ⇒
    // reconcile fail-safe — rather than re-deriving the working-state vocabulary
    // here. A RECOGNIZED working status (classify_status returns Indeterminate
    // WITHOUT alerting — open / trigger pending / update) ⇒ Acknowledged (a live
    // order we simply have no counts for yet); anything else with no qty signal —
    // an UNRECOGNIZED status (Indeterminate + alert) OR a recognized terminal like
    // COMPLETE that carries no qty (classify_status ⇒ AlreadyComplete) ⇒ Unknown,
    // forcing a reconcile rather than guessing the order's fate.
    const brokerreason::Classification c = brokerreason::classify_status(raw_status);
    const bool recognized_working =
        c.reason == brokerreason::RejectReason::Indeterminate && !c.should_alert;
    state = recognized_working ? domain::OrderState::Acknowledged : domain::OrderState::Unknown;
  }

  // ── Trust: a push is NEVER authoritative; an exit may only be sized off a
  // reconciled fill. ──
  const bool authoritative = from_reconcile;
  const bool exit_qty_trustworthy = authoritative && filled > 0;

  FillSnapshot snap;
  snap.canonical_state = state;
  snap.filled_qty = filled;
  snap.pending_qty = pending;
  snap.authoritative = authoritative;
  snap.exit_qty_trustworthy = exit_qty_trustworthy;
  snap.detail = make_detail(state, filled, pending);
  return snap;
}

std::int64_t exit_qty_for(const FillSnapshot& snap) noexcept {
  // The ONLY safe basis for sizing an exit off a fill. A non-authoritative push
  // (exit_qty_trustworthy == false) yields 0: do NOT exit off a push — reconcile
  // first. A reconcile that observed no fill likewise yields 0.
  return snap.exit_qty_trustworthy ? snap.filled_qty : 0;
}

bool is_authoritative(const FillSnapshot& snap) noexcept { return snap.authoritative; }

}  // namespace broker_exec::fillnorm
