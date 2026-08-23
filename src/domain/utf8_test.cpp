#include "broker_exec/domain/utf8.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>

#include "broker_exec/domain/redaction.hpp"

// IMP-17: canonical_text() was PROMOTED out of src/ledger/ledger.cpp into
// `domain` so the ledger and the intent log — two independent hash chains —
// share ONE implementation instead of a copy each. These are the tests of the
// function ITSELF; the per-consumer behaviour (append/verify/reload) stays in
// ledger_test.cpp and intent_log_test.cpp.
//
// Every hex escape below is followed by a SPACE, a non-hex character, or a
// string-literal break, because a C++ hex escape is GREEDY: "\xE2" "f" written as
// "\xE2f" is ONE escape, not two characters.

namespace {

using broker_exec::domain::canonical_text;
using broker_exec::domain::kUtf8Replacement;

constexpr std::string_view kFffd = "\xEF\xBF\xBD";

}  // namespace

TEST_CASE("canonical_text: valid UTF-8 is a FIXED POINT (P1)", "[domain][utf8]") {
  // ASCII, including the bytes redaction cares about.
  CHECK(canonical_text("") == "");
  CHECK(canonical_text("plain ascii payload") == "plain ascii payload");
  CHECK(canonical_text("token=abc-123_XYZ key: value") == "token=abc-123_XYZ key: value");

  // Well-formed multi-byte at every length: U+00E9 (2), U+20B9 RUPEE (3),
  // U+1F600 (4). Not one byte may move — this is what keeps every hash written
  // before IMP-17 valid.
  CHECK(canonical_text("caf\xC3\xA9") == "caf\xC3\xA9");
  CHECK(canonical_text("net \xE2\x82\xB9 125000") == "net \xE2\x82\xB9 125000");
  CHECK(canonical_text("emoji \xF0\x9F\x98\x80 end") == "emoji \xF0\x9F\x98\x80 end");

  // U+FFFD itself is valid UTF-8, so it survives verbatim (this is what makes
  // re-canonicalising a canonical string a no-op).
  CHECK(canonical_text(kFffd) == kFffd);
  CHECK(kUtf8Replacement == kFffd);
}

TEST_CASE("canonical_text: one U+FFFD per MAXIMAL SUBPART, spelled out", "[domain][utf8]") {
  // A byte that can never lead: a bare continuation, and 0xFF.
  CHECK(canonical_text("lone \x80 byte") == std::string("lone ") + std::string(kFffd) + " byte");
  CHECK(canonical_text("invalid \xFF byte") ==
        std::string("invalid ") + std::string(kFffd) + " byte");

  // A TRUNCATED run is ONE subpart -> ONE replacement, not one per byte.
  CHECK(canonical_text("truncated \xE2\x82 run") ==
        std::string("truncated ") + std::string(kFffd) + " run");
  CHECK(canonical_text("\xF0\x9F\x98") == kFffd);

  // Two independent bad bytes are two subparts.
  CHECK(canonical_text("\x80\x80") == std::string(kFffd) + std::string(kFffd));

  // THE RE-READ RULE, which is what makes the semantics match nlohmann's: a byte
  // that cannot CONTINUE the sequence in progress is re-read as a FRESH LEAD, so
  // a truncated rupee sign followed by a whole one yields U+FFFD + the rupee,
  // NOT two replacements.
  CHECK(canonical_text("\xE2\x82"
                       "\xE2\x82\xB9") == std::string(kFffd) + "\xE2\x82\xB9");
  // Same rule with an ASCII byte breaking the run: the 'A' is preserved.
  CHECK(canonical_text("\xE2\x82"
                       "A") == std::string(kFffd) + "A");

  // The four tightened second-byte ranges: overlong 2-byte, overlong 3-byte,
  // a UTF-16 surrogate, and a code point above U+10FFFF. All ill-formed.
  CHECK(canonical_text("\xC0\xAF") == std::string(kFffd) + std::string(kFffd));  // overlong '/'
  CHECK(canonical_text("\xE0\x80\xAF") ==
        std::string(kFffd) + std::string(kFffd) + std::string(kFffd));
  CHECK(canonical_text("\xED\xA0\x80") ==  // U+D800, a lone surrogate
        std::string(kFffd) + std::string(kFffd) + std::string(kFffd));
  CHECK(canonical_text("\xF4\x90\x80\x80") ==  // > U+10FFFF
        std::string(kFffd) + std::string(kFffd) + std::string(kFffd) + std::string(kFffd));
}

TEST_CASE("canonical_text: IDEMPOTENT, and the output is always valid UTF-8 (P2/P3)",
          "[domain][utf8]") {
  const std::string_view inputs[] = {
      "",
      "plain ascii",
      "lone \x80 byte",
      "invalid \xFF byte",
      "trunc \xE2\x82 run",
      "\xC0\xAF",
      "\xED\xA0\x80",
      "net \xE2\x82\xB9 125000",
      "emoji \xF0\x9F\x98\x80",
      "\xEF\xBF\xBD",
  };
  for (const std::string_view in : inputs) {
    const std::string once = canonical_text(in);
    CHECK(canonical_text(once) == once);  // P3: a second pass changes nothing

    // P2, asserted structurally: no byte of the output can be an ill-formed lead
    // or a stray continuation — every sequence in it round-trips through the
    // decoder unchanged, which is exactly what "a later dump() is a no-op" means.
    CHECK(canonical_text(std::string(once) + once) == std::string(once) + once);

    // P5: it never shrinks.
    CHECK(once.size() >= in.size());
  }
}

TEST_CASE("canonical_text: the ASCII subsequence is preserved EXACTLY (P4)", "[domain][utf8]") {
  // No ASCII byte is created, destroyed or reordered; U+FFFD is three bytes that
  // are ALL >= 0x80, so it can never be mistaken for one.
  const auto ascii_only = [](std::string_view s) {
    std::string out;
    for (const char c : s) {
      if (static_cast<unsigned char>(c) < 0x80U) {
        out.push_back(c);
      }
    }
    return out;
  };
  const std::string_view inputs[] = {
      "mpin \x80\x80 1234",
      "passwordtokentotp\xC0\x80"
      "12345678",
      "a\x80"
      "b\xFF"
      "c\xE2\x82"
      "d",
      "\xE2\x82\xB9 100",
  };
  for (const std::string_view in : inputs) {
    CHECK(ascii_only(canonical_text(in)) == ascii_only(in));
  }
}

TEST_CASE("canonical_text vs scrub: the TOKEN-SHAPED rules are order-invariant", "[domain][utf8]") {
  using broker_exec::domain::kRedactionMarker;
  using broker_exec::domain::scrub;

  // A token run is a maximal run of ASCII [A-Za-z0-9_-]. By P4/P5 normalisation
  // can neither JOIN two runs nor SPLIT one, so `key=value` and the >=20-char
  // high-entropy rule fire on exactly the same runs either way round.
  const std::string_view inputs[] = {
      "\x80 order token=Xy8ZqA1bCd2eFg3hIj4k",
      "api_key=\xFF"
      "secret-value more",
      "\xE2\x82 abcdefghij0123456789ABC tail",
      "prefix token: \x80\x80 aaaaaaaaaa1111111111 suffix",
  };
  for (const std::string_view in : inputs) {
    const std::string scrub_first = canonical_text(scrub(in));
    const std::string norm_first = scrub(canonical_text(in));
    CHECK(scrub_first == norm_first);
    CHECK(scrub_first.find(kRedactionMarker) != std::string::npos);
  }
}

TEST_CASE("canonical_text vs scrub: the AUTH-WINDOW rule is ORDER-SENSITIVE — both directions",
          "[domain][utf8][redaction]") {
  using broker_exec::domain::kRedactionMarker;
  using broker_exec::domain::scrub;

  // THIS IS THE REASON THE ORDER IS FIXED, and it is the claim the original
  // IMP-17 comment got wrong. canonical_text is NOT length-preserving (P5): one
  // ill-formed byte becomes THREE. domain::auth_context_before() looks back a
  // FIXED 10-BYTE window for an auth keyword, so the expansion MOVES the keyword
  // relative to that window and can flip the decision — IN EITHER DIRECTION.

  // (1) Shipped order (scrub on the RAW bytes) redacts; normalise-first does not.
  //     "mpin " (5) + 2 bad bytes + " " (1) puts the '1' at offset 8, so the
  //     10-byte lookbehind is s[0..7] == "mpin \x80\x80 " and still contains
  //     "mpin". Normalised, the two U+FFFDs occupy SIX bytes: the '1' moves to
  //     offset 12, the window becomes s[2..11] and "mpin" has fallen out of it.
  {
    constexpr std::string_view kMpin = "mpin \x80\x80 1234";
    const std::string scrub_first = canonical_text(scrub(kMpin));
    const std::string norm_first = scrub(canonical_text(kMpin));
    CHECK(scrub_first != norm_first);
    CHECK(scrub_first.find(kRedactionMarker) != std::string::npos);  // the PIN is destroyed
    CHECK(scrub_first.find("1234") == std::string::npos);
    CHECK(norm_first.find(kRedactionMarker) == std::string::npos);  // ...and would LEAK
    CHECK(norm_first.find("1234") != std::string::npos);
  }

  // (2) And the OPPOSITE, so nobody mistakes (1) for "scrub-first always redacts
  //     more": here the shipped order redacts LESS. "totp" is embedded inside
  //     "passwordtokentotp", so the whole-word check rejects it on the raw bytes;
  //     after normalisation the expansion separates it and it matches.
  {
    constexpr std::string_view kEmbedded =
        "passwordtokentotp\xC0\x80"
        "12345678";
    const std::string scrub_first = canonical_text(scrub(kEmbedded));
    const std::string norm_first = scrub(canonical_text(kEmbedded));
    CHECK(scrub_first != norm_first);
    CHECK(scrub_first.find("12345678") != std::string::npos);  // leaks under scrub-first
    CHECK(norm_first.find("12345678") == std::string::npos);
    CHECK(norm_first.find(kRedactionMarker) != std::string::npos);
  }

  // The consequence, stated as an assertion rather than a comment: because the
  // two orders are NOT interchangeable, scrub() must keep seeing the RAW bytes
  // (which is what every existing redaction test pins) and canonicalisation must
  // stay LAST. Both consumers do exactly that.
}
