#include "broker_exec/adapters/kite/cpr_http_client.hpp"

#include <cpr/cpr.h>

#include <chrono>
#include <string>
#include <utility>

#include "broker_exec/adapters/kite/http_client.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/result.hpp"

// cpr/libcurl is confined to THIS translation unit (boundary rule): no cpr type
// appears in any header. Everything below maps cpr's vocabulary onto the
// transport-neutral HttpRequest/HttpResponse + the typed error taxonomy.

namespace broker_exec::adapters::kite {

namespace {

// Map a cpr transport error (DNS/connect/timeout/...) onto the typed taxonomy.
// We never echo cpr's free-form message (it can carry the resolved URL/host);
// instead we derive a fixed, redaction-safe message and a short broker_code.
// Dangerous-op vs safe-read disambiguation is the caller's concern — at the raw
// transport we pick the conservative ReconcileFirst baseline for ambiguity.
[[nodiscard]] errors::Error map_cpr_error(const cpr::Error& err) {
  const std::string code = "CURL " + std::to_string(static_cast<int>(err.code));
  switch (err.code) {
    case cpr::ErrorCode::OPERATION_TIMEDOUT:
      return errors::make_error(errors::ErrorCategory::Timeout, "transport timed out", code);
    case cpr::ErrorCode::CONNECTION_FAILURE:
    case cpr::ErrorCode::HOST_RESOLUTION_FAILURE:
    case cpr::ErrorCode::PROXY_RESOLUTION_FAILURE:
    case cpr::ErrorCode::SSL_CONNECT_ERROR:
      return errors::make_error(errors::ErrorCategory::Network, "transport connection failure",
                                code);
    default:
      return errors::make_error(errors::ErrorCategory::Network, "transport failure", code);
  }
}

}  // namespace

CprHttpClient::CprHttpClient(std::string base_url, std::int64_t timeout_ms)
    : base_url_(std::move(base_url)), timeout_ms_(timeout_ms) {}

Result<HttpResponse> CprHttpClient::send(const HttpRequest& request) const {
  cpr::Session session;
  session.SetUrl(cpr::Url{base_url_ + request.path});

  cpr::Parameters params;
  for (const auto& [key, value] : request.query) {
    params.Add({key, value});
  }
  session.SetParameters(params);

  cpr::Header header;
  for (const auto& [key, value] : request.headers) {
    header[key] = value;
  }
  session.SetHeader(header);

  if (!request.body.empty()) {
    session.SetBody(cpr::Body{request.body});
  }

  // ── REFUSE REDIRECTS ──────────────────────────────────────────────────────
  // A default-constructed cpr::Session enables CURLOPT_FOLLOWLOCATION with
  // MAXREDIRS=50 and CURLOPT_POSTREDIR=CURL_REDIR_POST_ALL (cpr/redirect.h). For
  // a GET that is merely surprising; for the POST that places an order it is a
  // duplicate-order machine — libcurl re-sends the body on every hop, so one
  // place() could put up to 50 orders on the wire while the intent log recorded
  // a single send. The duplicate would be created BELOW dispatch()'s
  // record -> fsync -> send -> record chokepoint, invisible to the idempotency
  // reservation that exists to prevent exactly this.
  //
  // Following a redirect would also hand the live access token to whatever host
  // the Location names: the Kite credential travels in a custom Authorization
  // header, and libcurl forwards custom headers across a redirect regardless of
  // CURLOPT_UNRESTRICTED_AUTH (which only governs its own CURLOPT_USERPWD).
  //
  // A broker REST API has no reason to redirect an order. A 3xx therefore stays
  // a 3xx: the rest client rejects any non-2xx, and classify_http has no 3xx
  // branch, so it lands on Unknown/ReconcileFirst — the order goes UNKNOWN and is
  // reconciled against broker truth. Fail-closed is the right answer to a
  // response we do not understand.
  session.SetRedirect(cpr::Redirect{/*maximum=*/0L, /*follow=*/false,
                                    /*cont_send_cred=*/false, cpr::PostRedirectFlags::NONE});

  // Build the timeout from a chrono duration so the int64 ms value is not
  // brace-narrowed into cpr::Timeout's int32 overload.
  session.SetTimeout(cpr::Timeout{std::chrono::milliseconds{timeout_ms_}});

  cpr::Response response;
  switch (request.method) {
    case HttpRequest::Method::Get:
      response = session.Get();
      break;
    case HttpRequest::Method::Post:
      response = session.Post();
      break;
    case HttpRequest::Method::Put:
      response = session.Put();
      break;
    case HttpRequest::Method::Delete:
      response = session.Delete();
      break;
  }

  // A transport-level failure (no HTTP status reached) -> typed Error, no throw.
  if (response.error) {
    return broker_exec::fail(map_cpr_error(response.error));
  }

  HttpResponse out;
  out.status_code = response.status_code;
  out.headers.reserve(response.header.size());
  for (const auto& [key, value] : response.header) {
    out.headers.emplace_back(key, value);
  }
  out.body = std::move(response.text);
  return out;
}

}  // namespace broker_exec::adapters::kite
