#pragma once

// broker_exec::adapters::kotak — the Kotak Neo MULTI-STEP session
// (Story 6.1, AC-1, FR-21, IBR-6, SEC-3).
//
// WHY THIS IS NOT A STRING. Kite's session is one daily `access_token`. Kotak's
// is NOT: a usable Kotak session is an OPAQUE BUNDLE of several artifacts that
// must travel together on every subsequent call (arch IBR-6) —
//
//     access_token  the OAuth bearer minted from the consumer key/secret
//     token         the post-2FA session token  (the `Auth:` header)
//     sid           the post-2FA session id     (the `Sid:` header)
//     hs_server_id  the routing server id       (the `sId=` query on orders)
//
// Modeling that as one string is exactly how a "logged in" process ends up
// silently unable to place orders, so `KotakSessionBundle` is the unit that is
// built, persisted, loaded and validated — atomically.
//
// THE FLOW (three legs, all over the injected HttpClient seam):
//   1. OAuth        POST /oauth2/token
//                   Authorization: Basic base64(consumer_key:consumer_secret)
//                   -> access_token
//   2. Login        POST /login/1.0/login/v2/validate  (Bearer access_token)
//                   {mobileNumber, password}   -> VIEW token + sid (+hsServerId)
//   3. 2FA          POST /login/1.0/login/v2/validate  (Bearer + Auth + Sid)
//                   {mobileNumber, mpin}       -> FINAL token + sid + hsServerId
//
// ATOMIC PERSISTENCE: NOTHING is written until leg 3 returns a COMPLETE bundle.
// A wrong MPIN, a dead consumer key, or a transport failure at any leg leaves the
// TokenStore exactly as it was — there is no half-session on disk to mistake for
// a live one.
//
// AT REST: the bundle is serialized to JSON and handed to the existing
// `secrets::TokenStore`, i.e. AES-256-GCM encrypted with a key that is NOT
// co-located with the ciphertext (SEC-1). The JSON never touches the filesystem
// in the clear.
//
// SECRETS: consumer key/secret, mobile, password and MPIN are fetched by logical
// name through `ports::SecretProvider` at the start of `establish()` — never at
// construction, never cached on this object — and are gone when the call
// returns. None of them appears in a log, an Error message, or a broker_code.
// The same holds for every field of the bundle itself.
//
// HEADLESS REFRESH IS **UNKNOWN** FOR KOTAK (fail-closed). We have not verified
// whether the Neo session can be renewed server-to-server, so the capability
// model reports `Unknown` — which the gate collapses to unsupported. We do NOT
// optimistically auto-refresh; a dead session surfaces as `NeedsReauth` and the
// operator re-establishes.
//
// NO-THROW: every fallible call returns `Result<T>`. Cross-platform: C++20
// standard library + nlohmann_json (in the .cpp only). No OS APIs, no `#ifdef`,
// no float.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "broker_exec/adapters/kotak/kotak_transport.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/secret_provider.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/secrets/token_store.hpp"
#include "broker_exec/session/session_state.hpp"

namespace broker_exec::adapters::kotak {

// The opaque per-broker session bundle (IBR-6). Every field is SENSITIVE: this
// type has no logging/streaming support on purpose, and `to_json()` exists ONLY
// to feed the encrypting TokenStore.
struct KotakSessionBundle {
  std::string access_token;  // OAuth bearer      -> `Authorization: Bearer ...`
  std::string token;         // post-2FA token    -> `Auth: ...`
  std::string sid;           // post-2FA session  -> `Sid: ...`
  std::string hs_server_id;  // routing server id -> `?sId=...` on order calls

  // Any further 2FA/login artifacts the broker returns (ucc, greeting name,
  // rid, ...). Kept opaque and forward-compatible so a new field does not
  // require a schema change; carried through persistence untouched.
  std::vector<std::pair<std::string, std::string>> extras;

  // A bundle is usable only with all FOUR core artifacts. A partially populated
  // bundle is never persisted and never accepted from disk as a live session.
  [[nodiscard]] bool complete() const noexcept;

  // Lookup into `extras`; nullptr when absent. The pointer is valid while this
  // bundle is alive.
  [[nodiscard]] const std::string* find_extra(std::string_view name) const noexcept;

  // Serialize for ENCRYPTED persistence only. Never log or return this string.
  [[nodiscard]] std::string to_json() const;

  // Parse a bundle previously written by `to_json()`. A malformed blob yields a
  // typed, secret-free Error (the blob is never echoed).
  [[nodiscard]] static Result<KotakSessionBundle> from_json(std::string_view text);
};

// Build the authenticated Kotak headers for one call. Deliberately a free
// function taking the bundle BY REFERENCE at the call site: nothing here is ever
// cached on a client object.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> auth_headers(
    const KotakSessionBundle& bundle);

// The logical SecretProvider names for the login credentials. Non-secret
// configuration (names, not values); the defaults are the story's contract.
struct KotakLoginInputs {
  std::string consumer_key_secret_name = "kotak_consumer_key";
  std::string consumer_secret_secret_name = "kotak_consumer_secret";
  std::string mobile_secret_name = "kotak_mobile";
  std::string password_secret_name = "kotak_password";
  std::string mpin_secret_name = "kotak_mpin";
};

class KotakSessionEstablisher {
 public:
  // Kotak headless re-auth is UNVERIFIED, so it is `Unknown` in the tri-state
  // capability model — which the gate collapses to this fail-closed boolean.
  // See `kotak_capabilities()` for the authoritative tri-state value.
  static constexpr bool kSupportsHeadlessSessionRefresh = false;

  // `http` issues the requests (transport seam). `secrets` resolves the login
  // credentials by logical name. `store` persists/loads the bundle ENCRYPTED,
  // keyed by (`account_id`, `bundle_store_name`). All references must outlive
  // this object.
  KotakSessionEstablisher(const HttpClient& http, const ports::SecretProvider& secrets,
                          secrets::TokenStore& store, std::string account_id,
                          std::string bundle_store_name);

  // Run the full three-leg flow and, ONLY on a complete bundle, persist it
  // encrypted. Returns:
  //   Healthy — bundle built and persisted.
  //   Failed  — the broker REFUSED a leg (wrong MPIN/password, dead consumer
  //             key, `stat:"Not_Ok"`), or answered success with an incomplete
  //             bundle. NOTHING is persisted on this path.
  // A transport failure or a 5xx (i.e. "try again", not "your credentials are
  // wrong") is a typed Error instead.
  //
  // WHY A REFUSAL IS `Failed`, NOT `NeedsReauth`: NeedsReauth means "the live
  // session died — operator, re-establish". Establishment IS that
  // re-establishment; answering it with "re-establish" would loop the operator.
  // A refused credential is a hard establishment failure.
  //
  // The refusal REASON is deliberately not returned: Kotak login error text can
  // echo the submitted credential. Callers needing diagnostics map the response
  // through `map_kotak_error`, which scrubs it.
  [[nodiscard]] Result<session::SessionState> establish(const KotakLoginInputs& inputs = {});

  // Load the persisted bundle and probe it with ONE cheap authenticated read
  // (the order book). Session death -> NeedsReauth (never a silent refresh); a
  // stored-but-incomplete bundle -> Failed; success -> Healthy.
  [[nodiscard]] Result<session::SessionState> validate() const;

  // Load and decrypt the persisted bundle. This is what a caller wires into the
  // REST client's bundle provider so the client holds no credentials of its own.
  [[nodiscard]] Result<KotakSessionBundle> load() const;

  // The fixed, secret-free operator alert for a dead Kotak session.
  [[nodiscard]] static errors::Error needs_reauth_error();

 private:
  // `credential_refusal` is set by the legs to distinguish "the broker positively
  // refused these credentials" (-> Failed) from "the broker was unreachable or
  // broken" (-> a typed Error worth retrying). The decision is made where the
  // raw response is still in hand; by the time only an Error survives, the
  // evidence (did the envelope even parse?) is gone.
  //
  // Leg 1: consumer key/secret -> OAuth access_token. Takes the resolved values
  // (not the names) so `establish` can fail fast on a missing secret BEFORE any
  // request, keeping "config is broken" distinct from "the broker refused us".
  [[nodiscard]] Result<std::string> fetch_access_token(const std::string& consumer_key,
                                                       const std::string& consumer_secret,
                                                       bool& credential_refusal) const;

  // Legs 2 and 3 share one endpoint and one response shape; `extra_headers`
  // carries the view token/sid on leg 3.
  struct LoginLeg {
    std::string token;
    std::string sid;
    std::string hs_server_id;
    std::vector<std::pair<std::string, std::string>> extras;
  };

  [[nodiscard]] Result<LoginLeg> post_login(
      const std::string& access_token, const std::string& body,
      const std::vector<std::pair<std::string, std::string>>& extra_headers,
      bool& credential_refusal) const;

  const HttpClient& http_;
  const ports::SecretProvider& secrets_;
  secrets::TokenStore& store_;
  std::string account_id_;
  std::string bundle_store_name_;
};

}  // namespace broker_exec::adapters::kotak
