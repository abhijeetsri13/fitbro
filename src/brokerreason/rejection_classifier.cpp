#include "broker_exec/brokerreason/rejection_classifier.hpp"

#include <array>
#include <cctype>
#include <string>
#include <string_view>

#include "broker_exec/domain/redaction.hpp"

namespace broker_exec::brokerreason {

namespace {

// ── Normalization ─────────────────────────────────────────────────────────────
// ASCII-lowercase a copy so the scan is case-insensitive. We deliberately use the
// `unsigned char` cast on std::tolower (passing a negative char is UB) and never
// touch locale/OS — the classifier must behave identically on every platform.
[[nodiscard]] std::string to_lower_ascii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    out.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

// Trim ASCII whitespace from both ends (used only for exact status matching).
[[nodiscard]] std::string_view trim(std::string_view s) {
  const auto is_ws = [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
  };
  while (!s.empty() && is_ws(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_ws(s.back())) s.remove_suffix(1);
  return s;
}

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

// ── The VERSIONED keyword → reason table (kClassifierVersion == 1) ────────────
// ORDER IS PRECEDENCE: the first reason whose ANY keyword is a substring of the
// lowercased message wins. The order is chosen so a more specific reason beats a
// more generic one when their keywords overlap. The most important overlap:
// "RMS:Margin" must classify as Margin (checked first), NOT the generic RmsBlock
// ("rms") which sits late. Keywords are stored lowercase; the message is
// lowercased once before the scan. AlreadyComplete and Indeterminate are handled
// out of band (two-keyword / many short-token rules) — see classify_rejection.
struct ReasonRule {
  RejectReason reason;
  RetryPosture posture;
  bool alert;
};

// canonical posture/alert per reason (single source of truth, also used by the
// out-of-band rules and by classify_status).
[[nodiscard]] ReasonRule rule_for(RejectReason reason) {
  switch (reason) {
    case RejectReason::Margin:
      return {RejectReason::Margin, RetryPosture::DoNotRetry, true};
    case RejectReason::CircuitLimit:
      return {RejectReason::CircuitLimit, RetryPosture::DoNotRetry, true};
    case RejectReason::FreezeQuantity:
      // DoNotRetry as-is: the caller must RE-ROUTE to the freeze-qty slicer
      // rather than resend the oversized order.
      return {RejectReason::FreezeQuantity, RetryPosture::DoNotRetry, true};
    case RejectReason::Illiquid:
      return {RejectReason::Illiquid, RetryPosture::DoNotRetry, true};
    case RejectReason::RmsBlock:
      return {RejectReason::RmsBlock, RetryPosture::DoNotRetry, true};
    case RejectReason::SessionExpired:
      return {RejectReason::SessionExpired, RetryPosture::ReconcileFirst, true};
    case RejectReason::RateLimited:
      // Expected/transient: a backoff-and-retry condition, NOT an alert (the
      // caller throttles; sustained throttling alerting is the caller's job).
      return {RejectReason::RateLimited, RetryPosture::SafeToRetryReadOnly, false};
    case RejectReason::OrderNotFound:
      return {RejectReason::OrderNotFound, RetryPosture::ReconcileFirst, true};
    case RejectReason::AlreadyComplete:
      // Terminal already: reconcile to fold the truth into the book; benign, no
      // alert.
      return {RejectReason::AlreadyComplete, RetryPosture::ReconcileFirst, false};
    case RejectReason::Indeterminate:
      // The DUPLICATE-ORDER HAZARD: the order may or may not have reached the
      // exchange. Reconcile first; NEVER blindly retry. Alert.
      return {RejectReason::Indeterminate, RetryPosture::ReconcileFirst, true};
    case RejectReason::Unknown:
      break;
  }
  // FAIL-CLOSED default (the core safety property): an unclassifiable reject is a
  // mutating action we must NOT blindly resend. DoNotRetry + alert. Never Safe.
  return {RejectReason::Unknown, RetryPosture::DoNotRetry, true};
}

// Build the redaction-safe detail. By construction it embeds ONLY canonical
// reason/posture words — never any part of the raw broker text — and is then run
// through domain::scrub as defense in depth (so even a future change that tried
// to echo raw text could not leak a token-shaped secret).
[[nodiscard]] std::string make_detail(RejectReason reason, RetryPosture posture) {
  std::string detail = "rejection classified as '";
  detail += std::string(to_string(reason));
  detail += "'; retry posture '";
  detail += std::string(to_string(posture));
  detail += "' (classifier v";
  detail += std::to_string(kClassifierVersion);
  detail += ")";
  return domain::scrub(detail);
}

[[nodiscard]] Classification make(RejectReason reason, RetryPosture posture, bool alert) {
  Classification c;
  c.reason = reason;
  c.posture = posture;
  c.should_alert = alert;
  c.canonical_detail = make_detail(reason, posture);
  return c;
}

[[nodiscard]] Classification make(ReasonRule r) { return make(r.reason, r.posture, r.alert); }

}  // namespace

std::string_view to_string(RejectReason reason) noexcept {
  switch (reason) {
    case RejectReason::Margin:
      return "margin";
    case RejectReason::CircuitLimit:
      return "circuit_limit";
    case RejectReason::FreezeQuantity:
      return "freeze_quantity";
    case RejectReason::Illiquid:
      return "illiquid";
    case RejectReason::RmsBlock:
      return "rms_block";
    case RejectReason::SessionExpired:
      return "session_expired";
    case RejectReason::RateLimited:
      return "rate_limited";
    case RejectReason::OrderNotFound:
      return "order_not_found";
    case RejectReason::AlreadyComplete:
      return "already_complete";
    case RejectReason::Indeterminate:
      return "indeterminate";
    case RejectReason::Unknown:
      return "unknown";
  }
  return "unknown";
}

std::string_view to_string(RetryPosture posture) noexcept {
  switch (posture) {
    case RetryPosture::DoNotRetry:
      return "do_not_retry";
    case RetryPosture::ReconcileFirst:
      return "reconcile_first";
    case RetryPosture::SafeToRetryReadOnly:
      return "safe_to_retry_read_only";
  }
  return "do_not_retry";
}

errors::SuggestedAction to_suggested_action(RetryPosture posture) noexcept {
  switch (posture) {
    case RetryPosture::DoNotRetry:
      return errors::SuggestedAction::DoNotRetry;
    case RetryPosture::ReconcileFirst:
      return errors::SuggestedAction::ReconcileFirst;
    case RetryPosture::SafeToRetryReadOnly:
      return errors::SuggestedAction::RetrySafe;
  }
  // Fail closed: an unmapped posture must not imply a safe retry.
  return errors::SuggestedAction::DoNotRetry;
}

Classification classify_rejection(std::string_view raw_message) {
  // FAIL-CLOSED on empty: no text ⇒ Unknown / DoNotRetry / alert. We NEVER assume
  // an empty reject is safe to retry.
  if (trim(raw_message).empty()) {
    return make(rule_for(RejectReason::Unknown));
  }

  const std::string m = to_lower_ascii(raw_message);

  // The ordered precedence scan. First reason with a matching keyword wins.
  // Margin is FIRST so "RMS:Margin" resolves to Margin, not the generic RmsBlock.
  if (contains(m, "margin") || contains(m, "insufficient funds") ||
      contains(m, "insufficient balance") || contains(m, "rms:margin")) {
    return make(rule_for(RejectReason::Margin));
  }
  if (contains(m, "circuit") || contains(m, "price out of") || contains(m, "lpp") ||
      contains(m, "dpr") || contains(m, "out of range") ||
      contains(m, "outside the daily price")) {
    return make(rule_for(RejectReason::CircuitLimit));
  }
  if (contains(m, "freeze") || contains(m, "quantity higher than maximum") ||
      contains(m, "max allowed qty") || contains(m, "maximum allowed") ||
      contains(m, "exceeds the maximum")) {
    return make(rule_for(RejectReason::FreezeQuantity));
  }
  if (contains(m, "no trades in this instrument") || contains(m, "illiquid") ||
      contains(m, "no liquidity") || contains(m, "no trades")) {
    return make(rule_for(RejectReason::Illiquid));
  }
  // Session/auth BEFORE the generic RmsBlock and BEFORE OrderNotFound's "invalid"
  // so "Token is invalid or has expired" resolves to SessionExpired.
  if (contains(m, "access_token") || contains(m, "api_key") || contains(m, "token") ||
      contains(m, "session") || contains(m, "unauthorized") || contains(m, "401")) {
    return make(rule_for(RejectReason::SessionExpired));
  }
  if (contains(m, "too many requests") || contains(m, "rate limit") ||
      contains(m, "rate-limit") || contains(m, "429")) {
    return make(rule_for(RejectReason::RateLimited));
  }
  // AlreadyComplete: the broker says the order is already terminal. Two-keyword
  // rule, checked before OrderNotFound so "already cancelled" is not read as a
  // not-found. Benign / reconcile.
  if (contains(m, "already") &&
      (contains(m, "complete") || contains(m, "executed") || contains(m, "cancelled") ||
       contains(m, "canceled") || contains(m, "filled") || contains(m, "traded"))) {
    return make(rule_for(RejectReason::AlreadyComplete));
  }
  if (contains(m, "order not found") || contains(m, "invalid order") ||
      contains(m, "unknown order") || contains(m, "order does not exist") ||
      contains(m, "no such order")) {
    return make(rule_for(RejectReason::OrderNotFound));
  }
  // Indeterminate: the DUPLICATE-ORDER HAZARD — timeouts and OMS/gateway errors
  // where we cannot tell whether the order reached the exchange. Checked before
  // the generic RmsBlock so an OMS-gateway 503 is reconciled, not mis-blocked.
  if (contains(m, "timeout") || contains(m, "timed out") ||
      contains(m, "no response from oms") || contains(m, "no response") ||
      contains(m, "kt-oms") || contains(m, "oms") || contains(m, "gateway timeout") ||
      contains(m, "502") || contains(m, "503") || contains(m, "504")) {
    return make(rule_for(RejectReason::Indeterminate));
  }
  // Generic RmsBlock LAST among the matchers: broad "rms"/"blocked" keywords, so
  // it only catches what the more specific reasons above did not.
  if (contains(m, "rms") || contains(m, "blocked") || contains(m, "block") ||
      contains(m, "not allowed to trade")) {
    return make(rule_for(RejectReason::RmsBlock));
  }

  // FAIL-CLOSED (THE KEY SAFETY DEFAULT): non-empty text we could not classify.
  // Treat it as a mutating action that must NOT be blindly resent. Unknown /
  // DoNotRetry / alert — NEVER safe-to-retry.
  return make(rule_for(RejectReason::Unknown));
}

Classification classify_status(std::string_view raw_status) {
  // Exact (normalized) match on a focused set of broker order statuses. The full
  // canonical OrderState mapping lives in the adapters; this is the broker-neutral
  // *unknown-status ⇒ reconcile* fail-safe plus a few unambiguous shortcuts.
  const std::string s = to_lower_ascii(trim(raw_status));

  // Terminal-done states: reconcile to fold the final truth into the book. Benign.
  if (s == "complete" || s == "completed") {
    return make(RejectReason::AlreadyComplete, RetryPosture::ReconcileFirst, false);
  }
  if (s == "cancelled" || s == "canceled") {
    return make(RejectReason::AlreadyComplete, RetryPosture::ReconcileFirst, false);
  }
  // Terminal REJECTED: the order did not execute, but the canonical *reason* lives
  // in the rejection TEXT (use classify_rejection on it). On the status alone we
  // fail closed: do not blindly resend, and alert.
  if (s == "rejected") {
    return make(RejectReason::Unknown, RetryPosture::DoNotRetry, true);
  }
  // Known in-flight/working states: the order is live at the broker. Reconcile to
  // learn its fate; NEVER resend. Normal, so no alert.
  if (s == "open" || s == "open pending" || s == "trigger pending" || s == "update" ||
      s == "modify pending" || s == "validation pending" || s == "put order req received" ||
      s == "amo req received" || s == "after market order req received") {
    return make(RejectReason::Indeterminate, RetryPosture::ReconcileFirst, false);
  }

  // FAIL-SAFE: any UNRECOGNIZED (or empty) status ⇒ Indeterminate + ReconcileFirst
  // + alert. Force a reconcile rather than guess an order's fate.
  return make(RejectReason::Indeterminate, RetryPosture::ReconcileFirst, true);
}

}  // namespace broker_exec::brokerreason
