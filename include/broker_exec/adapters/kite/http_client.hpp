#pragma once

// broker_exec::adapters::kite::HttpClient — the HTTP transport seam beneath the
// Kite Connect REST client (Story 2.3, AC-1/AC-3).
//
// This is a thin, broker-agnostic request/response abstraction so the protocol
// layer (KiteRestClient) can be exercised by recorded fixtures with NO network
// and NO live credentials (the conformance / VCR substrate, TO-6). The concrete
// libcurl/cpr implementation lives in `src/adapters/kite/cpr_http_client.*`; NO
// cpr type ever appears in this header — only the standard library does.
//
// NO-THROW POLICY: `send()` returns `Result<HttpResponse>`; a transport failure
// (DNS/connect/timeout) is a typed `errors::Error`, never an exception.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "broker_exec/result.hpp"

namespace broker_exec::adapters::kite {

// A single HTTP request expressed in transport-neutral terms. `path` is appended
// to the client's configured base URL; `query` and `headers` are ordered
// key/value lists (Kite never needs multi-map semantics here).
struct HttpRequest {
  enum class Method { Get, Post, Put, Delete };

  Method method = Method::Get;
  std::string path;
  std::vector<std::pair<std::string, std::string>> query;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};

// A single HTTP response. `headers` carry the rate-limit signal (X-RateLimit-*,
// Retry-After) the rate limiter (Story 2.12) consumes via `find_header`.
struct HttpResponse {
  long status_code = 0;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;

  // Case-insensitive header lookup (HTTP header names are case-insensitive).
  // Returns the first matching value, or nullopt when absent.
  [[nodiscard]] std::optional<std::string> find_header(std::string_view name) const {
    const auto eq_ci = [](char a, char b) noexcept {
      return std::tolower(static_cast<unsigned char>(a)) ==
             std::tolower(static_cast<unsigned char>(b));
    };
    for (const auto& kv : headers) {
      if (kv.first.size() == name.size() &&
          std::equal(kv.first.begin(), kv.first.end(), name.begin(), eq_ci)) {
        return kv.second;
      }
    }
    return std::nullopt;
  }
};

// Abstract HTTP transport. Header-only, pure-virtual; concrete impls live in
// `adapters` (the only layer that may link a transport library).
class HttpClient {
 public:
  virtual ~HttpClient() = default;

  // Issue one request. A transport-layer failure yields a typed Error; an HTTP
  // error *status* is a successful `HttpResponse` (the protocol layer maps it).
  [[nodiscard]] virtual Result<HttpResponse> send(const HttpRequest& request) const = 0;
};

}  // namespace broker_exec::adapters::kite
