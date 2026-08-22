#include "broker_exec/cli/health_snapshot.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>

#include "broker_exec/cli/health_state.hpp"
#include "broker_exec/domain/redaction.hpp"
#include "broker_exec/session/session_state.hpp"

using broker_exec::cli::fail_closed_default;
using broker_exec::cli::HealthSnapshot;
using broker_exec::cli::is_live;
using broker_exec::cli::is_ready;
using broker_exec::cli::to_json;
using broker_exec::session::SessionState;

namespace {

constexpr std::int64_t kBudgetMs = 5000;

// A fully-healthy, well-within-budget snapshot — the readiness baseline tests mutate
// one field at a time from.
[[nodiscard]] HealthSnapshot healthy_snapshot() {
  return HealthSnapshot(SessionState::Healthy, /*heartbeat_age_ms=*/100, /*tick_age_ms=*/50,
                        /*in_flight_count=*/0, /*clock_sane=*/true, /*replay_clean=*/true);
}

}  // namespace

// ── AC-2: liveness vs readiness ──────────────────────────────────────────────

TEST_CASE("is_live is true only within budget and with a sane clock", "[cli][health]") {
  CHECK(is_live(healthy_snapshot(), kBudgetMs));

  SECTION("heartbeat past the budget is not live") {
    const HealthSnapshot stale(SessionState::Healthy, kBudgetMs + 1, 0, 0, true, true);
    CHECK_FALSE(is_live(stale, kBudgetMs));
  }
  SECTION("a stalled clock is not live even within budget") {
    const HealthSnapshot insane(SessionState::Healthy, 100, 0, 0, /*clock_sane=*/false, true);
    CHECK_FALSE(is_live(insane, kBudgetMs));
  }
  SECTION("a negative budget or negative age fails closed") {
    CHECK_FALSE(is_live(healthy_snapshot(), -1));
    const HealthSnapshot negative(SessionState::Healthy, -5, 0, 0, true, true);
    CHECK_FALSE(is_live(negative, kBudgetMs));
  }
  SECTION("heartbeat exactly at the budget is still live (inclusive)") {
    const HealthSnapshot edge(SessionState::Healthy, kBudgetMs, 0, 0, true, true);
    CHECK(is_live(edge, kBudgetMs));
  }
}

TEST_CASE("is_ready requires full health and is strictly stronger than is_live", "[cli][health]") {
  CHECK(is_ready(healthy_snapshot(), kBudgetMs));

  SECTION("a non-Healthy session is live-but-not-ready") {
    const HealthSnapshot needs_reauth(SessionState::NeedsReauth, 100, 50, 0, true, true);
    CHECK(is_live(needs_reauth, kBudgetMs));         // process responds
    CHECK_FALSE(is_ready(needs_reauth, kBudgetMs));  // but not fit to trade
  }
  SECTION("a dirty replay is not ready") {
    const HealthSnapshot dirty(SessionState::Healthy, 100, 50, 0, true, /*replay_clean=*/false);
    CHECK(is_live(dirty, kBudgetMs));
    CHECK_FALSE(is_ready(dirty, kBudgetMs));
  }
  SECTION("a negative in-flight count is not ready") {
    const HealthSnapshot bad_count(SessionState::Healthy, 100, 50, /*in_flight_count=*/-1, true,
                                   true);
    CHECK(is_live(bad_count, kBudgetMs));
    CHECK_FALSE(is_ready(bad_count, kBudgetMs));
  }
  SECTION("not live implies not ready") {
    const HealthSnapshot stale(SessionState::Healthy, kBudgetMs + 1, 50, 0, true, true);
    CHECK_FALSE(is_live(stale, kBudgetMs));
    CHECK_FALSE(is_ready(stale, kBudgetMs));
  }
}

TEST_CASE("the fail-closed default is neither live nor ready for any budget", "[cli][health]") {
  const HealthSnapshot def = fail_closed_default();
  CHECK(def.session_state == SessionState::Failed);
  CHECK_FALSE(is_live(def, std::numeric_limits<std::int64_t>::max()));
  CHECK_FALSE(is_ready(def, std::numeric_limits<std::int64_t>::max()));
}

// ── AC-3: the served body is redaction-safe ─────────────────────────────────

TEST_CASE("to_json renders the documented integer-only fields", "[cli][health]") {
  const std::string body = to_json(healthy_snapshot());
  const nlohmann::json parsed = nlohmann::json::parse(body);

  CHECK(parsed.at("session_state").get<std::string>() == "Healthy");
  CHECK(parsed.at("heartbeat_age_ms").get<std::int64_t>() == 100);
  CHECK(parsed.at("tick_age_ms").get<std::int64_t>() == 50);
  CHECK(parsed.at("in_flight_count").get<int>() == 0);
  CHECK(parsed.at("clock_sane").get<bool>() == true);
  CHECK(parsed.at("replay_clean").get<bool>() == true);
  // No floating-point: the numeric fields are integers, not reals.
  CHECK(parsed.at("heartbeat_age_ms").is_number_integer());
  CHECK_FALSE(parsed.at("heartbeat_age_ms").is_number_float());
}

TEST_CASE("the served body is scrubbed — no token-shaped run survives", "[cli][health]") {
  // The struct is integer/enum-only by construction, so no field can carry a secret;
  // to_json's domain::scrub is the defense-in-depth guard on the outbound payload.
  // Proof of wiring: the rendered body is scrub-stable (already scrubbed), and a
  // token-shaped sentinel run never appears in any served body.
  const std::string body = to_json(healthy_snapshot());
  CHECK(body == broker_exec::domain::scrub(body));
  CHECK(body.find("***REDACTED***") == std::string::npos);  // nothing to redact here

  // Sanity: the scrubber the body is routed through DOES redact a token shape, so a
  // token leaking into the JSON (e.g. via a future free-form field) would be caught.
  const std::string token = "abcdef0123456789abcdef0123456789";
  CHECK(broker_exec::domain::scrub(token).find(token) == std::string::npos);
}
