#include "broker_exec/adapters/kotak/kotak_errors.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "broker_exec/adapters/kotak/kotak_transport.hpp"
#include "broker_exec/brokerreason/rejection_classifier.hpp"
#include "broker_exec/domain/redaction.hpp"
#include "broker_exec/errors/error.hpp"

namespace broker_exec::adapters::kotak {

using json = nlohmann::json;

namespace {

// Render a JSON scalar as text without ever promoting it to a double (no float in
// any path). Strings pass through; integers/unsigned print exactly; anything else
// is left empty rather than guessed.
[[nodiscard]] std::string scalar_to_text(const json& value) {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_number_integer()) {
    return std::to_string(value.get<long long>());
  }
  if (value.is_number_unsigned()) {
    return std::to_string(value.get<unsigned long long>());
  }
  if (value.is_boolean()) {
    return value.get<bool>() ? "true" : "false";
  }
  return std::string{};
}

// First present string-ish field among `names`, as text ("" when none matched).
[[nodiscard]] std::string first_text(const json& object, std::initializer_list<const char*> names) {
  for (const char* name : names) {
    const auto it = object.find(name);
    if (it != object.end() && !it->is_null() && !it->is_object() && !it->is_array()) {
      std::string text = scalar_to_text(*it);
      if (!text.empty()) {
        return text;
      }
    }
  }
  return std::string{};
}

[[nodiscard]] char lower_ascii(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool equals_ignore_ascii_case(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower_ascii(a[i]) != lower_ascii(b[i])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::string to_lower_ascii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    out.push_back(lower_ascii(c));
  }
  return out;
}

[[nodiscard]] bool is_token_char(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '-';
}

[[nodiscard]] bool is_digit_char(char c) noexcept {
  return c >= '0' && c <= '9';
}

// ── Kotak-specific pre-redaction ─────────────────────────────────────────────
//
// `domain::scrub` denylists token/secret/password/api_key/mpin/totp/bearer and
// redacts high-entropy runs (>=20 mixed alnum chars). Neither rule covers
// Kotak's OWN session artifacts: `sid` and `Auth` are not in the denylist, and a
// Kotak sid can be a SHORT ALL-DIGIT run ("884213") that no entropy rule will
// ever catch. Relying on scrub() alone therefore leaks a session id the moment
// the broker echoes a short one back at us.
//
// So: before untrusted broker text reaches scrub(), blank out any value that the
// text ITSELF labels as a sid/auth artifact. domain/ is deliberately untouched —
// this is a broker-specific denylist and belongs in the broker's adapter.
constexpr std::array<std::string_view, 5> kKotakSecretKeys = {"sid", "auth", "hsserverid",
                                                              "serverid", "jsessionid"};

[[nodiscard]] bool is_kotak_secret_key(std::string_view key) {
  for (const std::string_view needle : kKotakSecretKeys) {
    if (equals_ignore_ascii_case(key, needle)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::string redact_kotak_keys(std::string_view text) {
  std::string out;
  out.reserve(text.size() + domain::kRedactionMarker.size());

  const std::size_t n = text.size();
  std::size_t i = 0;
  while (i < n) {
    // Only consider a KEY at a token-run boundary, so `sid` inside a longer word
    // ("consid") is never treated as a key.
    if (!is_token_char(text[i]) || (i > 0 && is_token_char(text[i - 1]))) {
      out.push_back(text[i]);
      ++i;
      continue;
    }
    std::size_t key_end = i;
    while (key_end < n && is_token_char(text[key_end])) {
      ++key_end;
    }
    const std::string_view key = text.substr(i, key_end - i);
    out.append(key);
    i = key_end;
    if (!is_kotak_secret_key(key)) {
      continue;
    }

    // Consume (and echo) the separator run between the key and its value.
    std::size_t cur = i;
    bool explicit_separator = false;
    while (cur < n && (text[cur] == '"' || text[cur] == '\'' || text[cur] == ' ' ||
                       text[cur] == '\t' || text[cur] == ':' || text[cur] == '=')) {
      explicit_separator = explicit_separator || text[cur] == ':' || text[cur] == '=';
      out.push_back(text[cur]);
      ++cur;
    }
    std::size_t value_end = cur;
    while (value_end < n && is_token_char(text[value_end])) {
      ++value_end;
    }
    const std::string_view value = text.substr(cur, value_end - cur);

    // With an explicit `:`/`=` the pairing is unambiguous, so ANY value goes.
    // With only whitespace ("invalid sid 884213") redact only value-SHAPED runs,
    // so ordinary prose ("auth failed for this user") survives for diagnostics.
    const bool has_digit = std::any_of(value.begin(), value.end(), is_digit_char);
    const bool value_shaped = value.size() >= 3 && (value.size() >= 12 || has_digit);
    if (!value.empty() && (explicit_separator || value_shaped)) {
      out.append(domain::kRedactionMarker);
      i = value_end;
    } else {
      i = cur;
    }
  }
  return out;
}

// The full outward transform for untrusted broker text: Kotak's own artifacts
// first, then the shared scrubber. Both are idempotent, and so is the pair.
[[nodiscard]] std::string scrub_broker_text(std::string_view text) {
  return domain::scrub(redact_kotak_keys(text));
}

// ── Session-verdict phrasing ─────────────────────────────────────────────────
//
// `brokerreason` matches a BARE "token", which is far too loose here: an
// "Invalid instrument token" ORDER REJECT would masquerade as a dead session,
// triggering a spurious re-auth and — from validate() — a false NeedsReauth on a
// perfectly healthy session. In this module a phrasing-only session verdict
// therefore ALSO requires session-ish co-phrasing. An HTTP 401/403 remains
// sufficient on its own.
constexpr std::array<std::string_view, 8> kSessionCoPhrases = {
    "session",     "expired",      "re-login",     "relogin",
    "login again", "log in again", "unauthorized", "invalid credentials"};

[[nodiscard]] bool has_session_co_phrase(std::string_view message) {
  const std::string lowered = to_lower_ascii(message);
  return std::any_of(
      kSessionCoPhrases.begin(), kSessionCoPhrases.end(),
      [&lowered](std::string_view phrase) { return lowered.find(phrase) != std::string::npos; });
}

// Fill the envelope from a JSON OBJECT (the array case is unwrapped by the
// caller). Never throws: every lookup is a find() on an already-parsed value.
void read_object(const json& object, KotakEnvelope& env) {
  env.parsed = true;

  if (const auto stat = object.find("stat"); stat != object.end() && stat->is_string()) {
    env.has_stat = true;
    env.stat_ok = equals_ignore_ascii_case(stat->get<std::string>(), "Ok");
  }
  env.has_data = object.find("data") != object.end();

  // The API-gateway fault shape sits beside (never inside) the trading envelope.
  if (const auto fault = object.find("fault"); fault != object.end() && fault->is_object()) {
    env.has_fault = true;
    env.message = first_text(*fault, {"message", "faultstring", "description"});
    env.status_code = first_text(*fault, {"code", "errorcode", "errorCode"});
    if (env.status_code.empty()) {
      if (const auto detail = fault->find("detail");
          detail != fault->end() && detail->is_object()) {
        env.status_code = first_text(*detail, {"errorcode", "errorCode", "code"});
      }
    }
  }

  // The login endpoints answer failures with an `error` node instead of the
  // trading envelope: an array of {code,message}, an object, or a bare string.
  if (const auto error_node = object.find("error"); error_node != object.end()) {
    const json* record = nullptr;
    if (error_node->is_array() && !error_node->empty() && error_node->front().is_object()) {
      record = &error_node->front();
    } else if (error_node->is_object()) {
      record = &(*error_node);
    } else if (error_node->is_string()) {
      env.has_error = true;
      if (env.message.empty()) {
        env.message = error_node->get<std::string>();
      }
    }
    if (record != nullptr) {
      env.has_error = true;
      if (env.message.empty()) {
        env.message = first_text(*record, {"message", "errMsg", "description"});
      }
      if (env.status_code.empty()) {
        env.status_code = first_text(*record, {"code", "errorcode", "errorCode"});
      }
    }
  }

  // Trading-envelope error text. Kotak spells this errMsg / errmsg / emsg /
  // message depending on the endpoint generation; take whichever is present.
  if (env.message.empty()) {
    env.message = first_text(object, {"errMsg", "errmsg", "emsg", "errorMessage", "message"});
  }
  if (env.status_code.empty()) {
    env.status_code = first_text(object, {"stCode", "stcode", "errCode", "code"});
  }
}

// Map a canonical rejection reason onto the broker-neutral error category.
// Unknown/Indeterminate deliberately fall through to the caller's fail-closed
// handling rather than being forced into a category here.
[[nodiscard]] bool category_for_reason(brokerreason::RejectReason reason,
                                       errors::ErrorCategory& out) {
  switch (reason) {
    case brokerreason::RejectReason::Margin:
      out = errors::ErrorCategory::InsufficientFunds;
      return true;
    case brokerreason::RejectReason::CircuitLimit:
    case brokerreason::RejectReason::FreezeQuantity:
      out = errors::ErrorCategory::Validation;
      return true;
    case brokerreason::RejectReason::Illiquid:
      out = errors::ErrorCategory::BrokerRejected;
      return true;
    case brokerreason::RejectReason::RmsBlock:
      out = errors::ErrorCategory::RiskRejected;
      return true;
    case brokerreason::RejectReason::SessionExpired:
      out = errors::ErrorCategory::SessionExpired;
      return true;
    case brokerreason::RejectReason::RateLimited:
      out = errors::ErrorCategory::RateLimited;
      return true;
    case brokerreason::RejectReason::OrderNotFound:
      out = errors::ErrorCategory::OrderNotFound;
      return true;
    case brokerreason::RejectReason::AlreadyComplete:
      out = errors::ErrorCategory::BrokerRejected;
      return true;
    case brokerreason::RejectReason::Indeterminate:
    case brokerreason::RejectReason::Unknown:
      break;
  }
  return false;
}

}  // namespace

KotakEnvelope parse_kotak_envelope(std::string_view body) {
  KotakEnvelope env;
  // Iterator overload: unambiguous for a string_view on every supported toolchain.
  const json parsed = json::parse(body.begin(), body.end(), nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded()) {
    return env;
  }
  if (parsed.is_object()) {
    read_object(parsed, env);
    return env;
  }
  // Some Kotak reads answer with a bare array of records; that is a success shape
  // with no envelope fields, so only the first object (if any) is inspected.
  if (parsed.is_array() && !parsed.empty() && parsed.front().is_object()) {
    read_object(parsed.front(), env);
  }
  return env;
}

bool is_kotak_success(const HttpResponse& response) {
  if (response.status_code < 200 || response.status_code >= 300) {
    return false;
  }
  const KotakEnvelope env = parse_kotak_envelope(response.body);
  if (env.has_fault || env.has_error) {
    return false;
  }
  // A missing `stat` is treated as success ONLY on a 2xx: several Kotak reads
  // (scrip master, file paths) answer without the trading envelope at all.
  return !env.has_stat || env.stat_ok;
}

bool is_session_death(const HttpResponse& response) {
  if (response.status_code == 401 || response.status_code == 403) {
    return true;
  }
  // A 5xx (or a status-less synthesized response) is the SERVER being broken,
  // never a verdict about our session. Claiming session death here would demand
  // a spurious operator re-auth during a broker outage — and, worse, would mask
  // the reconcile-first posture the outage actually requires.
  if (response.status_code >= 500 || response.status_code <= 0) {
    return false;
  }
  if (is_kotak_success(response)) {
    return false;
  }
  const KotakEnvelope env = parse_kotak_envelope(response.body);
  if (env.message.empty()) {
    return false;
  }
  // Same tightened rule as map_kotak_error: a bare "token" match is NOT enough.
  return brokerreason::classify_rejection(env.message).reason ==
             brokerreason::RejectReason::SessionExpired &&
         has_session_co_phrase(env.message);
}

errors::Error map_kotak_error(const HttpResponse& response) {
  const long status = response.status_code;
  const KotakEnvelope env = parse_kotak_envelope(response.body);

  // broker_code: short, secret-free diagnostics only. stCode/fault codes are
  // numeric or short slugs, but they are BROKER-CONTROLLED text, so scrub them
  // defensively — broker_code is the one Error field not otherwise redacted.
  std::string code = "HTTP " + std::to_string(status);
  if (!env.status_code.empty()) {
    code += " stCode=";
    code += scrub_broker_text(env.status_code);
  }
  if (const auto retry_after = response.find_header("Retry-After")) {
    code += " Retry-After=";
    code += scrub_broker_text(*retry_after);
  }

  // The broker message is UNTRUSTED: redact Kotak's own sid/auth artifacts and
  // then scrub, before it can reach any Error/log.
  const std::string scrubbed = env.message.empty() ? std::string{} : scrub_broker_text(env.message);

  const auto build = [&](errors::ErrorCategory category, errors::SuggestedAction action,
                         std::string_view fallback) {
    errors::Error error;
    error.category = category;
    error.action = action;
    error.message = scrubbed.empty() ? std::string(fallback) : ("kotak: " + scrubbed);
    error.broker_code = code;
    return error;
  };

  // Phrasing classification via the versioned canonical classifier (the shared,
  // fail-closed reason table). It never retains the raw text.
  const brokerreason::Classification classification =
      env.message.empty() ? brokerreason::Classification{}
                          : brokerreason::classify_rejection(env.message);

  // The classifier's SessionExpired keyword set includes a bare "token", so an
  // "Invalid instrument token" order reject would otherwise be read as a dead
  // session. Demote such a match unless the text really reads like a session
  // failure; it then falls through to the fail-closed branches below.
  brokerreason::RejectReason reason = classification.reason;
  if (reason == brokerreason::RejectReason::SessionExpired && !has_session_co_phrase(env.message)) {
    reason = brokerreason::RejectReason::Unknown;
  }

  // 1. **THE POSSIBLY-LIVE-ORDER GUARD, FIRST.** A 5xx / status-less / timeout-
  // shaped failure means the request may have reached the exchange and left a
  // LIVE order: the only safe verdict is "reconcile against broker truth".
  //
  // THIS MUST OUTRANK EVERY PHRASING BRANCH. A 502 whose body happens to say
  // "throttled" is NOT a safe retry (that re-places the order), and a 503 whose
  // body says "token expired" is NOT a re-auth (that orphans the order). The
  // body of a failing gateway is a hint; the status is the fact.
  if (status >= 500 || status <= 0 || reason == brokerreason::RejectReason::Indeterminate) {
    return build(errors::ErrorCategory::Network, errors::SuggestedAction::ReconcileFirst,
                 "kotak: broker/transport error; reconcile required");
  }

  // 2. Session death: a dead session must re-establish, not retry.
  if (status == 401 || status == 403 || reason == brokerreason::RejectReason::SessionExpired) {
    return build(errors::ErrorCategory::SessionExpired, errors::SuggestedAction::ReEstablishSession,
                 "kotak: session expired or invalid; re-establish required");
  }

  // 3. Throttling. RetrySafe is a READ-ONLY posture — see the caller obligation
  // documented in the header; the REST client downgrades this to ReconcileFirst
  // on a mutation, because a 429 on a place is still ambiguous.
  if (status == 429 || reason == brokerreason::RejectReason::RateLimited) {
    return build(errors::ErrorCategory::RateLimited, errors::SuggestedAction::RetrySafe,
                 "kotak: rate limited by broker");
  }

  // 4. A canonical, phrasing-classified broker verdict (margin, circuit, freeze,
  // RMS, order-not-found, ...). The posture comes straight from the classifier.
  if (errors::ErrorCategory category = errors::ErrorCategory::Unknown;
      category_for_reason(reason, category)) {
    return build(category, brokerreason::to_suggested_action(classification.posture),
                 "kotak: broker rejected the request");
  }

  // 5. A 404 is NOT a client-input rejection here. Our endpoint paths are an
  // UNVERIFIED tier-2 assumption, so a 404 on cancel/modify may mean "we asked
  // the wrong URL", not "that order does not exist" — abandoning a possibly-live
  // order on that evidence is exactly the failure we refuse to risk. Reconcile.
  if (status == 404) {
    return build(errors::ErrorCategory::OrderNotFound, errors::SuggestedAction::ReconcileFirst,
                 "kotak: order or endpoint not found; reconcile required");
  }

  // 6. A genuine client-input rejection: the broker gave a verdict and the order
  // was never accepted, so fixing the request is the only correct action.
  if (status == 400 || status == 422) {
    return build(errors::ErrorCategory::Validation, errors::SuggestedAction::DoNotRetry,
                 "kotak: request rejected as invalid");
  }

  // 7. FAIL CLOSED. We could not classify it, so we do not know whether an order
  // is live: reconcile first (the strictly safer of the two unsafe guesses).
  if (env.has_fault || env.has_error || !scrubbed.empty() || (env.has_stat && !env.stat_ok)) {
    return build(errors::ErrorCategory::BrokerRejected, errors::SuggestedAction::ReconcileFirst,
                 "kotak: broker rejected the request");
  }
  return build(errors::ErrorCategory::Unknown, errors::SuggestedAction::ReconcileFirst,
               "kotak: unclassified transport error");
}

}  // namespace broker_exec::adapters::kotak
