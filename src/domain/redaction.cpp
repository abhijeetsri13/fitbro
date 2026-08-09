#include "broker_exec/domain/redaction.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace broker_exec::domain {

namespace {

// ── character classes ─────────────────────────────────────────────────────────

[[nodiscard]] char to_lower_ascii(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool is_letter(char c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

[[nodiscard]] bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// A hex digit in EITHER case. Used only by the provenance-id shape rule: every
// id this library mints is built from hex runs (a sha256 sig8, a v4 UUID's five
// groups), so "all hex" is a homogeneity class an id satisfies and a base64url
// credential run essentially never does.
[[nodiscard]] bool is_hex_digit(char c) noexcept {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// A "token char": the alphabet of base64url / hex / opaque-credential runs and
// of `key` names. Deliberately excludes '.' so dotted paths (host names, dotted
// config keys) split into separate words at boundaries.
[[nodiscard]] bool is_token_char(char c) noexcept {
  return is_letter(c) || is_digit(c) || c == '_' || c == '-';
}

[[nodiscard]] bool is_alnum(char c) noexcept { return is_letter(c) || is_digit(c); }

// The provenance-id charset: the token chars PLUS '#', which is what joins a
// slicer child (`<parent>#<k>`) and an IMP-13 square-off exit (`<parent>#X`) to
// its parent. Deliberately no '.', '=', '/', ':' or space — those mark a URL,
// a `key=value` pair or a pasted blob, none of which is an identifier.
[[nodiscard]] bool is_provenance_id_char(char c) noexcept {
  return is_token_char(c) || c == '#';
}

[[nodiscard]] bool is_space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

// Where an unquoted `key=value` value ends. Covers log fields (space), URL query
// params (&), JSON/object punctuation, and list/CSV separators.
[[nodiscard]] bool is_value_delim(char c) noexcept {
  switch (c) {
    case ' ':
    case '\t':
    case '\r':
    case '\n':
    case '\f':
    case '\v':
    case '&':
    case ',':
    case ';':
    case '"':
    case '\'':
    case '}':
    case ']':
    case ')':
    case '<':
    case '>':
    case '|':
      return true;
    default:
      return false;
  }
}

// ── sensitive-key detection ───────────────────────────────────────────────────
//
// Lowercase needles. "token" subsumes access_token/enc_token/request_token/
// public_token; the api_key/api-key/apikey trio is spelled out because the
// separators differ. Mirrors the config loader's secret denylist intent.
constexpr std::array<std::string_view, 9> kSensitiveKeyNeedles = {
    "token", "secret", "password", "api_key", "api-key",
    "apikey", "mpin",   "totp",     "bearer"};

[[nodiscard]] bool key_is_sensitive(std::string_view key) noexcept {
  for (std::string_view needle : kSensitiveKeyNeedles) {
    if (needle.size() > key.size()) {
      continue;
    }
    const std::size_t last = key.size() - needle.size();
    for (std::size_t i = 0; i <= last; ++i) {
      bool match = true;
      for (std::size_t j = 0; j < needle.size(); ++j) {
        if (to_lower_ascii(key[i + j]) != needle[j]) {
          match = false;
          break;
        }
      }
      if (match) {
        return true;
      }
    }
  }
  return false;
}

// Auth-context keywords for the bare MPIN/TOTP digit-run rule: a short digit run
// is only redacted when one of these appears just before it.
constexpr std::array<std::string_view, 4> kAuthContextNeedles = {"mpin", "totp", "otp", "pin"};

// True if any auth keyword appears in the small window of text immediately
// preceding `pos` (case-insensitive). Window kept tight (10 chars) so distant,
// innocuous words ("spinning ... 12345") do not trigger a redaction.
[[nodiscard]] bool auth_context_before(std::string_view s, std::size_t pos) noexcept {
  constexpr std::size_t kWindow = 10;
  const std::size_t start = pos > kWindow ? pos - kWindow : 0;
  const std::string_view window = s.substr(start, pos - start);
  for (std::string_view needle : kAuthContextNeedles) {
    if (needle.size() > window.size()) {
      continue;
    }
    const std::size_t last = window.size() - needle.size();
    for (std::size_t i = 0; i <= last; ++i) {
      bool match = true;
      for (std::size_t j = 0; j < needle.size(); ++j) {
        if (to_lower_ascii(window[i + j]) != needle[j]) {
          match = false;
          break;
        }
      }
      // Whole-word only: a letter immediately before/after the needle means it is
      // embedded in a larger word ("ping", "spinning"), not a standalone keyword.
      if (match && (i == 0 || !is_letter(window[i - 1])) &&
          (i + needle.size() == window.size() || !is_letter(window[i + needle.size()]))) {
        return true;
      }
    }
  }
  return false;
}

[[nodiscard]] bool run_has_letter_and_digit(std::string_view s, std::size_t begin,
                                            std::size_t end) noexcept {
  bool letter = false;
  bool digit = false;
  for (std::size_t i = begin; i < end; ++i) {
    letter = letter || is_letter(s[i]);
    digit = digit || is_digit(s[i]);
  }
  return letter && digit;
}

[[nodiscard]] bool run_is_all_digits(std::string_view s, std::size_t begin,
                                     std::size_t end) noexcept {
  for (std::size_t i = begin; i < end; ++i) {
    if (!is_digit(s[i])) {
      return false;
    }
  }
  return end > begin;
}

constexpr std::size_t kNoMatch = static_cast<std::size_t>(-1);

// Try to match a sensitive `key=value` / `key: value` / `"key":"value"` pair
// whose key run begins at `i`. On a match, append the verbatim key+separator
// region and a single marker for the (non-empty) value to `out`, and return the
// index just past the value (the closing quote, if any, is left for the main
// loop to emit). Returns kNoMatch when this is not a sensitive key=value.
[[nodiscard]] std::size_t try_key_value(std::string_view s, std::size_t i, std::string& out) {
  const std::size_t n = s.size();

  std::size_t k = i;
  while (k < n && is_token_char(s[k])) {
    ++k;
  }
  if (k == i || !key_is_sensitive(s.substr(i, k - i))) {
    return kNoMatch;
  }

  std::size_t cur = k;
  if (cur < n && (s[cur] == '"' || s[cur] == '\'')) {  // closing quote of a quoted key
    ++cur;
  }
  while (cur < n && is_space(s[cur])) {
    ++cur;
  }
  if (cur >= n || (s[cur] != ':' && s[cur] != '=')) {
    return kNoMatch;  // a token run that merely contains a needle, not a key=value
  }
  ++cur;  // consume the separator
  while (cur < n && is_space(s[cur])) {
    ++cur;
  }

  // Emit key, optional close-quote, whitespace and separator verbatim.
  out.append(s.substr(i, cur - i));

  if (cur < n && (s[cur] == '"' || s[cur] == '\'')) {
    const char quote = s[cur];
    out.push_back(quote);
    ++cur;
    const std::size_t value_begin = cur;
    while (cur < n && s[cur] != quote) {
      ++cur;
    }
    if (cur > value_begin) {
      out.append(kRedactionMarker);
    }
    return cur;  // closing quote (if present) emitted by the main loop
  }

  const std::size_t value_begin = cur;
  while (cur < n && !is_value_delim(s[cur])) {
    ++cur;
  }
  if (cur > value_begin) {
    out.append(kRedactionMarker);
  }
  return cur;
}

}  // namespace

std::string scrub(std::string_view text) {
  const std::size_t n = text.size();
  std::string out;
  out.reserve(n + kRedactionMarker.size());

  std::size_t i = 0;
  while (i < n) {
    const char c = text[i];

    // Only attempt token-shaped rules at the start of a token run (word
    // boundary), so a needle in the middle of a longer run is not mishandled.
    if (is_token_char(c) && (i == 0 || !is_token_char(text[i - 1]))) {
      if (const std::size_t kv = try_key_value(text, i, out); kv != kNoMatch) {
        i = kv;
        continue;
      }

      std::size_t j = i;
      while (j < n && is_token_char(text[j])) {
        ++j;
      }

      // Bare high-entropy run: >=20 token chars mixing letters AND digits — opaque
      // Kite access/enc/request tokens that appear without a key. The mixed
      // letters+digits requirement keeps ordinary prose (even very long words) and
      // pure-numeric ids/timestamps untouched; real broker tokens are alphanumeric.
      // KNOWN LIMITATION (accepted, low risk): a hypothetical all-letter opaque
      // token with no digit and no sensitive key name would slip this bare rule —
      // there is no all-letter length threshold that separates such a token from a
      // long natural-language word, so we do not guess. Named secrets are still
      // caught by the key=value rule regardless of value shape.
      if (j - i >= 20 && run_has_letter_and_digit(text, i, j)) {
        out.append(kRedactionMarker);
        i = j;
        continue;
      }

      // MPIN/TOTP: a 4–8 digit run in an auth context. Pure-digit runs only, so
      // mixed ids are handled above and long numeric ids/timestamps are left be.
      const std::size_t len = j - i;
      if (len >= 4 && len <= 8 && run_is_all_digits(text, i, j) &&
          auth_context_before(text, i)) {
        out.append(kRedactionMarker);
        i = j;
        continue;
      }

      out.append(text.substr(i, j - i));  // ordinary word/id — emit unchanged
      i = j;
      continue;
    }

    out.push_back(c);
    ++i;
  }

  return out;
}

bool is_provenance_id_shape(std::string_view value) noexcept {
  // (1) BOUNDED. An empty column carries no provenance and a long one is a blob.
  if (value.empty() || value.size() > kMaxProvenanceIdChars) {
    return false;
  }

  // (2) CHARSET, and (3) STRUCTURE in the same pass over the maximal alphanumeric
  // SEGMENTS — the runs between the '-'/'_'/'#' separators.
  //
  // (3) has TWO halves, and BOTH are load-bearing:
  //   (3a) COUNT: two or more non-empty segments, i.e. a separator standing
  //        strictly between two alphanumerics. A bare credential pasted whole is
  //        one unbroken segment and fails here.
  //   (3b) HOMOGENEITY: every segment must be all-hex (either case), all-digits
  //        (a subset of all-hex) or all-letters. THIS is what the count alone
  //        could not do: `[A-Za-z0-9_-]` IS the base64url alphabet, so a URL-safe
  //        credential carrying a single '-' or '_' between alphanumerics passes
  //        (3a) and would otherwise be emitted in full. A base64url/JWT run mixes
  //        digits with non-hex letters inside ONE segment, so it fails here; every
  //        segment of a minted ref is a hex run (sig8, the five UUID groups), a
  //        decimal counter (`#3`) or a word (the strategy name, `#X`).
  std::size_t segments = 0;
  bool in_segment = false;
  bool all_letters = false;  // homogeneity flags for the segment being scanned
  bool all_hex = false;
  for (const char c : value) {
    if (!is_provenance_id_char(c)) {
      return false;  // a '=', '.', '/', ':', space, quote... — not an id.
    }
    if (is_alnum(c)) {
      if (!in_segment) {
        ++segments;
        in_segment = true;
        all_letters = true;
        all_hex = true;
      }
      all_letters = all_letters && is_letter(c);
      all_hex = all_hex && is_hex_digit(c);
      if (!all_letters && !all_hex) {
        return false;  // a heterogeneous segment — credential-shaped, not an id.
      }
    } else {
      in_segment = false;  // a separator closes the current segment
    }
  }
  return segments >= 2;
}

std::string scrub_provenance_column(std::string_view value) {
  // FAIL CLOSED: verbatim only for a value that IS an id shape; everything else
  // takes the ordinary, unchanged scrub path.
  return is_provenance_id_shape(value) ? std::string(value) : scrub(value);
}

}  // namespace broker_exec::domain
