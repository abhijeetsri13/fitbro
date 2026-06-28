#include "broker_exec/alerting/multi_channel_alert_sink.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "broker_exec/alerting/alert_channel.hpp"
#include "broker_exec/alerting/heartbeat_monitor.hpp"
#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

using broker_exec::Result;
using broker_exec::alerting::AlertChannel;
using broker_exec::alerting::HeartbeatMonitor;
using broker_exec::alerting::MultiChannelAlertSink;
using broker_exec::alerting::PostFn;
using broker_exec::clock::TestClock;
using broker_exec::errors::ErrorCategory;
using broker_exec::errors::make_error;
using broker_exec::ports::AlertLevel;
using broker_exec::ports::Ok;
using namespace std::chrono_literals;

namespace {

// A capturing POST seam: records every (url, body) and returns ok(). `fail_urls`
// lets a test mark specific urls as failing (best-effort coverage).
struct CapturingSeam {
  std::vector<std::pair<std::string, std::string>> posts;
  std::vector<std::string> fail_urls;
  bool fail_all = false;

  [[nodiscard]] Result<Ok> operator()(std::string_view url, std::string_view body) {
    posts.emplace_back(std::string(url), std::string(body));
    if (fail_all) {
      return broker_exec::fail(make_error(ErrorCategory::Network, "seam down"));
    }
    for (const std::string& f : fail_urls) {
      if (url == f) {
        return broker_exec::fail(make_error(ErrorCategory::Network, "seam down for url"));
      }
    }
    return broker_exec::ports::ok();
  }
};

[[nodiscard]] AlertChannel telegram(std::string url, std::string chat_id) {
  return AlertChannel{AlertChannel::Kind::Telegram, std::move(url), std::move(chat_id)};
}

[[nodiscard]] AlertChannel webhook(std::string url) {
  return AlertChannel{AlertChannel::Kind::Webhook, std::move(url), {}};
}

[[nodiscard]] bool body_contains(const std::string& body, std::string_view needle) {
  return body.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("MultiChannelAlertSink: delivers to every channel (AC-1)", "[alerting][deliver]") {
  TestClock clock;
  CapturingSeam capture;
  PostFn post = [&capture](std::string_view url, std::string_view body) {
    return capture(url, body);
  };

  MultiChannelAlertSink sink(post, {telegram("tg", "123"), webhook("hook")}, clock);

  const Result<Ok> r = sink.send(AlertLevel::Warning, "hello");
  REQUIRE(r);
  REQUIRE(capture.posts.size() == 2);

  bool saw_tg = false;
  bool saw_hook = false;
  for (const auto& [url, body] : capture.posts) {
    REQUIRE(body_contains(body, "hello"));
    if (url == "tg") {
      saw_tg = true;
    }
    if (url == "hook") {
      saw_hook = true;
    }
  }
  REQUIRE(saw_tg);
  REQUIRE(saw_hook);
}

TEST_CASE("MultiChannelAlertSink: send_test_alert posts to every channel (AC-1)",
          "[alerting][test-alert]") {
  TestClock clock;
  CapturingSeam capture;
  PostFn post = [&capture](std::string_view url, std::string_view body) {
    return capture(url, body);
  };

  MultiChannelAlertSink sink(post, {telegram("tg", "123"), webhook("hook")}, clock);

  const Result<Ok> r = sink.send_test_alert();
  REQUIRE(r);
  REQUIRE(capture.posts.size() == 2);

  // A fixed self-test message reached BOTH channels.
  bool saw_tg = false;
  bool saw_hook = false;
  for (const auto& [url, body] : capture.posts) {
    if (url == "tg") {
      saw_tg = true;
    }
    if (url == "hook") {
      saw_hook = true;
    }
  }
  REQUIRE(saw_tg);
  REQUIRE(saw_hook);
}

TEST_CASE("MultiChannelAlertSink: scrubs the outbound body (4.2 lesson)",
          "[alerting][redaction]") {
  TestClock clock;
  CapturingSeam capture;
  PostFn post = [&capture](std::string_view url, std::string_view body) {
    return capture(url, body);
  };

  MultiChannelAlertSink sink(post, {telegram("tg", "123"), webhook("hook")}, clock);

  // A 32-char alnum token (mixed letters+digits) that domain::scrub redacts.
  const std::string token = "ab12CD34ef56GH78ij90KL12mn34OP56";
  const std::string message = "auth failure access_token leaked " + token + " investigate";

  const Result<Ok> r = sink.send(AlertLevel::Error, message);
  REQUIRE(r);
  REQUIRE(capture.posts.size() == 2);

  // ZERO occurrences of the raw token in ANY posted body.
  for (const auto& [url, body] : capture.posts) {
    REQUIRE_FALSE(body_contains(body, token));
  }
}

TEST_CASE("MultiChannelAlertSink: best-effort delivery semantics", "[alerting][best-effort]") {
  TestClock clock;

  SECTION("one channel fails, the other succeeds -> ok()") {
    CapturingSeam capture;
    capture.fail_urls = {"tg"};
    PostFn post = [&capture](std::string_view url, std::string_view body) {
      return capture(url, body);
    };
    MultiChannelAlertSink sink(post, {telegram("tg", "123"), webhook("hook")}, clock);
    const Result<Ok> r = sink.send(AlertLevel::Warning, "hi");
    REQUIRE(r);  // at least one channel delivered
  }

  SECTION("all channels fail -> Error") {
    CapturingSeam capture;
    capture.fail_all = true;
    PostFn post = [&capture](std::string_view url, std::string_view body) {
      return capture(url, body);
    };
    MultiChannelAlertSink sink(post, {telegram("tg", "123"), webhook("hook")}, clock);
    const Result<Ok> r = sink.send(AlertLevel::Warning, "hi");
    REQUIRE_FALSE(r);
    REQUIRE(r.error().category == ErrorCategory::Network);
  }

  SECTION("no channels configured -> Error") {
    CapturingSeam capture;
    PostFn post = [&capture](std::string_view url, std::string_view body) {
      return capture(url, body);
    };
    MultiChannelAlertSink sink(post, {}, clock);
    const Result<Ok> r = sink.send(AlertLevel::Warning, "hi");
    REQUIRE_FALSE(r);
  }
}

TEST_CASE("MultiChannelAlertSink: a throwing seam is swallowed", "[alerting][no-throw]") {
  TestClock clock;
  PostFn post = [](std::string_view, std::string_view) -> Result<Ok> {
    throw std::runtime_error("boom");
  };
  MultiChannelAlertSink sink(post, {webhook("hook")}, clock);

  // The throwing seam is caught and treated as a per-channel failure: all
  // channels failed -> Error, but NO exception escapes.
  Result<Ok> r = broker_exec::ports::ok();
  REQUIRE_NOTHROW(r = sink.send(AlertLevel::Critical, "panic"));
  REQUIRE_FALSE(r);
}

TEST_CASE("HeartbeatMonitor: dead-man's-switch over the steady clock (AC-2/AC-3)",
          "[alerting][heartbeat]") {
  TestClock clock;
  HeartbeatMonitor monitor(clock);

  // Before any beat -> no sign of life.
  REQUIRE_FALSE(monitor.is_alive(2s));

  // A beat makes it alive within the gap.
  monitor.beat();
  REQUIRE(monitor.is_alive(2s));

  // Advance the steady clock past the gap with NO beat (the alerter "killed").
  clock.advance(3s);
  REQUIRE_FALSE(monitor.is_alive(2s));  // the absence alarm the external watcher fires on

  // A subsequent beat revives it.
  monitor.beat();
  REQUIRE(monitor.is_alive(2s));
}

TEST_CASE("MultiChannelAlertSink: a successful send beats the heartbeat",
          "[alerting][heartbeat]") {
  TestClock clock;
  CapturingSeam capture;
  PostFn post = [&capture](std::string_view url, std::string_view body) {
    return capture(url, body);
  };
  MultiChannelAlertSink sink(post, {webhook("hook")}, clock);

  // No delivery yet -> not alive.
  REQUIRE_FALSE(sink.is_alive(2s));

  const Result<Ok> r = sink.send(AlertLevel::Info, "alive");
  REQUIRE(r);

  // The delivered alert was itself a sign of life.
  REQUIRE(sink.is_alive(2s));
  REQUIRE(sink.heartbeat_monitor().is_alive(2s));

  // After the gap with no further beat, liveness lapses.
  clock.advance(3s);
  REQUIRE_FALSE(sink.is_alive(2s));
}

TEST_CASE("MultiChannelAlertSink: send_test_alert requires ALL channels (AC-1)",
          "[alerting][test-alert]") {
  TestClock clock;

  SECTION("one channel fails, the other succeeds -> Error (every channel must be proven)") {
    CapturingSeam capture;
    capture.fail_urls = {"tg"};  // Telegram is wired wrong; the webhook is fine.
    PostFn post = [&capture](std::string_view url, std::string_view body) {
      return capture(url, body);
    };
    MultiChannelAlertSink sink(post, {telegram("tg", "123"), webhook("hook")}, clock);

    // Unlike send() (>=1 channel ok), a self-test demands EVERY channel deliver.
    const Result<Ok> r = sink.send_test_alert();
    REQUIRE_FALSE(r.has_value());
  }

  SECTION("both channels succeed -> ok") {
    CapturingSeam capture;  // no fail_urls -> the seam accepts every url.
    PostFn post = [&capture](std::string_view url, std::string_view body) {
      return capture(url, body);
    };
    MultiChannelAlertSink sink(post, {telegram("tg", "123"), webhook("hook")}, clock);

    const Result<Ok> r = sink.send_test_alert();
    REQUIRE(r);
  }

  SECTION("no channels configured -> Error (cannot self-test with nothing wired)") {
    CapturingSeam capture;
    PostFn post = [&capture](std::string_view url, std::string_view body) {
      return capture(url, body);
    };
    MultiChannelAlertSink sink(post, {}, clock);

    const Result<Ok> r = sink.send_test_alert();
    REQUIRE_FALSE(r.has_value());
  }
}

TEST_CASE("HeartbeatMonitor: max_gap boundary is inclusive (<= off-by-one guard, AC-3)",
          "[alerting][heartbeat]") {
  TestClock clock;
  HeartbeatMonitor monitor(clock);

  monitor.beat();

  // EXACTLY at max_gap: the dead-man's-switch is still alive (the comparison is
  // inclusive, gap <= max_gap). This is the classic boundary where a `<` instead
  // of `<=` would spuriously trip the absence alarm one tick early.
  clock.advance(2000ms);
  REQUIRE(monitor.is_alive(2000ms));

  // One tick past the gap: now dead.
  clock.advance(1ms);
  REQUIRE_FALSE(monitor.is_alive(2000ms));
}

TEST_CASE("MultiChannelAlertSink: scrubs a key=value secret value (4.2 lesson)",
          "[alerting][redaction]") {
  TestClock clock;
  CapturingSeam capture;
  PostFn post = [&capture](std::string_view url, std::string_view body) {
    return capture(url, body);
  };

  MultiChannelAlertSink sink(post, {telegram("tg", "123"), webhook("hook")}, clock);

  // An all-letter value: it is caught ONLY by domain::scrub's key=value rule
  // (the sensitive `access_token` key), NOT the bare high-entropy run rule
  // (which requires >=20 chars mixing letters+digits) — so this genuinely guards
  // the key=value path rather than piggy-backing on the bare-token rule.
  const std::string secret = "SecretAccessValue";
  const std::string message = "login failed access_token=" + secret + " retrying";

  const Result<Ok> r = sink.send(AlertLevel::Error, message);
  REQUIRE(r);
  REQUIRE(capture.posts.size() == 2);

  // ZERO occurrences of the secret value in ANY posted body.
  for (const auto& [url, body] : capture.posts) {
    REQUIRE_FALSE(body_contains(body, secret));
  }
}
