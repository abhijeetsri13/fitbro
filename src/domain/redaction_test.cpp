#include "broker_exec/domain/redaction.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>

using broker_exec::domain::explain_invalid_strategy_name;
using broker_exec::domain::is_instrument_symbol_shape;
using broker_exec::domain::is_provenance_id_shape;
using broker_exec::domain::is_valid_strategy_name;
using broker_exec::domain::kMaxInstrumentSymbolChars;
using broker_exec::domain::kMaxProvenanceIdChars;
using broker_exec::domain::kMaxStrategyNameChars;
using broker_exec::domain::kRedactionMarker;
using broker_exec::domain::ProvenanceField;
using broker_exec::domain::render_provenance_block;
using broker_exec::domain::scrub;
using broker_exec::domain::scrub_provenance_column;
using broker_exec::domain::scrub_symbol_column;

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

// "abc" + the given RAW BYTES + "def". Built byte-by-byte rather than as a string
// literal on purpose: `"\xa8" "def"` is a lexing hazard (\x is greedy over hex
// digits and d/e/f ARE hex digits), and these probes must be exact.
[[nodiscard]] std::string wrap_bytes(std::initializer_list<unsigned char> bytes) {
  std::string s = "abc";
  for (const unsigned char b : bytes) {
    s.push_back(static_cast<char>(b));
  }
  s += "def";
  return s;
}

// The whole block a single redacted column renders to.
[[nodiscard]] std::string redacted_block(std::string_view key) {
  return " [" + std::string(key) + "=" + std::string(kRedactionMarker) + "]";
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
  const std::string log_line =
      "2026-06-27T10:00:00Z level=info event=login access_token=" + std::string(kFakeToken) +
      " account=acct-1";
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

TEST_CASE("named-secret key=value, URL params, and MPIN context are redacted",
          "[domain][redaction]") {
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

  CHECK(is_provenance_id_shape(ref));                 // make_client_ref, canonical uuid
  CHECK(is_provenance_id_shape(kDashlessClientRef));  // ... and the dashless uuid spelling
  CHECK(is_provenance_id_shape(ref + "#3"));          // slicer child `<parent>#<k>`
  CHECK(is_provenance_id_shape(ref + "#X"));          // IMP-13 exit ref `<parent>#X`
  CHECK(is_provenance_id_shape(ref + "#X" + "#X"));   // an exit of an exit is still an id
  CHECK(is_provenance_id_shape("alpha-1"));           // a short test-style ref
  CHECK(is_provenance_id_shape("KOT_123-45"));        // separators may be '_' or '-'
  CHECK(
      is_provenance_id_shape("-1a2b3c4d-deadbeef99"));  // an EMPTY strategy still leaves 2 segments

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
  CHECK_FALSE(is_provenance_id_shape("alpha 1a2b3c4d"));            // a space is not an id char
  CHECK_FALSE(is_provenance_id_shape("kite.order.1"));              // '.' is not an id char
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

// ── IMP-16: the shared ` [k=v k=v]` renderer for FREE-FORM bodies ─────────────

TEST_CASE("render_provenance_block: an empty context renders NOTHING (the non-breaking hinge)",
          "[domain][redaction][provenance][imp16]") {
  // This is what makes IMP-16 additive everywhere: `body + block` must be
  // BYTE-IDENTICAL to `body` when no provenance is supplied. The ledger leans on
  // it directly — an empty context must hash exactly like a plain append.
  CHECK(render_provenance_block({}).empty());
  CHECK(render_provenance_block({{"client_ref", ""}, {"broker_order_id", ""}}).empty());

  const std::string body = "UNKNOWN order has no authoritative broker match";
  CHECK(body + render_provenance_block({{"client_ref", ""}}) == body);
}

TEST_CASE("render_provenance_block: a real minted client_ref survives verbatim, empties omitted",
          "[domain][redaction][provenance][imp16]") {
  // The exact defect IMP-16 fixes: this ref through the FREE-FORM path is
  // destroyed (asserted just below), but as a TYPED COLUMN it reaches the operator.
  const std::string block = render_provenance_block({
      {"client_ref", kClientRef},
      {"broker_order_id", "240627000123456"},  // a Kite 15-digit id
      {"strategy", "alpha"},
  });
  CHECK(block ==
        " [client_ref=alpha-1a2b3c4d-deadbeef-cafe-4bab-8abe-0123456789ab"
        " broker_order_id=240627000123456 strategy=alpha]");

  // ...whereas the same ref inside a free-form body is still redacted. The two
  // paths stay separate; the body's rule is not weakened by the block existing.
  CHECK_FALSE(contains(scrub("ref=" + std::string(kClientRef)), kClientRef));

  // An empty column is omitted entirely — never a dangling `key=`.
  CHECK(render_provenance_block({{"client_ref", kClientRef}, {"broker_order_id", ""}}) ==
        " [client_ref=alpha-1a2b3c4d-deadbeef-cafe-4bab-8abe-0123456789ab]");
  CHECK(render_provenance_block({{"client_ref", ""}, {"strategy", "alpha"}}) ==
        " [strategy=alpha]");
}

TEST_CASE("render_provenance_block: a token in an id column is STILL redacted (fail closed)",
          "[domain][redaction][provenance][imp16]") {
  // The block is NOT an escape hatch. A credential parked in a typed column takes
  // the ordinary scrub, exactly as scrub_provenance_column promises.
  const std::string block = render_provenance_block({{"client_ref", kFakeToken}});
  CHECK_FALSE(contains(block, kFakeToken));
  CHECK(contains(block, kRedactionMarker));
  CHECK_FALSE(has_token_shaped_run(block));

  // A pasted key=value blob in an id column likewise.
  const std::string pasted = "access_token=" + std::string(kFakeToken);
  const std::string blob_block = render_provenance_block({{"broker_order_id", pasted}});
  CHECK_FALSE(contains(blob_block, kFakeToken));
  CHECK(contains(blob_block, kRedactionMarker));
}

TEST_CASE("render_provenance_block: the block grammar is UNFORGEABLE by a broker-supplied id",
          "[domain][redaction][provenance][imp16]") {
  // broker_order_id is BROKER-CONTROLLED. Without the structural guard, an id that
  // spells its own way out of the block would let the BROKER forge a client_ref in
  // an operator alert — a WRONG id, which is worse than a missing one.
  const std::string forged = "1] client_ref=" + std::string(kClientRef);
  const std::string block = render_provenance_block({{"broker_order_id", forged}});
  CHECK(block == " [broker_order_id=" + std::string(kRedactionMarker) + "]");
  // Exactly one '[' and one ']' — no second, forged field could be spliced in.
  CHECK(block.find_first_of('[') == block.find_last_of('['));
  CHECK(block.find_first_of(']') == block.find_last_of(']'));

  // Every structural byte is rejected wholesale, including control bytes that
  // would split the block when an operator greps a log line.
  constexpr std::array<std::string_view, 5> structural = {
      "abc]def",   // would terminate the block early
      "abc[def",   // would open a second block
      "abc=def",   // would forge a key boundary
      "abc def",   // would forge a field boundary
      "abc\ndef",  // would split the line entirely
  };
  for (const std::string_view probe : structural) {
    CHECK(render_provenance_block({{"client_ref", probe}}) ==
          " [client_ref=" + std::string(kRedactionMarker) + "]");
  }

  // The guard costs nothing legitimate: no id-shaped value can contain any of
  // those bytes (they are outside the allowlist charset), so the minted ref and
  // the marker itself both pass through untouched.
  CHECK(contains(render_provenance_block({{"client_ref", kClientRef}}), kClientRef));
  CHECK(
      contains(render_provenance_block({{"client_ref", kDashlessClientRef}}), kDashlessClientRef));
}

// ── The block guard is an ALLOWLIST, not a denylist ──────────────────────────

TEST_CASE("render_provenance_block: NON-ASCII bytes cannot substitute for the blocked ASCII",
          "[domain][redaction][provenance][imp16]") {
  // THE DEFECT: the guard used to be a DENYLIST of four ASCII classes (c <= 0x20,
  // '[', ']', '='), and scrub() passes any byte it does not recognise through
  // UNCHANGED — so every byte >= 0x7F reached the operator verbatim. Unicode has a
  // substitute for each blocked ASCII byte, and `broker_order_id` is
  // BROKER-CONTROLLED. The probes below are exactly those substitutes.
  //
  // The array above this one enumerates only the four classes the old guard
  // checked, so it could not possibly have caught this: it was written from the
  // guard's shape rather than from the attacker's.
  struct Probe {
    std::string bytes;
    std::string what;
  };
  const std::array<Probe, 6> non_ascii = {{
      {wrap_bytes({0xE2, 0x80, 0xA8}), "U+2028 LINE SEPARATOR — a newline the guard did not name"},
      {wrap_bytes({0xC2, 0xA0}), "U+00A0 NO-BREAK SPACE — a field separator"},
      {wrap_bytes({0xE2, 0x80, 0xAE}), "U+202E RTL OVERRIDE — visually reverses the run"},
      {wrap_bytes({0xEF, 0xBC, 0xBD}), "U+FF3D FULLWIDTH ']' — a visual block terminator"},
      {wrap_bytes({0x7F}), "DEL 0x7F — a C1-adjacent control byte above the old <= 0x20 test"},
      {wrap_bytes({0xFF}), "a raw 0xFF — not even valid UTF-8"},
  }};

  for (const Probe& probe : non_ascii) {
    INFO(probe.what);
    CHECK(render_provenance_block({{"broker_order_id", probe.bytes}}) ==
          redacted_block("broker_order_id"));
    // ...and not one byte of the probe survives anywhere in the block.
    CHECK_FALSE(contains(render_provenance_block({{"broker_order_id", probe.bytes}}), probe.bytes));
  }

  // THE COMPOSED ATTACK, end to end: NBSP for the spaces and U+2028 for the line
  // break let a broker-supplied order id append arbitrary multi-word, multi-line
  // PROSE to a Critical alert body — one alert rendered as two.
  std::string forged_prose = "1";
  for (const char c : std::string_view("\xc2\xa0")) {  // U+00A0
    forged_prose.push_back(c);
  }
  forged_prose += "RESOLVED";
  for (const char c : std::string_view("\xe2\x80\xa8")) {  // U+2028
    forged_prose.push_back(c);
  }
  forged_prose += "INFO";
  CHECK(render_provenance_block({{"broker_order_id", forged_prose}}) ==
        redacted_block("broker_order_id"));
}

TEST_CASE("render_provenance_block: the LENGTH BOUND covers the scrub fallback too",
          "[domain][redaction][provenance][imp16]") {
  // THE DEFECT: kMaxProvenanceIdChars bounded only is_provenance_id_shape's
  // verbatim branch. The scrub FALLBACK was unbounded, and scrub() redacts only
  // runs mixing letters AND digits — so an all-digit or all-letter value of ANY
  // length was emitted in full. BROKER ORDER IDS ARE NUMERIC.
  //
  // WHY IT MATTERS BEYOND TIDINESS: a 5000-digit id yielded a ~5019-byte block,
  // which overruns Telegram's 4096-char sendMessage cap. The POST fails, and on a
  // Telegram-only deployment deliver() then returns Network on every channel — so
  // the CRITICAL alert never reaches the operator at all. That is broker-controlled
  // suppression of the fail-closed escalation path, driven purely by value LENGTH.
  const std::string huge_digits(5000, '9');   // the realistic shape: a numeric order id
  const std::string huge_letters(5000, 'z');  // the all-letter twin scrub also ignores

  CHECK(scrub(huge_digits) == huge_digits);    // pinned: scrub() itself does NOT bound these
  CHECK(scrub(huge_letters) == huge_letters);  // (unchanged behaviour — the block bounds them)

  CHECK(render_provenance_block({{"broker_order_id", huge_digits}}) ==
        redacted_block("broker_order_id"));
  CHECK(render_provenance_block({{"broker_order_id", huge_letters}}) ==
        redacted_block("broker_order_id"));

  // The boundary itself: kMaxProvenanceIdChars is admitted, one more is not.
  const std::string at_bound(kMaxProvenanceIdChars, '7');
  const std::string over_bound(kMaxProvenanceIdChars + 1, '7');
  CHECK(render_provenance_block({{"broker_order_id", at_bound}}) ==
        " [broker_order_id=" + at_bound + "]");
  CHECK(render_provenance_block({{"broker_order_id", over_bound}}) ==
        redacted_block("broker_order_id"));

  // A LEGITIMATE long id still renders: the id-shaped 128-char value the shape
  // rule already admits is not collateral damage of the new bound.
  const std::string long_id = "a-" + std::string(kMaxProvenanceIdChars - 2, 'b');
  REQUIRE(is_provenance_id_shape(long_id));
  CHECK(render_provenance_block({{"client_ref", long_id}}) == " [client_ref=" + long_id + "]");

  // ...and a real Kite 15-digit order id is untouched by any of this.
  CHECK(render_provenance_block({{"broker_order_id", "240627000123456"}}) ==
        " [broker_order_id=240627000123456]");
}

TEST_CASE("render_provenance_block: a KEY that could break the grammar drops its field",
          "[domain][redaction][provenance][imp16]") {
  // Keys are first-party literals today, but this is public API and the key is a
  // caller-supplied view — so it takes the same charset allowlist as a value. A
  // field whose key cannot be rendered safely cannot ATTRIBUTE its value, so the
  // whole field is dropped rather than emitted under a mangled name.
  CHECK(render_provenance_block({{"bad key", kClientRef}}).empty());
  CHECK(render_provenance_block({{"a=b", kClientRef}}).empty());
  CHECK(render_provenance_block({{"a]b", kClientRef}}).empty());
  CHECK(render_provenance_block({{"", kClientRef}}).empty());

  // A bad key does not take its GOOD neighbours down with it, and the surviving
  // block is still well formed (exactly one '[' and one ']').
  const std::string block = render_provenance_block({
      {"bad key", kClientRef},
      {"client_ref", kClientRef},
  });
  CHECK(block == " [client_ref=" + std::string(kClientRef) + "]");
  CHECK(block.find_first_of('[') == block.find_last_of('['));
  CHECK(block.find_first_of(']') == block.find_last_of(']'));
}

// ── IMP-16 / M4: the INSTRUMENT SYMBOL is a separate shape ───────────────────

TEST_CASE("scrub() DESTROYS the very option symbols this library trades: the M4 defect",
          "[domain][redaction][symbol]") {
  // The regression baseline, pinned before the fix is asserted. scrub()'s bare
  // high-entropy rule redacts any >=20-char run mixing letters and digits, and an
  // index-option symbol is exactly that. NIFTY survived only by being 17 chars —
  // pure luck of length, not a property anyone chose.
  CHECK(scrub("NIFTY24JUN24000CE") == "NIFTY24JUN24000CE");    // 17 — survives
  CHECK(scrub("FINNIFTY24JUN23000CE") == kRedactionMarker);    // 20 — destroyed
  CHECK(scrub("BANKNIFTY24JUN52000CE") == kRedactionMarker);   // 21 — destroyed
  CHECK(scrub("MIDCPNIFTY24JUN12000CE") == kRedactionMarker);  // 22 — destroyed

  // ...and the ID rule cannot rescue them: a symbol is ONE heterogeneous segment,
  // so it fails the homogeneity half and falls straight back to scrub().
  CHECK_FALSE(is_provenance_id_shape("BANKNIFTY24JUN52000CE"));
  CHECK(scrub_provenance_column("BANKNIFTY24JUN52000CE") == kRedactionMarker);
}

TEST_CASE("is_instrument_symbol_shape admits real symbols and nothing credential-shaped",
          "[domain][redaction][symbol]") {
  constexpr std::array<std::string_view, 6> symbols = {
      "NIFTY24JUN24000CE",
      "FINNIFTY24JUN23000CE",
      "BANKNIFTY24JUN52000CE",
      "MIDCPNIFTY24JUN12000CE",
      "INFY",
      "SENSEX24JUN80000PE",
  };
  for (const std::string_view symbol : symbols) {
    INFO(std::string(symbol));
    CHECK(is_instrument_symbol_shape(symbol));
    CHECK(scrub_symbol_column(symbol) == symbol);  // verbatim through the typed column
  }

  // The rule is TIGHTER than the id rule, not looser — that is what makes it safe.
  // A credential mixes CASE; an exchange symbol never does.
  CHECK_FALSE(is_instrument_symbol_shape(kFakeToken));
  CHECK(scrub_symbol_column(kFakeToken) == kRedactionMarker);
  CHECK_FALSE(is_instrument_symbol_shape("nifty24jun24000ce"));  // lowercase
  CHECK_FALSE(is_instrument_symbol_shape("NIFTY 24JUN"));        // a space
  CHECK_FALSE(is_instrument_symbol_shape("api_key=ABC123"));     // a pasted key=value
  CHECK_FALSE(is_instrument_symbol_shape(""));                   // empty is not a symbol

  // Bounded, like every other column rule.
  CHECK(is_instrument_symbol_shape(std::string(kMaxInstrumentSymbolChars, 'A')));
  CHECK_FALSE(is_instrument_symbol_shape(std::string(kMaxInstrumentSymbolChars + 1, 'A')));
}

TEST_CASE("render_provenance_block: a Symbol field needs Kind::Symbol — the Id rule destroys it",
          "[domain][redaction][symbol][imp16]") {
  constexpr std::string_view banknifty = "BANKNIFTY24JUN52000CE";

  // WITH the symbol shape: the operator learns which instrument is naked.
  CHECK(render_provenance_block({{"symbol", banknifty, ProvenanceField::Kind::Symbol}}) ==
        " [symbol=BANKNIFTY24JUN52000CE]");

  // WITHOUT it (the default Id kind): redacted. This is why Kind exists at all —
  // a plain column add would have shipped `symbol=***REDACTED***` and looked fixed.
  CHECK(render_provenance_block({{"symbol", banknifty}}) == redacted_block("symbol"));

  // Symbol and id columns compose in one block, each under its own rule, and a
  // token parked in the symbol column is STILL redacted (fail closed).
  CHECK(render_provenance_block({
            {"client_ref", kClientRef},
            {"symbol", banknifty, ProvenanceField::Kind::Symbol},
        }) == " [client_ref=" + std::string(kClientRef) + " symbol=BANKNIFTY24JUN52000CE]");
  CHECK(render_provenance_block({{"symbol", kFakeToken, ProvenanceField::Kind::Symbol}}) ==
        redacted_block("symbol"));

  // An empty symbol is omitted like any other empty column (the non-breaking hinge).
  CHECK(render_provenance_block({{"symbol", "", ProvenanceField::Kind::Symbol}}).empty());
}

// ── M1: the `strategy` column's cost, PINNED as a decision ───────────────────

TEST_CASE("DOCUMENTED, PINNED: an ordinary strategy name is WHOLLY REDACTED in every block",
          "[domain][redaction][provenance][strategy]") {
  // `strategy` is caller-supplied and NOTHING in this library constrains its
  // charset (idempotency::make_client_ref concatenates it as-is). These are the
  // consequences, asserted so they are a DECISION and not an accident. The fix
  // belongs at the boundary where a strategy name enters — NOT in the block, where
  // admitting a space would hand a broker-controlled broker_order_id the field
  // separator. See the `strategy` note in redaction.hpp.

  // (1) A SPACE. "iron condor v2" is an entirely ordinary options strategy name.
  CHECK(render_provenance_block({{"strategy", "iron condor v2"}}) == redacted_block("strategy"));

  // (2) A HETEROGENEOUS name — subtler and worse, because it silently poisons the
  // CLIENT_REF too. make_client_ref mints `<strategy>-<sig8>-<uuid>`, and
  // is_provenance_id_shape requires every segment to be homogeneous. "S1" is one
  // letter plus one digit, so a strategy named "S1" makes EVERY ref it mints
  // non-id-shaped — the ref then falls back to scrub(), which sees one 48-char run
  // mixing letters and digits and destroys it. The order becomes unnameable.
  constexpr std::string_view s1_ref = "S1-1a2b3c4d-deadbeef-cafe-4bab-8abe-0123456789ab";
  CHECK_FALSE(is_provenance_id_shape(s1_ref));
  CHECK(render_provenance_block({{"client_ref", s1_ref}}) == redacted_block("client_ref"));

  // (3) The homogeneous spelling of the same name works perfectly. An all-letter
  // ("alpha") or all-digit strategy segment keeps the whole ref id-shaped — this is
  // the constraint the boundary should enforce.
  CHECK(render_provenance_block({{"strategy", "alpha"}}) == " [strategy=alpha]");
  CHECK(render_provenance_block({{"client_ref", kClientRef}}) ==
        " [client_ref=" + std::string(kClientRef) + "]");

  // (4) IMP-19: the boundary now EXISTS, and it is exactly these names it refuses.
  // The block's behaviour above is unchanged — nothing here was loosened; what
  // changed is that config::load() and the safe-start gate no longer let a name
  // that produces the outcomes above reach a trading session.
  CHECK_FALSE(is_valid_strategy_name("iron condor v2"));
  CHECK_FALSE(is_valid_strategy_name("S1"));
  CHECK(is_valid_strategy_name("alpha"));
  CHECK(is_valid_strategy_name("S-1"));
}

// ── IMP-19: is_valid_strategy_name — the INPUT side of the id shape ───────────

TEST_CASE("IMP-19: an accepted strategy name survives its own typed column",
          "[domain][redaction][provenance][strategy][IMP-19]") {
  // V6 stated as a property: every accepted name is emitted VERBATIM from a
  // `strategy=` column. (That the same name also mints an id-shaped client_ref is
  // proven against the REAL make_client_ref in idempotency_test.cpp — a predicate
  // checked only against itself would prove nothing.)
  // In order: the common all-letter case; the documented fix for "S1"; the
  // documented fix for "momentum-v2"; letters and digits each in their own
  // segment; '_' separating just as '-' does; uppercase; a run that is all-hex AND
  // all-letters; all-digits (a subset of all-hex); all-hex mixing letters and
  // digits inside ONE segment; a single character; a 19-char hex run (just under
  // scrub's 20-char high-entropy rule); and 21 chars in two all-letter segments.
  const std::array<std::string_view, 12> accepted = {
      "alpha",
      "S-1",
      "momentum-v-2",
      "atm-straddle-9-20",
      "IRON_CONDOR",
      "ATM",
      "deadbeef",
      "12345",
      "1a2b3c4d",
      "x",
      "abcdef0123456789abc",
      "conservative-momentum",
  };
  for (const std::string_view name : accepted) {
    INFO("name = " << name);
    CHECK(is_valid_strategy_name(name));
    CHECK(explain_invalid_strategy_name(name).empty());
    CHECK(scrub_provenance_column(name) == name);
    const std::string expected = " [strategy=" + std::string(name) + "]";
    CHECK(render_provenance_block({{"strategy", name}}) == expected);
  }
}

TEST_CASE("IMP-19: a rejected strategy name is named, with the rule it broke",
          "[domain][redaction][provenance][strategy][IMP-19]") {
  // V1 — empty.
  CHECK_FALSE(is_valid_strategy_name(""));
  CHECK(contains(explain_invalid_strategy_name(""), "EMPTY"));

  // V1, second half — ATTRIBUTABLE. A name of nothing but separators satisfies
  // every OTHER rule in the list: it is in charset, its (zero) segments are all
  // vacuously homogeneous, and it survives its own typed column untouched — the
  // ref `---1a2b3c4d-deadbeef-...` really is id-shaped. It is refused for the same
  // reason "" is: `strategy=---` attributes an order to nothing at all. Rejecting
  // one and accepting the other would have made V1's stated rationale a fiction.
  for (const std::string_view unattributable : {"-", "_", "---", "___", "-_-", "__--__"}) {
    INFO("name = " << unattributable);
    CHECK_FALSE(is_valid_strategy_name(unattributable));
    CHECK(contains(explain_invalid_strategy_name(unattributable), "no letter or digit"));
    // ...and it is refused for THAT reason, not by accident of another rule.
    CHECK(scrub_provenance_column(unattributable) == unattributable);
  }
  // One alphanumeric byte anywhere is enough — the rule is attribution, not shape.
  CHECK(is_valid_strategy_name("-a"));
  CHECK(is_valid_strategy_name("a-"));
  CHECK(is_valid_strategy_name("_9_"));

  // V2 — bounded, so `<name>-<sig8>-<uuid>` (+ a `#<k>` child) stays inside
  // kMaxProvenanceIdChars. One under the limit passes; one over is refused.
  const std::string at_limit(kMaxStrategyNameChars, 'a');
  const std::string over_limit(kMaxStrategyNameChars + 1, 'a');
  const std::string bound = std::to_string(kMaxStrategyNameChars);
  CHECK(is_valid_strategy_name(at_limit));
  CHECK_FALSE(is_valid_strategy_name(over_limit));
  CHECK(contains(explain_invalid_strategy_name(over_limit), bound));
  // The message ECHOES a bounded, sanitised name — never the raw one. An 8 KiB
  // name must not become an 8 KiB alert (the unbounded-block hazard).
  const std::string huge(8192, 'a');
  CHECK(explain_invalid_strategy_name(huge).size() < std::size_t{512});

  // V3 — valid UTF-8 (canonical_text's notion). A lone continuation byte and a
  // truncated two-byte lead are both ill-formed.
  CHECK_FALSE(is_valid_strategy_name(std::string("alpha\x80")));
  CHECK(contains(explain_invalid_strategy_name(std::string("alpha\x80")), "not valid UTF-8"));
  CHECK_FALSE(is_valid_strategy_name(std::string("\xC3(")));

  // V4 — charset. A space is the "iron condor v2" case; '#' is excluded even
  // though the id charset admits it, because idempotency's child-ref parser keys
  // on it (proven in idempotency_test.cpp).
  for (const std::string_view bad : {"iron condor v2", "a.b", "a/b", "a:b", "a=b", "a#b"}) {
    INFO("name = " << bad);
    CHECK_FALSE(is_valid_strategy_name(bad));
    CHECK(contains(explain_invalid_strategy_name(bad), "[A-Za-z0-9_-]"));
  }
  // The offending byte is NEVER echoed raw: it is shown as '?', so an ill-formed
  // or hostile name cannot forge a provenance block or inject a line break.
  const std::string spaced = explain_invalid_strategy_name("iron condor v2");
  CHECK(contains(spaced, "iron?condor?v2"));
  CHECK_FALSE(contains(spaced, "iron condor v2"));

  // V5 — segment homogeneity, THE rule that makes the minted ref id-shaped. The
  // message names the SEGMENT and suggests a spelling that is actually accepted.
  const std::string s1 = explain_invalid_strategy_name("S1");
  CHECK_FALSE(is_valid_strategy_name("S1"));
  CHECK(contains(s1, "segment 'S1'"));
  CHECK(contains(s1, "mixes letters and digits"));
  CHECK(contains(s1, "use 'S-1'"));
  CHECK(is_valid_strategy_name("S-1"));

  // Only the OFFENDING segment is named, and only it is rewritten in the
  // suggestion — the valid part of the name is left exactly as the operator wrote it.
  const std::string mv2 = explain_invalid_strategy_name("momentum-v2");
  CHECK_FALSE(is_valid_strategy_name("momentum-v2"));
  CHECK(contains(mv2, "segment 'v2'"));
  CHECK(contains(mv2, "use 'momentum-v-2'"));
  CHECK(is_valid_strategy_name("momentum-v-2"));

  // V6 — a name that clears V1-V5 can STILL be destroyed as its own column: a
  // single un-separated 20-char hex run is what scrub() cannot tell from a
  // credential. One '-' fixes it. (19 chars is accepted above.)
  constexpr std::string_view hex20 = "abcdef0123456789abcd";
  CHECK(is_provenance_id_shape(std::string(hex20) + "-1a2b3c4d"));  // 2 segments: fine
  CHECK_FALSE(is_valid_strategy_name(hex20));
  CHECK(contains(explain_invalid_strategy_name(hex20), "credential"));
  CHECK(is_valid_strategy_name("abcdef0123-456789abcd"));
}

TEST_CASE("IMP-19: the strategy rule does NOT touch is_provenance_id_shape",
          "[domain][redaction][provenance][strategy][IMP-19]") {
  // The alternative fix — exempting the FIRST segment from homogeneity so that
  // `momentum-v2` would be admitted — was REJECTED, and this pins why: the
  // credential guarantee is stated over the WHOLE value, and the strategy slot is
  // not one segment but as many as the operator writes, so a "first segment only"
  // exemption would not even have admitted `momentum-v2` (`v2` is the SECOND
  // segment) while still weakening rule 3b. Nothing below changed.
  // In order: a credential as one mixed run; a base64url credential carrying a
  // '-'; a heterogeneous FIRST segment; a heterogeneous SECOND segment (which is
  // where `momentum-v2`'s offending segment actually lands); and the real minted
  // ref, which still passes.
  CHECK_FALSE(is_provenance_id_shape(kFakeToken));
  CHECK_FALSE(is_provenance_id_shape("Xk29mZpQ7rTb-4Lw8Nc1Vd6Ya"));
  CHECK_FALSE(is_provenance_id_shape("S1-1a2b3c4d-deadbeef"));
  CHECK_FALSE(is_provenance_id_shape("momentum-v2-1a2b3c4d"));
  CHECK(is_provenance_id_shape(kClientRef));
}

TEST_CASE("IMP-19: why TAIL-ANCHORING was rejected, and why the OLD reason was wrong",
          "[domain][redaction][provenance][strategy][IMP-19]") {
  // The version of the relaxation that WOULD have worked is tail-anchoring:
  // recognise the minted `-<8 hex>-<8-4-4-4-12 hex>` tail and relax the head in
  // front of it. Both halves of the rationale in redaction.hpp are executable, so
  // pin them here rather than leaving them as prose nobody can check.
  //
  // (1) THE REASON ORIGINALLY GIVEN WAS FALSE for its own flagship example. The
  //     claim was "the bare `strategy=` column would be destroyed anyway". It is
  //     not: scrub()'s bare high-entropy rule needs >=20 token chars mixing
  //     letters and digits, and these names are far shorter. They render TODAY.
  CHECK(scrub("momentum-v2") == "momentum-v2");
  CHECK(scrub("iron-condor-v2") == "iron-condor-v2");
  CHECK(scrub("bnf-15m") == "bnf-15m");
  // The claim only becomes true at length: 29 chars mixing letters and digits.
  CHECK(contains(scrub("momentum-breakout-v2-intraday"), kRedactionMarker));

  // (2) THE REASON THAT IS REAL: no safe head rule exists at this altitude. The
  //     worked counterexample — a 16-char mixed-alnum credential head (the first
  //     half of kFakeToken) followed by a perfectly well-formed sig8 and UUID.
  constexpr std::string_view kForgedTail =
      "Xk29mZpQ7rTb4Lw8-1a2b3c4d-deadbeef-cafe-4bab-8abe-0123456789ab";
  // Even the CAREFUL head rule ("the head must itself survive scrub()") admits it,
  // because 16 < 20 — so tail-anchoring would emit the whole thing VERBATIM from a
  // typed column, including from the broker-controlled `broker_order_id`.
  CHECK(scrub("Xk29mZpQ7rTb4Lw8") == "Xk29mZpQ7rTb4Lw8");
  // The rule as shipped rejects it, and the block redacts it wholesale.
  CHECK_FALSE(is_provenance_id_shape(kForgedTail));
  CHECK(render_provenance_block({{"broker_order_id", kForgedTail}}) ==
        " [broker_order_id=" + std::string(kRedactionMarker) + "]");
}
