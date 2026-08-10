#include "broker_exec/cli/health_endpoint.hpp"

#include <cstdint>
#include <string>
#include <string_view>

// cpp-httplib is CONFINED to this translation unit (its socket headers never enter
// any public header). The dependency's own code handles OS socket portability
// internally — no `#ifdef`/OS API appears in our sources.
#include <httplib.h>

#include "broker_exec/cli/health_snapshot.hpp"

namespace broker_exec::cli {

namespace {

constexpr int kOk = 200;
constexpr int kServiceUnavailable = 503;
constexpr int kNotFound = 404;

constexpr std::string_view kJsonContentType = "application/json";

// The clean 404 body for any unknown path or wrong method. A fixed literal — it
// carries no request echo, so it can never leak anything from the caller.
constexpr std::string_view kNotFoundBody = R"({"error":"not found"})";

// Build a 200/503 reply from a health predicate. `healthy` decides the status; the
// body is the SCRUBBED snapshot JSON either way (the operator sees WHY it is 503).
[[nodiscard]] HttpReply health_reply(bool healthy, const HealthSnapshot& snapshot) {
  HttpReply reply;
  reply.status = healthy ? kOk : kServiceUnavailable;
  reply.body = to_json(snapshot);  // already scrubbed by to_json
  reply.content_type = std::string(kJsonContentType);
  return reply;
}

[[nodiscard]] HttpReply not_found_reply() {
  HttpReply reply;
  reply.status = kNotFound;
  reply.body = std::string(kNotFoundBody);
  reply.content_type = std::string(kJsonContentType);
  return reply;
}

}  // namespace

HttpReply route(std::string_view method, std::string_view path, const HealthState& state,
                std::int64_t live_budget_ms) {
  // Only GET is a defined verb on this surface; any other method (e.g. POST
  // /healthz) is a clean 404, not a 405 — anything unexpected fails to "not found"
  // (AC-3). No throw on any path.
  if (method != "GET") {
    return not_found_reply();
  }

  const HealthSnapshot snapshot = state.latest();  // fail-closed default if unpublished

  if (path == "/healthz") {
    return health_reply(is_live(snapshot, live_budget_ms), snapshot);
  }
  if (path == "/ready") {
    return health_reply(is_ready(snapshot, live_budget_ms), snapshot);
  }
  return not_found_reply();
}

// ── Thin localhost server ────────────────────────────────────────────────────
// Adapts the pure `route()` handler onto a cpp-httplib Server. NOT unit-tested
// (no real bind in the suite). The Server lives behind pImpl so httplib headers
// stay confined here.

struct HealthHttpServer::Impl {
  const HealthState& state;
  std::int64_t live_budget_ms;
  httplib::Server server;

  Impl(const HealthState& state_in, std::int64_t budget) : state(state_in), live_budget_ms(budget) {}
};

namespace {

// Translate one pure HttpReply into the httplib response — the single adaptation
// point between our value type and the transport.
void apply_reply(const HttpReply& reply, httplib::Response& res) {
  res.status = reply.status;
  res.set_content(reply.body, reply.content_type);
}

}  // namespace

HealthHttpServer::HealthHttpServer(const HealthState& state, std::int64_t live_budget_ms)
    : impl_(new Impl(state, live_budget_ms)) {
  // Register the two GET routes by delegating to the pure handler. httplib answers
  // any other path/method with its own 404, matching route()'s contract.
  impl_->server.Get("/healthz", [this](const httplib::Request&, httplib::Response& res) {
    apply_reply(route("GET", "/healthz", impl_->state, impl_->live_budget_ms), res);
  });
  impl_->server.Get("/ready", [this](const httplib::Request&, httplib::Response& res) {
    apply_reply(route("GET", "/ready", impl_->state, impl_->live_budget_ms), res);
  });
  // Parity with route(): a live unknown-path / wrong-method request returns the
  // SAME fixed JSON 404 the pure handler does (not httplib's default text/html),
  // so the served contract is identical whether exercised in a test or on a socket.
  impl_->server.set_error_handler([](const httplib::Request&, httplib::Response& res) {
    apply_reply(not_found_reply(), res);
  });
}

HealthHttpServer::~HealthHttpServer() { delete impl_; }

bool HealthHttpServer::listen(const std::string& host, int port) {
  // LOCALHOST-ONLY CONTRACT (AC-3), ENFORCED (not merely documented): this is an
  // out-of-band operator surface that serves session state + in-flight counts, so a
  // composition-root slip ("0.0.0.0"/""/a LAN IP) must NOT silently expose it
  // off-box. Fail closed — bind ONLY a loopback host.
  if (host != "127.0.0.1" && host != "::1" && host != "localhost") {
    return false;
  }
  return impl_->server.listen(host, port);
}

void HealthHttpServer::stop() { impl_->server.stop(); }

}  // namespace broker_exec::cli
