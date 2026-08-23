#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "broker_exec/alerting/alert_channel.hpp"
#include "broker_exec/alerting/heartbeat_monitor.hpp"
#include "broker_exec/alerting/multi_channel_alert_sink.hpp"
#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/domain/redaction.hpp"  // kRedactionMarker, for the IMP-16 assertions
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

TEST_CASE("MultiChannelAlertSink: scrubs the outbound body (4.2 lesson)", "[alerting][redaction]") {
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

TEST_CASE("MultiChannelAlertSink: a successful send beats the heartbeat", "[alerting][heartbeat]") {
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

// ── IMP-16: typed provenance on an alert ─────────────────────────────────────
//
// The defect: MultiChannelAlertSink scrubs the whole FREE-FORM body, and a minted
// client_ref is one long token-shaped run — so a caller that wrote
// `ref=<client_ref>` into the body shipped `ref=***REDACTED***`, and the
// operator's most urgent alert NAMED NO ORDER. The fix passes the ids beside the
// body in a typed ports::AlertContext, delivered through send_with_context(). The
// body's scrubbing is UNCHANGED: every message-scrubbing case asserted above still
// holds, and is re-asserted below for the send_with_context() path too.

namespace {

// A real minted client_ref, EXACTLY as make_client_ref() spells it:
// `<strategy>-<8 hex sig>-<canonical RFC-4122 v4 uuid>` (idempotency/uuid.cpp
// format_uuid_v4). This is the value that must survive end-to-end.
constexpr std::string_view kMintedRef = "alpha-1a2b3c4d-deadbeef-cafe-4bab-8abe-0123456789ab";

// A 32-char alnum token (mixes letters+digits) — what domain::scrub redacts.
constexpr std::string_view kSecretToken = "ab12CD34ef56GH78ij90KL12mn34OP56";

[[nodiscard]] broker_exec::ports::AlertContext ctx(std::string client_ref,
                                                   std::string broker_order_id,
                                                   std::string strategy, std::string symbol = {}) {
  broker_exec::ports::AlertContext out;
  out.client_ref = std::move(client_ref);
  out.broker_order_id = std::move(broker_order_id);
  out.strategy = std::move(strategy);
  out.symbol = std::move(symbol);
  return out;
}

}  // namespace

TEST_CASE("IMP-16: a REAL minted client_ref survives an alert end-to-end",
          "[alerting][provenance]") {
  TestClock clock;
  CapturingSeam capture;
  PostFn post = [&capture](std::string_view url, std::string_view body) {
    return capture(url, body);
  };
  MultiChannelAlertSink sink(post, {telegram("tg", "123"), webhook("hook")}, clock);

  const std::string message = "UNKNOWN order has no authoritative broker match (fail-closed)";
  const Result<Ok> r = sink.send_with_context(
      AlertLevel::Critical, message, ctx(std::string(kMintedRef), "240627000123456", "alpha"));
  REQUIRE(r);
  REQUIRE(capture.posts.size() == 2);

  for (const auto& [url, body] : capture.posts) {
    static_cast<void>(url);
    // The ref reaches EVERY channel intact, in the structured, greppable block.
    REQUIRE(body_contains(body, kMintedRef));
    REQUIRE(body_contains(body,
                          "[client_ref=alpha-1a2b3c4d-deadbeef-cafe-4bab-8abe-0123456789ab"
                          " broker_order_id=240627000123456 strategy=alpha]"));
    REQUIRE(body_contains(body, message));  // the free-form body is intact alongside it
  }

  // THE BASELINE THIS FIXES, pinned: the SAME ref interpolated into the free-form
  // body is STILL destroyed. The exemption reaches typed columns only, never the
  // body — a substring exemption there would blunt the bare high-entropy rule for
  // every alert ever sent (IMP-15's HIGH finding).
  CapturingSeam legacy;
  PostFn legacy_post = [&legacy](std::string_view url, std::string_view body) {
    return legacy(url, body);
  };
  MultiChannelAlertSink legacy_sink(legacy_post, {webhook("hook")}, clock);
  REQUIRE(legacy_sink.send(AlertLevel::Critical, message + " ref=" + std::string(kMintedRef)));
  REQUIRE(legacy.posts.size() == 1);
  CHECK_FALSE(body_contains(legacy.posts.front().second, kMintedRef));
  CHECK(body_contains(legacy.posts.front().second, broker_exec::domain::kRedactionMarker));
}

TEST_CASE("IMP-16: a token-shaped value in a CONTEXT field is still REDACTED",
          "[alerting][provenance][redaction]") {
  TestClock clock;
  CapturingSeam capture;
  PostFn post = [&capture](std::string_view url, std::string_view body) {
    return capture(url, body);
  };
  MultiChannelAlertSink sink(post, {telegram("tg", "123"), webhook("hook")}, clock);

  // A credential parked in EVERY id column. The typed block is not an escape
  // hatch: a value that is not id-shaped takes the ordinary scrub (fail closed).
  const std::string token(kSecretToken);
  const Result<Ok> r =
      sink.send_with_context(AlertLevel::Error, "auth failure", ctx(token, token, token));
  REQUIRE(r);
  REQUIRE(capture.posts.size() == 2);
  for (const auto& [url, body] : capture.posts) {
    static_cast<void>(url);
    REQUIRE_FALSE(body_contains(body, kSecretToken));
    REQUIRE(body_contains(body, broker_exec::domain::kRedactionMarker));
  }
}

TEST_CASE("IMP-16: the FREE-FORM body's scrubbing is UNCHANGED on the provenance path",
          "[alerting][provenance][redaction]") {
  TestClock clock;
  CapturingSeam capture;
  PostFn post = [&capture](std::string_view url, std::string_view body) {
    return capture(url, body);
  };
  MultiChannelAlertSink sink(post, {telegram("tg", "123"), webhook("hook")}, clock);

  // BOTH message-scrubbing cases asserted for send() above, re-run through
  // send_with_context() with a valid provenance context present: supplying
  // provenance must not relax the body by one byte.
  const std::string bare_token_body =
      "auth failure access_token leaked " + std::string(kSecretToken) + " investigate";
  const std::string secret = "SecretAccessValue";  // caught ONLY by the key=value rule
  const std::string kv_body = "login failed access_token=" + secret + " retrying";

  REQUIRE(sink.send_with_context(AlertLevel::Error, bare_token_body,
                                 ctx(std::string(kMintedRef), "", "alpha")));
  REQUIRE(sink.send_with_context(AlertLevel::Error, kv_body,
                                 ctx(std::string(kMintedRef), "", "alpha")));
  REQUIRE(capture.posts.size() == 4);

  for (const auto& [url, body] : capture.posts) {
    static_cast<void>(url);
    REQUIRE_FALSE(body_contains(body, kSecretToken));  // bare high-entropy rule intact
    REQUIRE_FALSE(body_contains(body, secret));        // key=value rule intact
    REQUIRE(body_contains(body, kMintedRef));          // ...and the ref still survives
  }
}

TEST_CASE("IMP-16: an all-empty context is byte-identical to a plain send()",
          "[alerting][provenance]") {
  TestClock clock;
  CapturingSeam two_arg;
  CapturingSeam three_arg;
  PostFn post2 = [&two_arg](std::string_view url, std::string_view body) {
    return two_arg(url, body);
  };
  PostFn post3 = [&three_arg](std::string_view url, std::string_view body) {
    return three_arg(url, body);
  };
  MultiChannelAlertSink sink2(post2, {webhook("hook")}, clock);
  MultiChannelAlertSink sink3(post3, {webhook("hook")}, clock);

  const std::string message = "recovery: could not load state";
  REQUIRE(sink2.send(AlertLevel::Warning, message));
  REQUIRE(
      sink3.send_with_context(AlertLevel::Warning, message, broker_exec::ports::AlertContext{}));

  REQUIRE(two_arg.posts.size() == 1);
  REQUIRE(three_arg.posts.size() == 1);
  // No block, no trailing space, no bracket — the exact same outbound bytes.
  CHECK(two_arg.posts.front().second == three_arg.posts.front().second);
  CHECK_FALSE(body_contains(three_arg.posts.front().second, "["));
}

TEST_CASE("IMP-16: a BROKER-supplied order id cannot forge a provenance field",
          "[alerting][provenance][redaction]") {
  TestClock clock;
  CapturingSeam capture;
  PostFn post = [&capture](std::string_view url, std::string_view body) {
    return capture(url, body);
  };
  MultiChannelAlertSink sink(post, {webhook("hook")}, clock);

  // broker_order_id is BROKER-CONTROLLED. An id that spells its own way out of the
  // block would let the broker attribute an alert to an order of ITS choosing — a
  // WRONG id, which is worse than a missing one. It is redacted wholesale.
  const std::string forged = "1] client_ref=" + std::string(kMintedRef);
  REQUIRE(sink.send_with_context(AlertLevel::Critical, "order rejected", ctx("", forged, "")));
  REQUIRE(capture.posts.size() == 1);

  const std::string& body = capture.posts.front().second;
  CHECK_FALSE(body_contains(body, kMintedRef));
  CHECK(body_contains(body, broker_exec::domain::kRedactionMarker));
}

TEST_CASE("IMP-16: a minted CHILD slice ref survives a real sink verbatim",
          "[alerting][provenance]") {
  // THE SLICER'S ALERT, END TO END. options::execute_sliced_leg escalates with the
  // CHILD ref of the slice that went UNKNOWN — `<minted parent>#<k>` — and that is
  // the ref an operator must reconcile against the broker. sliced_leg_test.cpp
  // proves the caller hands it over as a typed column, but its SpyAlertSink does
  // not scrub, so only this test (a REAL MultiChannelAlertSink, whose send path
  // runs domain::scrub over the body) proves the ref actually survives delivery.
  TestClock clock;
  CapturingSeam capture;
  PostFn post = [&capture](std::string_view url, std::string_view body) {
    return capture(url, body);
  };
  MultiChannelAlertSink sink(post, {telegram("tg", "123"), webhook("hook")}, clock);

  const std::string child_ref = std::string(kMintedRef) + "#2";
  const std::string message = "sliced leg PAUSED at child #2 (Unknown) — no further slice sent";
  REQUIRE(sink.send_with_context(AlertLevel::Critical, message,
                                 ctx(child_ref, "", "alpha", "NIFTY24JUN24000CE")));
  REQUIRE(capture.posts.size() == 2);

  for (const auto& [url, body] : capture.posts) {
    static_cast<void>(url);
    REQUIRE(body_contains(body, child_ref));  // the '#' suffix included, byte for byte
    REQUIRE(body_contains(
        body, "[client_ref=" + child_ref + " strategy=alpha symbol=NIFTY24JUN24000CE]"));
  }

  // THE BASELINE: the same child ref in the free-form body is still destroyed.
  CapturingSeam legacy;
  PostFn legacy_post = [&legacy](std::string_view url, std::string_view body) {
    return legacy(url, body);
  };
  MultiChannelAlertSink legacy_sink(legacy_post, {webhook("hook")}, clock);
  REQUIRE(legacy_sink.send(AlertLevel::Critical, message + " ref=" + child_ref));
  REQUIRE(legacy.posts.size() == 1);
  CHECK_FALSE(body_contains(legacy.posts.front().second, child_ref));
}

TEST_CASE("IMP-16/M4: the INSTRUMENT SYMBOL survives an alert end-to-end",
          "[alerting][provenance][symbol]") {
  // THE DEFECT THIS CLOSES: `symbol` was interpolated into the free-form body by
  // the protection, corporate-action and manual-intervention paths, and scrub()
  // redacts any >=20-char run mixing letters and digits. NIFTY (17 chars) survived
  // by luck; BANKNIFTY / FINNIFTY / MIDCPNIFTY option symbols (20-22) did NOT — so
  // "your position is unprotected" named no instrument for exactly the contracts
  // this library trades.
  TestClock clock;
  CapturingSeam capture;
  PostFn post = [&capture](std::string_view url, std::string_view body) {
    return capture(url, body);
  };
  MultiChannelAlertSink sink(post, {webhook("hook")}, clock);

  const std::string banknifty = "BANKNIFTY24JUN52000CE";
  const std::string message =
      "PROTECTION: re-arming protective Sell exit — stop trigger crossed but "
      "protective order did NOT fill";
  REQUIRE(sink.send_with_context(AlertLevel::Critical, message, ctx("", "", "", banknifty)));
  REQUIRE(capture.posts.size() == 1);
  CHECK(body_contains(capture.posts.front().second, "[symbol=BANKNIFTY24JUN52000CE]"));

  // THE BASELINE, pinned: the same symbol inside the body is still redacted, so the
  // typed column is doing the work and the body's rule is untouched.
  CapturingSeam legacy;
  PostFn legacy_post = [&legacy](std::string_view url, std::string_view body) {
    return legacy(url, body);
  };
  MultiChannelAlertSink legacy_sink(legacy_post, {webhook("hook")}, clock);
  REQUIRE(legacy_sink.send(AlertLevel::Critical, "PROTECTION: " + banknifty + " unprotected"));
  REQUIRE(legacy.posts.size() == 1);
  CHECK_FALSE(body_contains(legacy.posts.front().second, banknifty));
  CHECK(body_contains(legacy.posts.front().second, broker_exec::domain::kRedactionMarker));

  // A credential parked in the SYMBOL column is still redacted (fail closed): the
  // symbol rule is TIGHTER than the id rule, not an escape hatch.
  CapturingSeam token_capture;
  PostFn token_post = [&token_capture](std::string_view url, std::string_view body) {
    return token_capture(url, body);
  };
  MultiChannelAlertSink token_sink(token_post, {webhook("hook")}, clock);
  REQUIRE(token_sink.send_with_context(AlertLevel::Error, "auth failure",
                                       ctx("", "", "", std::string(kSecretToken))));
  REQUIRE(token_capture.posts.size() == 1);
  CHECK_FALSE(body_contains(token_capture.posts.front().second, kSecretToken));
  CHECK(body_contains(token_capture.posts.front().second, broker_exec::domain::kRedactionMarker));
}

TEST_CASE("IMP-16: a NON-ASCII broker order id cannot append prose to an alert",
          "[alerting][provenance][redaction]") {
  // The end-to-end form of the allowlist finding. Without it, U+00A0 (a space the
  // guard never named) and U+2028 (a line separator Telegram and most log viewers
  // DO render as a break) let a broker-supplied order id append arbitrary
  // multi-word, multi-line prose to a Critical alert — one alert rendered as two.
  TestClock clock;
  CapturingSeam capture;
  PostFn post = [&capture](std::string_view url, std::string_view body) {
    return capture(url, body);
  };
  MultiChannelAlertSink sink(post, {telegram("tg", "123")}, clock);

  const std::string forged =
      std::string("1") + "\xc2\xa0" + "RESOLVED" + "\xe2\x80\xa8" + "INFO: ignore previous";
  REQUIRE(sink.send_with_context(AlertLevel::Critical,
                                 "UNKNOWN order has no authoritative broker match",
                                 ctx("", forged, "")));
  REQUIRE(capture.posts.size() == 1);

  const std::string& body = capture.posts.front().second;
  CHECK_FALSE(body_contains(body, "RESOLVED"));
  CHECK_FALSE(body_contains(body, "ignore previous"));
  CHECK(body_contains(
      body, "[broker_order_id=" + std::string(broker_exec::domain::kRedactionMarker) + "]"));
}

TEST_CASE("IMP-16: an ENORMOUS numeric broker order id cannot suppress a Critical alert",
          "[alerting][provenance][redaction]") {
  // Broker order ids are NUMERIC, and scrub() leaves an all-digit run of any length
  // alone — so before the bound a hostile id produced a multi-kilobyte block.
  // Telegram's sendMessage caps text at 4096 chars: the POST fails, and on a
  // Telegram-ONLY deployment deliver() then returns Network on every channel, so
  // the CRITICAL alert never reaches the operator. The bound keeps the block small
  // enough that delivery is never the broker's to decide.
  TestClock clock;
  CapturingSeam capture;
  PostFn post = [&capture](std::string_view url, std::string_view body) {
    return capture(url, body);
  };
  MultiChannelAlertSink sink(post, {telegram("tg", "123")}, clock);

  const std::string message = "UNKNOWN order has no authoritative broker match";
  const std::string huge(5000, '9');
  const Result<Ok> r = sink.send_with_context(AlertLevel::Critical, message, ctx("", huge, ""));
  REQUIRE(r);  // the alert is still deliverable...
  REQUIRE(capture.posts.size() == 1);

  const std::string& body = capture.posts.front().second;
  CHECK_FALSE(body_contains(body, huge));
  CHECK(body_contains(body, message));  // ...and the operator still gets the WARNING
  // Comfortably inside Telegram's 4096-char limit — the broker cannot inflate it.
  CHECK(body.size() < std::size_t{4096});
}

TEST_CASE("IMP-16: send() still works for a stub that overrides only send()",
          "[alerting][provenance][compat]") {
  // The port's default send_with_context() delegates to send(), so every
  // pre-existing stub (~40 of them across the repo) keeps compiling and behaving
  // identically. Proven with a sink that overrides ONLY send(). Note there is no
  // `using AlertSink::send;` here and none is needed: send_with_context is a
  // DISTINCT NAME, so it hides nothing and trips no -Woverloaded-virtual.
  class LegacyStub final : public broker_exec::ports::AlertSink {
   public:
    Result<Ok> send(AlertLevel level, const std::string& message) override {
      level_ = level;
      message_ = message;
      ++count_;
      return broker_exec::ports::ok();
    }
    Result<Ok> send_test_alert() override { return broker_exec::ports::ok(); }

    [[nodiscard]] std::size_t count() const noexcept { return count_; }
    [[nodiscard]] AlertLevel level() const noexcept { return level_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

   private:
    std::size_t count_ = 0;
    AlertLevel level_ = AlertLevel::Info;
    std::string message_;
  };

  LegacyStub stub;
  broker_exec::ports::AlertSink& sink = stub;  // how every production call site holds it

  REQUIRE(sink.send(AlertLevel::Warning, "plain"));
  CHECK(stub.count() == 1);
  CHECK(stub.message() == "plain");

  // A send_with_context call through the abstract reference reaches the SAME
  // override with the message BYTE-IDENTICAL: the context is dropped (never
  // leaked, never spliced into the body) — the fail-closed direction for a sink
  // that opted out.
  REQUIRE(sink.send_with_context(AlertLevel::Critical, "plain",
                                 ctx(std::string(kMintedRef), "B-1", "alpha")));
  CHECK(stub.count() == 2);
  CHECK(stub.level() == AlertLevel::Critical);
  CHECK(stub.message() == "plain");
}
