#include "broker_exec/errors/error.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace broker_exec::errors {

namespace {

// Case-insensitive substring search over ASCII. We only ever match against
// lowercase broker keywords, so we fold the haystack, not the needle.
[[nodiscard]] bool contains_ci(std::string_view haystack, std::string_view needle) noexcept {
  if (needle.empty()) {
    return true;
  }
  if (needle.size() > haystack.size()) {
    return false;
  }
  const auto fold = [](char c) noexcept -> char {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  };
  const std::size_t last = haystack.size() - needle.size();
  for (std::size_t i = 0; i <= last; ++i) {
    bool match = true;
    for (std::size_t j = 0; j < needle.size(); ++j) {
      if (fold(haystack[i + j]) != needle[j]) {
        match = false;
        break;
      }
    }
    if (match) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::string_view to_string(ErrorCategory category) noexcept {
  switch (category) {
    case ErrorCategory::Transient:
      return "Transient";
    case ErrorCategory::RateLimited:
      return "RateLimited";
    case ErrorCategory::Network:
      return "Network";
    case ErrorCategory::Timeout:
      return "Timeout";
    case ErrorCategory::Auth:
      return "Auth";
    case ErrorCategory::SessionExpired:
      return "SessionExpired";
    case ErrorCategory::Validation:
      return "Validation";
    case ErrorCategory::InsufficientFunds:
      return "InsufficientFunds";
    case ErrorCategory::RiskRejected:
      return "RiskRejected";
    case ErrorCategory::NotSupported:
      return "NotSupported";
    case ErrorCategory::DuplicateOrder:
      return "DuplicateOrder";
    case ErrorCategory::OrderNotFound:
      return "OrderNotFound";
    case ErrorCategory::BrokerRejected:
      return "BrokerRejected";
    case ErrorCategory::MarketClosed:
      return "MarketClosed";
    case ErrorCategory::DataStale:
      return "DataStale";
    case ErrorCategory::Internal:
      return "Internal";
    case ErrorCategory::Unknown:
      return "Unknown";
  }
  return "Unknown";
}

std::string_view to_string(SuggestedAction action) noexcept {
  switch (action) {
    case SuggestedAction::RetrySafe:
      return "RetrySafe";
    case SuggestedAction::DoNotRetry:
      return "DoNotRetry";
    case SuggestedAction::ReconcileFirst:
      return "ReconcileFirst";
    case SuggestedAction::BlockStrategy:
      return "BlockStrategy";
    case SuggestedAction::ReEstablishSession:
      return "ReEstablishSession";
    case SuggestedAction::Cancel:
      return "Cancel";
    case SuggestedAction::SquareOff:
      return "SquareOff";
    case SuggestedAction::RaiseAlert:
      return "RaiseAlert";
  }
  return "RaiseAlert";
}

SuggestedAction default_action_for(ErrorCategory category) noexcept {
  switch (category) {
    case ErrorCategory::Transient:
      return SuggestedAction::RetrySafe;
    case ErrorCategory::RateLimited:
      // Slow down / queue, preserving the reserved exit lane (CAP-12). A safe
      // re-attempt under backoff is the right baseline; dangerous ops get
      // ReconcileFirst from the timeout path, not from here.
      return SuggestedAction::RetrySafe;
    case ErrorCategory::Network:
      return SuggestedAction::ReconcileFirst;
    case ErrorCategory::Timeout:
      // Baseline for an unqualified timeout is the safe-for-dangerous-ops
      // choice; classify_http() upgrades a read-timeout to RetrySafe.
      return SuggestedAction::ReconcileFirst;
    case ErrorCategory::Auth:
      return SuggestedAction::ReEstablishSession;
    case ErrorCategory::SessionExpired:
      return SuggestedAction::ReEstablishSession;
    case ErrorCategory::Validation:
      return SuggestedAction::DoNotRetry;
    case ErrorCategory::InsufficientFunds:
      return SuggestedAction::BlockStrategy;
    case ErrorCategory::RiskRejected:
      return SuggestedAction::ReconcileFirst;
    case ErrorCategory::NotSupported:
      return SuggestedAction::DoNotRetry;
    case ErrorCategory::DuplicateOrder:
      return SuggestedAction::ReconcileFirst;
    case ErrorCategory::OrderNotFound:
      return SuggestedAction::ReconcileFirst;
    case ErrorCategory::BrokerRejected:
      return SuggestedAction::ReconcileFirst;
    case ErrorCategory::MarketClosed:
      return SuggestedAction::BlockStrategy;
    case ErrorCategory::DataStale:
      return SuggestedAction::BlockStrategy;
    case ErrorCategory::Internal:
      return SuggestedAction::RaiseAlert;
    case ErrorCategory::Unknown:
      return SuggestedAction::ReconcileFirst;
  }
  return SuggestedAction::RaiseAlert;
}

Error make_error(ErrorCategory category, std::string message, std::string broker_code) {
  return Error{category, default_action_for(category), std::move(message), std::move(broker_code)};
}

Error classify_http(int status, std::string_view body) {
  // We map on the HTTP status first (the broker-neutral transport signal) and
  // use the body only as a refinement hint. The body is NEVER stored on the
  // Error: it can contain tokens. broker_code carries just the status code.
  const std::string code = "HTTP " + std::to_string(status);

  // 2xx is not an error path; if a caller routes a success here, surface it as
  // an internal misuse rather than silently inventing a category.
  if (status >= 200 && status < 300) {
    return make_error(ErrorCategory::Internal, "unexpected success status routed to classify_http",
                      code);
  }

  if (status == 401 || status == 403) {
    // Distinguish a dead session from a hard auth failure where the body says so.
    if (contains_ci(body, "expired") || contains_ci(body, "session")) {
      return make_error(ErrorCategory::SessionExpired, "session expired or invalid", code);
    }
    return make_error(ErrorCategory::Auth, "authentication failed", code);
  }
  if (status == 429) {
    return make_error(ErrorCategory::RateLimited, "rate limited by broker", code);
  }
  if (status == 404) {
    return make_error(ErrorCategory::OrderNotFound, "resource or order not found", code);
  }
  if (status == 408 || status == 504) {
    // A gateway/request timeout on a dangerous op must reconcile, never repeat.
    return make_error(ErrorCategory::Timeout, "request timed out", code);
  }
  if (status == 400 || status == 422) {
    return make_error(ErrorCategory::Validation, "request rejected as invalid", code);
  }
  if (status >= 500) {
    // 5xx / broker outage: reconcile, block new entries, retry health quietly.
    return make_error(ErrorCategory::Network, "broker server error", code);
  }
  if (status >= 400) {
    return make_error(ErrorCategory::BrokerRejected, "broker refused the request", code);
  }

  return make_error(ErrorCategory::Unknown, "unclassified transport status", code);
}

Error classify(std::string_view broker_code, std::string_view message) {
  // Single place broker error text is parsed. We preserve the short broker_code
  // (a code, not a body) and derive a minimal redaction-safe message; we do not
  // echo the raw `message` verbatim.
  std::string code(broker_code);

  const auto hit = [&](std::string_view kw) noexcept {
    return contains_ci(broker_code, kw) || contains_ci(message, kw);
  };

  // Order matters: most specific / most dangerous conditions first.
  if (hit("insufficient") || hit("margin shortfall") || hit("not enough")) {
    return make_error(ErrorCategory::InsufficientFunds, "insufficient funds / margin shortfall",
                      std::move(code));
  }
  if (hit("rms") || hit("risk")) {
    return make_error(ErrorCategory::RiskRejected, "broker risk system rejected the order",
                      std::move(code));
  }
  if (hit("session") || hit("token") || hit("expired")) {
    return make_error(ErrorCategory::SessionExpired, "session expired; re-establish required",
                      std::move(code));
  }
  if (hit("unauthor") || hit("auth") || hit("forbidden") || hit("api_key") || hit("apikey")) {
    return make_error(ErrorCategory::Auth, "authentication failed", std::move(code));
  }
  if (hit("rate limit") || hit("too many") || hit("throttle")) {
    return make_error(ErrorCategory::RateLimited, "rate limited by broker", std::move(code));
  }
  if (hit("duplicate") || hit("already exists") || hit("already placed")) {
    return make_error(ErrorCategory::DuplicateOrder, "broker reports a duplicate order",
                      std::move(code));
  }
  if (hit("not found") || hit("does not exist") || hit("already filled") ||
      hit("already cancelled") || hit("already canceled")) {
    return make_error(ErrorCategory::OrderNotFound, "order not found or already terminal",
                      std::move(code));
  }
  if (hit("market closed") || hit("market is closed") || hit("outside") || hit("session hours") ||
      hit("not open")) {
    return make_error(ErrorCategory::MarketClosed, "market closed / outside session",
                      std::move(code));
  }
  if (hit("not supported") || hit("unsupported") || hit("not allowed") || hit("not enabled")) {
    return make_error(ErrorCategory::NotSupported, "capability not supported by broker",
                      std::move(code));
  }
  if (hit("timeout") || hit("timed out")) {
    return make_error(ErrorCategory::Timeout, "broker call timed out", std::move(code));
  }
  if (hit("invalid") || hit("lot") || hit("tick") || hit("price") || hit("quantity") ||
      hit("symbol") || hit("instrument") || hit("product")) {
    return make_error(ErrorCategory::Validation, "request rejected as invalid", std::move(code));
  }
  if (hit("network") || hit("connection") || hit("unreachable") || hit("reset")) {
    return make_error(ErrorCategory::Network, "network/connection failure", std::move(code));
  }

  // No keyword matched and a code was supplied: a stated-but-unmapped broker
  // rejection. With nothing at all, it is genuinely ambiguous -> Unknown.
  if (!broker_code.empty() || !message.empty()) {
    return make_error(ErrorCategory::BrokerRejected, "broker rejected the request",
                      std::move(code));
  }
  return make_error(ErrorCategory::Unknown, "ambiguous broker error", std::move(code));
}

}  // namespace broker_exec::errors
