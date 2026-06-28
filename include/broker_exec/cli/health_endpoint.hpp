#pragma once

// broker_exec::cli health endpoint — the pure route handler plus a thin localhost
// HTTP server that exposes it (Story 4.6, AC-2/AC-3, FR-36).
//
// TWO LAYERS, deliberately split so the policy is unit-testable without a socket:
//   * `route()` — a PURE function: (method, path, HealthState, live budget) -> a
//     plain `HttpReply` value. This is the unit-tested seam. It NEVER throws and
//     NEVER binds a socket; tests drive it directly.
//   * `HealthHttpServer` — a thin wrapper that registers `route()` on a cpp-httplib
//     `Server` bound to 127.0.0.1. NOT exercised by unit tests (no real bind in the
//     suite); it only adapts the pure handler onto the transport.
//
// ROUTING CONTRACT (AC-2/AC-3):
//   GET /healthz -> 200 if is_live(latest)  else 503   (liveness)
//   GET /ready   -> 200 if is_ready(latest) else 503   (readiness, stronger)
//   anything else (unknown path OR wrong method, e.g. POST /healthz) -> 404
//                 `{"error":"not found"}` — a clean reply, never a crash, never a leak.
// The 200/503 body is the SCRUBBED snapshot JSON; content type is application/json.
//
// SECURITY CONTRACT (AC-3): the served body is an outbound payload and is scrubbed
// (`HealthSnapshot::to_json` runs `domain::scrub`). The server binds LOCALHOST ONLY
// (127.0.0.1) — it is an out-of-band operator surface, never exposed off-box. The
// composition root is responsible for the 0600-equivalent restriction of the
// surface (loopback bind + host-level access control); this module never widens it.
//
// Cross-platform: the cpp-httplib socket code (a dependency) handles OS portability
// internally — no `#ifdef`/OS API appears in our sources. C++20 standard library.

#include <cstdint>
#include <string>
#include <string_view>

#include "broker_exec/cli/health_state.hpp"

namespace broker_exec::cli {

// A fully-formed HTTP reply as a plain value — the pure handler's output. No
// transport, no socket: status code, body, and content type only.
struct HttpReply {
  int status = 404;
  std::string body;
  std::string content_type = "application/json";
};

// Pure route handler — the unit-tested seam. Maps a method+path against the latest
// health snapshot to an HttpReply. GET /healthz -> 200/503 on liveness; GET /ready
// -> 200/503 on readiness; everything else -> 404 `{"error":"not found"}`. The
// 200/503 body is the SCRUBBED snapshot JSON. NEVER throws. `live_budget_ms` is the
// max acceptable heartbeat age (caller-owned SLO).
[[nodiscard]] HttpReply route(std::string_view method, std::string_view path,
                              const HealthState& state, std::int64_t live_budget_ms);

// Thin localhost HTTP server: registers `route()` on a cpp-httplib Server bound to
// 127.0.0.1. NOT unit-tested (no real bind in the suite) — it only adapts the pure
// handler onto the transport. cpp-httplib is confined to health_endpoint.cpp so its
// socket headers never leak into other translation units.
class HealthHttpServer {
 public:
  // Bind reads the shared `state` and serves with the given liveness budget. The
  // state must outlive the server.
  HealthHttpServer(const HealthState& state, std::int64_t live_budget_ms);
  ~HealthHttpServer();

  HealthHttpServer(const HealthHttpServer&) = delete;
  HealthHttpServer& operator=(const HealthHttpServer&) = delete;
  HealthHttpServer(HealthHttpServer&&) = delete;
  HealthHttpServer& operator=(HealthHttpServer&&) = delete;

  // Blocking listen on `host`:`port`. The composition root MUST pass a loopback host
  // (127.0.0.1) — this is an out-of-band, localhost-only operator surface (AC-3).
  // Returns true if the bind+listen succeeded. Never throws across this boundary.
  [[nodiscard]] bool listen(const std::string& host, int port);

  // Ask a running `listen()` to return (stops the server loop).
  void stop();

 private:
  // pImpl: the cpp-httplib Server is held behind a pointer so its headers stay
  // confined to the .cpp and never enter this public header.
  struct Impl;
  Impl* impl_;
};

}  // namespace broker_exec::cli
