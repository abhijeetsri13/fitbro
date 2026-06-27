#pragma once

// broker_exec::adapters::kite::CprHttpClient — the cpr/libcurl implementation of
// the HttpClient transport seam (Story 2.3, AC-1).
//
// This header is cpr-FREE on purpose: cpr/libcurl is included ONLY in
// cpr_http_client.cpp so the transport library never leaks above the adapter
// boundary. The client is configured once with a base URL + per-request timeout
// (sourced from `config::BrokerConfig`); a transport failure (DNS/connect/
// timeout) is mapped to a typed `errors::Error`, never thrown.
//
// Cross-platform: C++20 standard library only in this header.

#include <cstdint>
#include <string>

#include "broker_exec/adapters/kite/http_client.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::adapters::kite {

class CprHttpClient final : public HttpClient {
 public:
  // `base_url` is prefixed to every request path (e.g. "https://api.kite.trade");
  // `timeout_ms` is the per-request timeout in milliseconds (config-driven).
  CprHttpClient(std::string base_url, std::int64_t timeout_ms);

  [[nodiscard]] Result<HttpResponse> send(const HttpRequest& request) const override;

 private:
  std::string base_url_;
  std::int64_t timeout_ms_;
};

}  // namespace broker_exec::adapters::kite
