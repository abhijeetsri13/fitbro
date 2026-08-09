#include "broker_exec/domain/redaction.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>

using broker_exec::domain::is_provenance_id_shape;
using broker_exec::domain::kMaxProvenanceIdChars;
using broker_exec::domain::kRedactionMarker;
using broker_exec::domain::scrub;
using broker_exec::domain::scrub_provenance_column;

namespace {

// A synthetic Kite-style access_token: 32 chars, mixes letters AND digits, drawn
// from [A-Za-z0-9]. Not a real credential — purely a token-SHAPED string.
constexpr std::string_view kFakeToken = "Xk29mZpQ7rTb4Lw8Nc1Vd6Ya3Hs0Ue5";

// A real-shaped client_ref EXACTLY as make_client_ref() mints it:
// `<strategy>-<8 hex signature>-<uuid>`, where <uuid> is the CANONICAL RFC-4122
// text form 8-4-4-4-12 WITH its dashes (idempotency/uuid.cpp format_uuid_v4).
// 51 chars, SEVEN alphanumeric segments, one unbroken token run mixing letters
// and digits — i.e. EXACTLY the shape the bare high-entropy rule used to redact,
// which is the whole IMP-15 defect. This is the regression baseline: it must be
// the shape the library really mints, not a simplification of it.
constexpr std::string_view kClientRef = "alpha-1a2b3c4d-deadbeef-cafe-4bab-8abe-0123456789ab";

// The dashless 32-hex-tail spelling, kept as an ADDITIONAL case: a caller may
// pass any uuid text to make_client_ref, so both shapes must behave identically.
constexpr std::string_view kDashlessClientRef = "alpha-1a2b3c4d-deadbeefcafebabe0123456789abcdef";

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

// ── IMP-15: the provenance-id shape allowlist ────────────────────────────────

TEST_CASE("scrub() alone DOES destroy a client_ref: the defect the allowlist exists for",
          "[domain][redaction][provenance]") {
  // Pinned as the regression baseline: the plain scrubber must keep treating a
  // bare 51-char token run as high-entropy. The allowlist is what exempts a
  // TYPED COLUMN; scrub() itself is deliberately not relaxed.
  CHECK(scrub(std::string(kClientRef)) == kRedactionMarker);
  CHECK(scrub(std::string(kDashlessClientRef)) == kRedactionMarker);
}

TEST_CASE("the ids this library mints and brokers send are id-shaped",
          "[domain][redaction][provenance]") {
  const std::string ref(kClientRef);

  CHECK(is_provenance_id_shape(ref));                     // make_client_ref, canonical uuid
  CHECK(is_provenance_id_shape(kDashlessClientRef));      // ... and the dashless uuid spelling
  CHECK(is_provenance_id_shape(ref + "#3"));              // slicer child `<parent>#<k>`
  CHECK(is_provenance_id_shape(ref + "#X"));              // IMP-13 exit ref `<parent>#X`
  CHECK(is_provenance_id_shape(ref + "#X" + "#X"));       // an exit of an exit is still an id
  CHECK(is_provenance_id_shape("alpha-1"));               // a short test-style ref
  CHECK(is_provenance_id_shape("KOT_123-45"));            // separators may be '_' or '-'
  CHECK(is_provenance_id_shape("-1a2b3c4d-deadbeef99"));  // an EMPTY strategy still leaves 2 segments

  // Every one of them survives the column redaction byte for byte.
  CHECK(scrub_provenance_column(ref) == ref);
  CHECK(scrub_provenance_column(ref + "#X") == ref + "#X");
  CHECK(scrub_provenance_column(ref + "#12") == ref + "#12");
  CHECK(scrub_provenance_column(kDashlessClientRef) == kDashlessClientRef);
}

TEST_CASE("a bare credential run is NOT id-shaped and stays redacted in a typed column",
          "[domain][redaction][provenance]") {
  // THE load-bearing assertion: a Kite access_token pasted whole is ONE unbroken
  // alphanumeric segment, so it fails the >=2-segment half of the structure rule
  // that every minted id passes. (Give it a separator and the HOMOGENEITY half
  // catches it instead — see the base64url case below.) The allowlist must never
  // become a way to smuggle a token through a typed column.
  CHECK_FALSE(is_provenance_id_shape(kFakeToken));
  CHECK(scrub_provenance_column(kFakeToken) == kRedactionMarker);
  CHECK(scrub_provenance_column(kFakeToken).find(kFakeToken) == std::string::npos);

  // Same for a 32-char hex blob with no structure at all.
  constexpr std::string_view bare_hex = "deadbeefcafebabe0123456789abcdef";
  CHECK_FALSE(is_provenance_id_shape(bare_hex));
  CHECK(scrub_provenance_column(bare_hex) == kRedactionMarker);
}

TEST_CASE("a SEPARATED credential is NOT id-shaped: [A-Za-z0-9_-] IS the base64url alphabet",
          "[domain][redaction][provenance]") {
  // THE HIGH-severity hole the segment COUNT alone left open. The allowlist
  // charset is exactly base64url, so a URL-safe credential carrying a single '-'
  // or '_' between alphanumerics is bounded, in-charset and has >=2 segments — it
  // satisfied every condition and was emitted IN FULL from a typed column.
  //
  // The HOMOGENEITY half of the rule closes it: a credential run mixes digits
  // with NON-HEX letters inside one segment, which no minted id ever does.
  constexpr std::array<std::string_view, 5> probes = {
      "v4Xk29mZpQ7rTb-4Lw8Nc1Vd6Ya3Hs0Ue5",        // an access_token with one '-'
      "token_Ab12Cd34Ef56Gh78Ij90Kl12",            // ... with one '_'
      "api_key-Ab12Cd34Ef56Gh78Ij90Kl12",          // ... both, and a named-secret prefix
      "SflKxwRJSMeKKF2QT4fwpM-eBHM8Zx1n_hqhVEAA",  // a JWT signature segment
      "Xk29mZpQ7rTb4Lw8Nc1Vd6Ya3Hs0Ue5-1",         // a bare token with a trailing counter
  };

  for (const std::string_view probe : probes) {
    CHECK_FALSE(is_provenance_id_shape(probe));
    // ... and the column helper therefore redacts it, whole.
    CHECK(scrub_provenance_column(probe) == kRedactionMarker);
    CHECK(scrub_provenance_column(probe).find(probe) == std::string::npos);
  }
}

TEST_CASE("DOCUMENTED RESIDUAL: an all-homogeneous value is admitted even if it is not a real id",
          "[domain][redaction][provenance]") {
  // Said out loud rather than papered over. A minted client_ref IS hex segments
  // joined by dashes, so any rule that rejects hex-with-a-separator rejects the
  // very ids this exemption exists to preserve. A HEX-ONLY credential in a typed
  // provenance column is therefore NOT protected by shape.
  constexpr std::string_view hex_with_dash = "deadbeefcafebabe-0123456789abcdef";
  CHECK(is_provenance_id_shape(hex_with_dash));
  CHECK(scrub_provenance_column(hex_with_dash) == hex_with_dash);

  // The same for an ALL-LETTER value — but that residual costs nothing new:
  // scrub() has never redacted an all-letter run either (the documented
  // limitation in redaction.cpp), so it was emitted verbatim before IMP-15 too.
  CHECK(is_provenance_id_shape("alpha-beta"));
  CHECK(scrub("alphabeta") == "alphabeta");
}

TEST_CASE("anything that is not an identifier fails the allowlist and is scrubbed",
          "[domain][redaction][provenance]") {
  // Out-of-charset bytes: a pasted key=value, URL, JSON blob, path or free text.
  CHECK_FALSE(is_provenance_id_shape("access_token=" + std::string(kFakeToken)));
  CHECK_FALSE(is_provenance_id_shape("https://api.kite.example/session?api_key=Ab12Cd34Ef56"));
  CHECK_FALSE(is_provenance_id_shape(R"({"client_ref":"alpha-1"})"));
  CHECK_FALSE(is_provenance_id_shape("alpha 1a2b3c4d"));  // a space is not an id char
  CHECK_FALSE(is_provenance_id_shape("kite.order.1"));    // '.' is not an id char
  CHECK_FALSE(is_provenance_id_shape(std::string("bad\xff\xfe")));  // invalid UTF-8 bytes

  // Bounds: empty is not provenance, and a blob is not an id however it is spelt.
  CHECK_FALSE(is_provenance_id_shape(""));
  CHECK(is_provenance_id_shape("a-" + std::string(kMaxProvenanceIdChars - 2, 'b')));
  CHECK_FALSE(is_provenance_id_shape("a-" + std::string(kMaxProvenanceIdChars - 1, 'b')));

  // A pasted credential still gets fully redacted through the column helper.
  const std::string pasted = "access_token=" + std::string(kFakeToken);
  CHECK(scrub_provenance_column(pasted).find(kFakeToken) == std::string::npos);
  CHECK(contains(scrub_provenance_column(pasted), kRedactionMarker));
}

TEST_CASE("a value that fails the allowlist is still returned legibly when it holds no secret",
          "[domain][redaction][provenance]") {
  // A Kite broker_order_id is 15 DIGITS: one segment, so not "id-shaped" — but it
  // never needed the exemption, because scrub() leaves an all-digit run alone.
  // Fail-closed here costs nothing.
  constexpr std::string_view kite_order_id = "240627000123456";
  CHECK_FALSE(is_provenance_id_shape(kite_order_id));
  CHECK(scrub_provenance_column(kite_order_id) == kite_order_id);

  // Likewise a short account code and a plain strategy name.
  CHECK(scrub_provenance_column("ZZ1234") == "ZZ1234");
  CHECK(scrub_provenance_column("alpha") == "alpha");
  CHECK(scrub_provenance_column("kite") == "kite");
}
