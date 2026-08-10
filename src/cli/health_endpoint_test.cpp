#include "broker_exec/cli/health_endpoint.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "broker_exec/cli/health_snapshot.hpp"
#include "broker_exec/cli/health_state.hpp"
#include "broker_exec/session/session_state.hpp"

using broker_exec::cli::HealthSnapshot;
using broker_exec::cli::HealthState;
using broker_exec::cli::HttpReply;
using broker_exec::cli::route;
using broker_exec::session::SessionState;

namespace {

constexpr std::int64_t kBudgetMs = 5000;

[[nodiscard]] HealthSnapshot healthy() {
  return HealthSnapshot(SessionState::Healthy, 100, 50, 0, true, true);
}

}  // namespace

// ── AC-2: /healthz liveness, /ready readiness ───────────────────────────────

TEST_CASE("GET /healthz is 200 when live and 503 when not", "[cli][endpoint]") {
  // NB: HealthState owns a std::mutex (non-movable), so it is constructed in place
  // in each test rather than returned from a helper.
  HealthState live;
  live.publish(healthy());
  const HttpReply ok = route("GET", "/healthz", live, kBudgetMs);
  CHECK(ok.status == 200);
  CHECK(ok.content_type == "application/json");

  HealthState stalled;
  stalled.publish(HealthSnapshot(SessionState::Healthy, kBudgetMs + 1, 50, 0,
                                 /*clock_sane=*/true, true));
  CHECK(route("GET", "/healthz", stalled, kBudgetMs).status == 503);
}

TEST_CASE("GET /ready is 200 only when fully healthy", "[cli][endpoint]") {
  HealthState ready;
  ready.publish(healthy());
  CHECK(route("GET", "/ready", ready, kBudgetMs).status == 200);

  SECTION("a non-Healthy session is 503") {
    HealthState s;
    s.publish(HealthSnapshot(SessionState::NeedsReauth, 100, 50, 0, true, true));
    CHECK(route("GET", "/ready", s, kBudgetMs).status == 503);
  }
  SECTION("a dirty replay is 503") {
    HealthState s;
    s.publish(HealthSnapshot(SessionState::Healthy, 100, 50, 0, true, false));
    CHECK(route("GET", "/ready", s, kBudgetMs).status == 503);
  }
  SECTION("an insane clock is 503") {
    HealthState s;
    s.publish(HealthSnapshot(SessionState::Healthy, 100, 50, 0, false, true));
    CHECK(route("GET", "/ready", s, kBudgetMs).status == 503);
  }
}

TEST_CASE("an empty HealthState fails closed — both endpoints 503", "[cli][endpoint]") {
  const HealthState empty;  // nothing published
  CHECK(route("GET", "/healthz", empty, kBudgetMs).status == 503);
  CHECK(route("GET", "/ready", empty, kBudgetMs).status == 503);
}

TEST_CASE("/ready is strictly stronger than /healthz (live but not ready)", "[cli][endpoint]") {
  // A live-but-not-ready snapshot: heartbeat fresh + clock sane (live), but the
  // session needs reauth (not ready).
  HealthState s;
  s.publish(HealthSnapshot(SessionState::NeedsReauth, 100, 50, 0, true, true));
  CHECK(route("GET", "/healthz", s, kBudgetMs).status == 200);
  CHECK(route("GET", "/ready", s, kBudgetMs).status == 503);
}

// ── AC-3: unknown route / wrong method -> clean 404, no throw, no leak ───────

TEST_CASE("an unknown route is a clean 404", "[cli][endpoint]") {
  HealthState s;
  s.publish(healthy());
  const HttpReply reply = route("GET", "/secret", s, kBudgetMs);
  CHECK(reply.status == 404);
  CHECK(reply.content_type == "application/json");
  const nlohmann::json parsed = nlohmann::json::parse(reply.body);
  CHECK(parsed.at("error").get<std::string>() == "not found");
}

TEST_CASE("a wrong method on a known path is a 404, not a crash", "[cli][endpoint]") {
  HealthState s;
  s.publish(healthy());
  CHECK(route("POST", "/healthz", s, kBudgetMs).status == 404);
  CHECK(route("DELETE", "/ready", s, kBudgetMs).status == 404);
}

TEST_CASE("the 200/503 body is the scrubbed snapshot JSON", "[cli][endpoint]") {
  HealthState s;
  s.publish(healthy());
  const HttpReply reply = route("GET", "/healthz", s, kBudgetMs);
  // Body matches to_json (already scrubbed) and is well-formed JSON, not a leak.
  CHECK(reply.body == broker_exec::cli::to_json(s.latest()));
  CHECK_NOTHROW(nlohmann::json::parse(reply.body));
}
