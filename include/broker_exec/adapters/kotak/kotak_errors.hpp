#pragma once

// broker_exec::adapters::kotak — the SINGLE Kotak Neo error-mapping point
// (Story 6.1, AC-3, FR-25, SEC-3, CAP-13).
//
// Kotak Neo reports failures in three overlapping shapes, and every one of them
// has to collapse into the same broker-neutral taxonomy Kite maps into (CAP-13:
// the same condition on both brokers resolves to the same category):
//
//   1. the trading envelope:   {"stat":"Not_Ok","errMsg":"...","stCode":5203}
//   2. an API-gateway fault:   {"fault":{"code":"...","message":"..."}}
//                              (also seen as {"fault":{"faultstring":"...",
//                               "detail":{"errorcode":"..."}}})
//   3. a login error node:     {"error":[{"code":"10022","message":"..."}]}
//   4. a bare HTTP status:     401/403/429/5xx with an empty or non-JSON body
//
// NOTE THE TRAP: Kotak frequently returns **HTTP 200 with `stat:"Not_Ok"`**, so
// status-code-only mapping silently swallows rejections. `map_kotak_error` reads
// the envelope FIRST and the status second.
//
// THE SAFETY ORDERING (read twice — the ORDER is the safety property):
//   1. 5xx / status-less / timeout-shaped -> Network / ReconcileFirst
//        THE POSSIBLY-LIVE-ORDER GUARD, AND IT COMES FIRST. Such a failure may
//        have left a LIVE order at the exchange, so the caller must reconcile
//        against broker truth. It deliberately outranks every phrasing branch: a
//        502 whose body says "throttled" is NOT a safe retry (that re-places the
//        order) and a 503 whose body says "token expired" is NOT a re-auth (that
//        orphans it). A failing gateway's body is a hint; its status is the fact.
//   2. 401/403/session-death -> SessionExpired  / ReEstablishSession
//   3. 429                   -> RateLimited     / RetrySafe (+Retry-After code)
//   4. phrasing-classified   -> the `brokerreason` canonical reason + posture
//   5. 404                   -> OrderNotFound   / ReconcileFirst (NOT DoNotRetry:
//        our endpoint paths are an unverified tier-2 assumption, so a 404 may
//        mean "wrong URL", not "no such order")
//   6. 400/422 client reject -> Validation      / DoNotRetry
//   7. anything else         -> fail closed (BrokerRejected/Unknown + Reconcile)
//
// SESSION PHRASING IS TIGHTENED HERE: `brokerreason` matches a bare "token", so
// an "Invalid instrument token" ORDER REJECT would otherwise read as a dead
// session (spurious re-auth; false NeedsReauth from validate()). A phrasing-only
// session verdict additionally requires session-ish co-phrasing; 401/403 alone
// still suffices.
//
// CALLER OBLIGATION (inherited from brokerreason): RetrySafe is only safe for an
// IDEMPOTENT READ. A 429 on a mutation is still ambiguous about whether the order
// reached the exchange — `KotakRestClient` downgrades it to ReconcileFirst on
// place/modify/cancel.
//
// REDACTION: `errMsg`/`fault.message` are UNTRUSTED broker text that can echo a
// token, a sid, or an account id. Every string that reaches `errors::Error`
// (message AND broker_code) is redacted twice: a Kotak-specific pass that blanks
// `sid`/`Auth`-keyed values (which `domain::scrub` does not denylist, and which
// can be SHORT ALL-DIGIT runs no entropy rule would catch), then `domain::scrub`
// itself. No credential (consumer secret, MPIN, TOTP, access_token, token, sid)
// is ever placed on an Error, logged, or persisted by this module.
//
// NO-THROW: pure value logic over already-received bytes; the JSON parse uses
// `allow_exceptions=false`. Cross-platform: C++20 standard library +
// nlohmann_json. No OS APIs, no `#ifdef`, no float.

#include <string>
#include <string_view>

#include "broker_exec/adapters/kotak/kotak_transport.hpp"
#include "broker_exec/errors/error.hpp"

namespace broker_exec::adapters::kotak {

// The parsed Kotak response envelope. Purely descriptive: it holds the RAW
// broker strings, which callers must scrub before letting them reach any sink.
// `map_kotak_error` is the only place in the adapter that does so.
struct KotakEnvelope {
  bool parsed = false;        // the body was a JSON object (or array of objects)
  bool has_stat = false;      // a "stat" field was present
  bool stat_ok = false;       // stat == "Ok" (case-insensitive)
  bool has_fault = false;     // an API-gateway "fault" object was present
  bool has_error = false;     // a login-style {"error":[{code,message}]} node
  bool has_data = false;      // a "data" payload was present
  std::string message;        // RAW errMsg / emsg / message / fault message
  std::string status_code;    // RAW stCode / fault code, as text ("" when absent)
};

// Parse a Kotak response body into the envelope above. A non-JSON or non-object
// body yields `parsed == false` with every other field left at its default —
// never an exception.
[[nodiscard]] KotakEnvelope parse_kotak_envelope(std::string_view body);

// True when the response is a Kotak SUCCESS: a 2xx status AND (no `stat` field,
// or `stat == "Ok"`) AND no gateway fault. The `stat` check is what stops an
// HTTP-200 `Not_Ok` rejection from being read as success.
[[nodiscard]] bool is_kotak_success(const HttpResponse& response);

// True when the response means the SESSION IS DEAD (401/403, a gateway auth
// fault, or session/token phrasing in the envelope). Callers surface this as
// `SessionState::NeedsReauth` — a normalized state, never a silent refresh.
[[nodiscard]] bool is_session_death(const HttpResponse& response);

// THE single mapping point from a raw Kotak response to the typed taxonomy.
// See the ordering contract at the top of this header. The returned Error is
// scrubbed and credential-free by construction.
[[nodiscard]] errors::Error map_kotak_error(const HttpResponse& response);

}  // namespace broker_exec::adapters::kotak
