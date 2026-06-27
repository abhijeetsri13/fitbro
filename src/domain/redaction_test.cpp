#include "broker_exec/domain/redaction.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using broker_exec::domain::kRedactionMarker;
using broker_exec::domain::scrub;

namespace {

// A synthetic Kite-style access_token: 32 chars, mixes letters AND digits, drawn
// from [A-Za-z0-9]. Not a real credential — purely a token-SHAPED string.
constexpr std::string_view kFakeToken = "Xk29mZpQ7rTb4Lw8Nc1Vd6Ya3Hs0Ue5";

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

// A token-shaped run survives if any >=20-char window of [A-Za-z0-9_-] mixing a
// letter and a digit remains. After scrubbing there must be none.
[[nodiscard]] bool has_token_shaped_run(std::string_view s) {
  const auto is_tok = [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
  };
  std::size_t i = 0;
  while (i < s.size()) {
    if (!is_tok(s[i])) {
      ++i;
      continue;
    }
    std::size_t j = i;
    bool letter = false;
    bool digit = false;
    while (j < s.size() && is_tok(s[j])) {
      letter = letter || (s[j] >= 'A' && s[j] <= 'Z') || (s[j] >= 'a' && s[j] <= 'z');
      digit = digit || (s[j] >= '0' && s[j] <= '9');
      ++j;
    }
    if (j - i >= 20 && letter && digit) {
      return true;
    }
    i = j;
  }
  return false;
}

}  // namespace

TEST_CASE("a synthetic access_token is scrubbed from a log line, an exception, and a JSON record",
          "[domain][redaction]") {
  const std::string log_line = "2026-06-27T10:00:00Z level=info event=login access_token=" +
                               std::string(kFakeToken) + " account=acct-1";
  const std::string exception_text =
      "OAuth handshake failed: access_token=" + std::string(kFakeToken) + " was rejected";
  const std::string json_record = R"({"client_ref":"abc-123","op":"login","access_token":")" +
                                  std::string(kFakeToken) + R"("})";

  for (const std::string& sink : {log_line, exception_text, json_record}) {
    const std::string scrubbed = scrub(sink);
    // The raw token must not survive in any sink.
    CHECK_FALSE(contains(scrubbed, kFakeToken));
    // And no token-shaped run may remain at all.
    CHECK_FALSE(has_token_shaped_run(scrubbed));
    // The marker is present where the token was.
    CHECK(contains(scrubbed, kRedactionMarker));
  }
}

TEST_CASE("scrub is idempotent", "[domain][redaction]") {
  const std::string sink = R"({"access_token":")" + std::string(kFakeToken) +
                           R"(","mpin":"4321","api_key":"pub123key456abc789xyz"})";
  const std::string once = scrub(sink);
  const std::string twice = scrub(once);
  CHECK(once == twice);
  CHECK_FALSE(contains(once, kFakeToken));
}

TEST_CASE("named-secret key=value, URL params, and MPIN context are redacted", "[domain][redaction]") {
  // key: value (log-shaped) and key=value (URL/query) forms.
  CHECK(contains(scrub("password=hunter2 next"), kRedactionMarker));
  CHECK_FALSE(contains(scrub("password=hunter2 next"), "hunter2"));

  const std::string url = "https://api.kite.example/session?api_key=Ab12Cd34Ef56&v=3";
  const std::string scrubbed_url = scrub(url);
  CHECK_FALSE(contains(scrubbed_url, "Ab12Cd34Ef56"));
  CHECK(contains(scrubbed_url, kRedactionMarker));
  CHECK(contains(scrubbed_url, "v=3"));  // ordinary param left intact

  // MPIN/TOTP digit run in an auth context (no separator).
  CHECK(contains(scrub("user entered MPIN 4321 at the prompt"), kRedactionMarker));
  CHECK_FALSE(contains(scrub("user entered MPIN 4321 at the prompt"), "4321"));
}

TEST_CASE("ordinary prose, ids, and timestamps are returned unchanged", "[domain][redaction]") {
  // Plain words, a short mixed id (<20), a pure-numeric order id and a 13-digit
  // epoch timestamp — none of these are secret-shaped.
  const std::string ordinary =
      "Order ORD123 for 50 shares of INFY at price 1450 placed at 1700000000000 status filled";
  CHECK(scrub(ordinary) == ordinary);

  // A bare 5-digit number with no auth keyword nearby must not be touched.
  const std::string with_id = "request id 12345 completed in 250 ms on port 8080";
  CHECK(scrub(with_id) == with_id);

  // A long, all-letters identifier (no digit) is not high-entropy enough.
  const std::string words = "supercalifragilisticexpialidocious is a very long word indeed";
  CHECK(scrub(words) == words);
}
