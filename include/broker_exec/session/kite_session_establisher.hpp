#pragma once

// broker_exec::session::KiteSessionEstablisher — the daily Kite login modeled as
// first-class establishment + expiry detection (Story 2.4, FR-21, IBR-1).
//
// Kite Connect issues a daily access_token that dies (~6am) with NO headless
// refresh: the operator supplies a fresh `request_token` at boot, which we
// exchange for the access_token, persist ENCRYPTED via TokenStore, and gate the
// safe-start on. A dead token is surfaced as `SessionState::NeedsReauth` (never a
// silent "refresh") so the runtime blocks trading and the operator re-logs in.
//
// `session` is an UPPER orchestration module: it may depend on `adapters/kite`
// (the HttpClient seam + map_http_error), `secrets` (TokenStore), `ports`
// (SecretProvider), and `errors`. It is NOT `domain`/`ports`.
//
// SECRETS: the api_key/api_secret/request_token, the derived checksum, and the
// access_token are NEVER logged or placed in any Error message/broker_code (the
// checksum is derived from the api_secret, so it is sensitive too). The
// access_token is persisted only through the TokenStore (AES-256-GCM at rest).
//
// NO-THROW POLICY: every method returns `Result<T>`; nothing throws across the
// boundary.
//
// Cross-platform: C++20 standard library + OpenSSL (portable). No OS APIs, no
// `#ifdef`.

#include <string>

#include "broker_exec/adapters/kite/http_client.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/secret_provider.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/secrets/token_store.hpp"
#include "broker_exec/session/session_state.hpp"

namespace broker_exec::session {

class KiteSessionEstablisher {
 public:
  // Kite has no server-to-server token refresh: the daily token is operator-
  // supplied at boot. Story 2.5 folds this into the full capability model; here
  // it must read unsupported (AC-3).
  static constexpr bool kSupportsHeadlessSessionRefresh = false;

  // `http` issues the requests (transport seam). `secrets` resolves the api_key
  // and api_secret by logical name. `store` persists/loads the daily access_token
  // ENCRYPTED, keyed by (`account_id`, `access_token_store_name`). All four
  // references/values must outlive this object except the copied strings.
  KiteSessionEstablisher(const adapters::kite::HttpClient& http,
                         const ports::SecretProvider& secrets, secrets::TokenStore& store,
                         std::string account_id, std::string api_key_secret_name,
                         std::string api_secret_secret_name, std::string access_token_store_name);

  // Exchange an operator-supplied `request_token` for the daily access_token and
  // persist it encrypted. Computes checksum = SHA-256(api_key+request_token+
  // api_secret), POSTs the UNauthenticated /session/token endpoint form-encoded,
  // and on `{status:"success"}` stores `data.access_token` and returns Healthy.
  // Any broker/transport failure -> a typed, secret-free Error.
  [[nodiscard]] Result<SessionState> establish(std::string request_token);

  // Probe the stored token with a lightweight authenticated read
  // (GET /user/margins/equity). A 401/TokenException -> NeedsReauth (the caller
  // blocks trading and alerts; NEVER auto-refresh). A transport/5xx -> a
  // reconcile-first Error. Success -> Healthy.
  [[nodiscard]] Result<SessionState> validate();

  // The typed alert/error for a dead daily session (SessionExpired +
  // ReEstablishSession), with a fixed secret-free message so the safe-start /
  // alerting path can raise the operator alert without re-deriving it.
  [[nodiscard]] static errors::Error needs_reauth_error();

 private:
  const adapters::kite::HttpClient& http_;
  const ports::SecretProvider& secrets_;
  secrets::TokenStore& store_;
  std::string account_id_;
  std::string api_key_secret_name_;
  std::string api_secret_secret_name_;
  std::string access_token_store_name_;
};

}  // namespace broker_exec::session
