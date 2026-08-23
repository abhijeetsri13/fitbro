#include "broker_exec/domain/redaction.hpp"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>

#include "broker_exec/domain/utf8.hpp"

namespace broker_exec::domain {

namespace {

// ── character classes ─────────────────────────────────────────────────────────

[[nodiscard]] char to_lower_ascii(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool is_letter(char c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

[[nodiscard]] bool is_upper_letter(char c) noexcept {
  return c >= 'A' && c <= 'Z';
}

[[nodiscard]] bool is_digit(char c) noexcept {
  return c >= '0' && c <= '9';
}

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

[[nodiscard]] bool is_alnum(char c) noexcept {
  return is_letter(c) || is_digit(c);
}

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
    "token", "secret", "password", "api_key", "api-key", "apikey", "mpin", "totp", "bearer"};

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
      if (len >= 4 && len <= 8 && run_is_all_digits(text, i, j) && auth_context_before(text, i)) {
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

// ── Strategy names (IMP-19) ──────────────────────────────────────────────────
//
// The INPUT-side twin of is_provenance_id_shape: the condition on a strategy name
// that makes every client_ref minted from it id-shaped. Read the derivation
// (V1-V6) in redaction.hpp; this file implements it and NOTHING beyond it.

namespace {

// V4: the provenance-id charset MINUS '#'. is_token_char IS [A-Za-z0-9_-]; the
// '#' that is_provenance_id_char adds is excluded because idempotency's
// is_child_ref/parent_of key on it, so a '#' in a name would make a PARENT ref
// parse as a CHILD of a truncated parent.
[[nodiscard]] bool is_strategy_name_char(char c) noexcept {
  return is_token_char(c);
}

// The FIRST '-'/'_'-separated segment of `name` that is neither all-letters nor
// all-hex (V5), or an empty view when every segment is homogeneous. Deliberately
// the same two homogeneity classes, computed the same way, as the loop in
// is_provenance_id_shape — if that rule ever changes, this must change with it.
[[nodiscard]] std::string_view first_mixed_segment(std::string_view name) noexcept {
  std::size_t i = 0;
  while (i < name.size()) {
    if (!is_alnum(name[i])) {
      ++i;  // a separator closes / precedes a segment
      continue;
    }
    std::size_t j = i;
    bool all_letters = true;
    bool all_hex = true;
    while (j < name.size() && is_alnum(name[j])) {
      all_letters = all_letters && is_letter(name[j]);
      all_hex = all_hex && is_hex_digit(name[j]);
      ++j;
    }
    if (!all_letters && !all_hex) {
      return name.substr(i, j - i);
    }
    i = j;
  }
  return {};
}

// `name` with a '-' inserted at every letter<->digit transition INSIDE a
// heterogeneous segment ("momentum-v2" -> "momentum-v-2", "S1" -> "S-1"). A
// homogeneous segment is copied untouched, so a valid part of the name is never
// rewritten. Every produced piece is all-letters or all-digits, hence homogeneous.
[[nodiscard]] std::string split_mixed_segments(std::string_view name) {
  std::string out;
  out.reserve(name.size() + 4);
  std::size_t i = 0;
  while (i < name.size()) {
    if (!is_alnum(name[i])) {
      out.push_back(name[i]);
      ++i;
      continue;
    }
    std::size_t j = i;
    while (j < name.size() && is_alnum(name[j])) {
      ++j;
    }
    const std::string_view segment = name.substr(i, j - i);
    const bool homogeneous = first_mixed_segment(segment).empty();
    for (std::size_t k = 0; k < segment.size(); ++k) {
      if (!homogeneous && k > 0 && is_letter(segment[k]) != is_letter(segment[k - 1])) {
        out.push_back('-');
      }
      out.push_back(segment[k]);
    }
    i = j;
  }
  return out;
}

// The name as it may appear in an Error message: TRUNCATED to the maximum a valid
// name could be, with every byte outside [A-Za-z0-9_-] shown as '?'. An invalid
// name is by definition untrusted text — echoing it raw would put arbitrary bytes
// (and an arbitrary LENGTH) into an alert body, which is the exact hazard
// render_provenance_block's allowlist + bound exist to close.
[[nodiscard]] std::string sanitized_for_message(std::string_view name) {
  const std::size_t shown =
      name.size() < kMaxStrategyNameChars ? name.size() : kMaxStrategyNameChars;
  std::string out;
  out.reserve(shown);
  for (std::size_t i = 0; i < shown; ++i) {
    out.push_back(is_strategy_name_char(name[i]) ? name[i] : '?');
  }
  return out;
}

}  // namespace

std::string explain_invalid_strategy_name(std::string_view name) {
  // V1 (first half): non-empty. The second half — "carries at least one letter or
  // digit" — is the same attributability rule and is checked after V4; see there.
  if (name.empty()) {
    return "strategy name is EMPTY; it must be 1.." + std::to_string(kMaxStrategyNameChars) +
           " characters from [A-Za-z0-9_-], at least one of them a letter or digit, with each "
           "'-'/'_'-separated segment all letters or all hex";
  }

  const std::string shown = sanitized_for_message(name);

  // V2: bounded, so `<name>-<sig8>-<uuid>` (+ any `#<k>` child suffix) stays
  // inside kMaxProvenanceIdChars.
  if (name.size() > kMaxStrategyNameChars) {
    return "strategy name '" + shown + "' (truncated here) is " + std::to_string(name.size()) +
           " bytes; the maximum is " + std::to_string(kMaxStrategyNameChars) +
           " so that every client_ref it mints stays inside the " +
           std::to_string(kMaxProvenanceIdChars) + "-char provenance-id bound";
  }

  // V3: valid UTF-8, in canonical_text's sense (P1: valid input is a fixed point).
  // Checked BEFORE the charset so an ill-formed name is diagnosed as ill-formed.
  const std::string canonical = canonical_text(name);
  if (std::string_view(canonical) != name) {
    return "strategy name '" + shown +
           "' (bytes outside [A-Za-z0-9_-] shown as '?') is not valid UTF-8; an ill-formed name "
           "travels raw through the store and the idempotency index and is normalised only at the "
           "log writer, where the hash preimage and the stored bytes then disagree";
  }

  // V4: charset.
  for (const char c : name) {
    if (!is_strategy_name_char(c)) {
      return "strategy name '" + shown +
             "' (bytes outside [A-Za-z0-9_-] shown as '?') contains a character that is not in "
             "[A-Za-z0-9_-]; a space, '.', '/', ':' or '#' makes the strategy column and every "
             "client_ref it mints unloggable (both are then wholly redacted)";
    }
  }

  // V1 (second half): ATTRIBUTABLE. A name of nothing but separators ("-", "___",
  // "-_-") clears every other rule — it mints a perfectly id-shaped ref and
  // survives its own column — and is exactly as unattributable as the empty name
  // rejected above, so it is refused for the SAME reason rather than left as an
  // inconsistency between the rule and its stated rationale.
  //
  // CHECKED HERE, NOT BESIDE THE EMPTY TEST, ON PURPOSE: after V3/V4 we know every
  // byte is in [A-Za-z0-9_-], so "no letter or digit" can only mean "all
  // separators". Checked first, an ill-formed or out-of-charset name (which also
  // has no alphanumeric) would be diagnosed with this message instead of the more
  // useful one it gets above.
  bool has_alnum = false;
  for (const char c : name) {
    has_alnum = has_alnum || is_alnum(c);
  }
  if (!has_alnum) {
    return "strategy name '" + shown +
           "' has no letter or digit; a name of only '-'/'_' separators renders a strategy "
           "column that attributes an order to nothing at all, exactly as an empty name does";
  }

  // ── ORDER IS LOAD-BEARING BELOW THIS LINE: V4 MUST PRECEDE V5/V6 ─────────────
  // Everything above echoes only `shown`, which sanitized_for_message has already
  // reduced to [A-Za-z0-9_-] and truncated. V5's message below embeds the
  // offending SEGMENT and the suggested spelling UN-SANITISED, straight from the
  // caller's bytes — which is safe ONLY because V4 has already proved every byte
  // of `name` is in the strategy charset. Reorder these blocks and a raw byte (a
  // newline, a ']', a U+202E) reaches an alert body through
  // session::require_valid_strategy_names, defeating the caller invariant that
  // render_provenance_block's grammar depends on (see redaction.hpp).

  // V5: every segment homogeneous — the rule that makes the minted ref id-shaped.
  if (const std::string_view mixed = first_mixed_segment(name); !mixed.empty()) {
    std::string msg = "strategy name '" + shown + "': segment '" + std::string(mixed) +
                      "' mixes letters and digits (every segment of a client_ref must be all "
                      "letters or all hex, or the WHOLE ref stops being id-shaped and is redacted "
                      "in every alert and ledger entry)";
    // Only suggest a spelling that would actually be accepted. The recursion
    // through is_valid_strategy_name terminates at depth 2: every segment of
    // `suggestion` is homogeneous by construction, so it cannot reach this branch.
    if (const std::string suggestion = split_mixed_segments(name);
        suggestion != name && is_valid_strategy_name(suggestion)) {
      msg += "; use '" + suggestion + "'";
    }
    return msg;
  }

  // V6: the name must also survive its OWN typed column. Stated against the real
  // scrubber rather than restated as a rule, so it cannot drift from scrub().
  if (const std::string as_column = scrub_provenance_column(name);
      std::string_view(as_column) != name) {
    return "strategy name '" + shown +
           "' is a single un-separated run that domain::scrub cannot distinguish from a "
           "credential, so the strategy column itself would be redacted; separate it with a '-' "
           "or '_' (a name with two or more segments is exempt as a whole typed column)";
  }

  return {};
}

bool is_valid_strategy_name(std::string_view name) {
  // ONE definition of the rule: validity IS "there is nothing to explain". A
  // second, independently-written predicate is exactly how a check and its
  // diagnostic drift apart.
  return explain_invalid_strategy_name(name).empty();
}

bool is_instrument_symbol_shape(std::string_view value) noexcept {
  // BOUNDED, and TIGHTER than the id rule rather than looser: UPPERCASE letters
  // and digits only. An exchange symbol never mixes case; every realistic broker
  // credential does. See the contract block in redaction.hpp for why a symbol
  // cannot reuse is_provenance_id_shape (it is ONE heterogeneous segment, so it
  // fails the >=2-segment rule and would fall through to scrub(), whose bare
  // high-entropy rule destroys every >=20-char option symbol we trade).
  if (value.empty() || value.size() > kMaxInstrumentSymbolChars) {
    return false;
  }
  for (const char c : value) {
    if (!is_upper_letter(c) && !is_digit(c)) {
      return false;
    }
  }
  return true;
}

std::string scrub_symbol_column(std::string_view value) {
  return is_instrument_symbol_shape(value) ? std::string(value) : scrub(value);
}

namespace {

// True iff `rendered` can sit inside the ` [k=v k=v]` block without being able to
// terminate it, forge a neighbouring field, or inject a line break.
//
// AN ALLOWLIST, DELIBERATELY — see the long rationale in redaction.hpp. The
// previous denylist (c <= 0x20, '[', ']', '=') was the ONLY guard in an otherwise
// allowlist-shaped design, and scrub() passes unrecognised bytes through
// unchanged, so every byte >= 0x7F reached the operator verbatim. Unicode
// substitutes for the four blocked ASCII bytes (U+00A0 for space, U+2028 for the
// newline, U+FF3D for ']', U+202E to reverse the run) let a broker-controlled
// broker_order_id compose arbitrary multi-line prose inside a Critical alert.
// Enumerating hostile code points is unwinnable; admitting only the charset a
// legitimate value can possibly use is. This ONE predicate is shared with
// is_provenance_id_shape so the two can never drift apart.
[[nodiscard]] bool is_block_safe(std::string_view rendered) noexcept {
  for (const char c : rendered) {
    if (!is_provenance_id_char(c) && c != '*') {  // '*' for kRedactionMarker
      return false;
    }
  }
  return true;
}

// A key must satisfy the SAME charset (no '*' — a key is never the marker) and be
// non-empty. Keys are first-party literals today; this is defence-in-depth for a
// public renderer whose `key` is a view the caller supplies.
[[nodiscard]] bool is_block_safe_key(std::string_view key) noexcept {
  if (key.empty()) {
    return false;
  }
  for (const char c : key) {
    if (!is_provenance_id_char(c)) {
      return false;
    }
  }
  return true;
}

// Render ONE column through the shape rule its Kind names. The two rules are
// genuinely different (an id has >=2 homogeneous segments; a symbol is one
// uppercase-alnum run) and neither may be applied to the other's column.
[[nodiscard]] std::string render_column(const ProvenanceField& field) {
  switch (field.kind) {
    case ProvenanceField::Kind::Symbol:
      return scrub_symbol_column(field.value);
    case ProvenanceField::Kind::Id:
      break;
  }
  return scrub_provenance_column(field.value);
}

}  // namespace

std::string render_provenance_block(std::initializer_list<ProvenanceField> fields) {
  std::string block;
  for (const ProvenanceField& field : fields) {
    if (field.value.empty()) {
      continue;  // an absent id is omitted entirely, never a dangling `key=`
    }
    if (!is_block_safe_key(field.key)) {
      continue;  // a key we cannot render safely cannot attribute its value: drop the field
    }
    // THE ONLY exemption path: a WHOLE typed column, never a substring of the
    // body. A value that does not match its column's shape takes the ordinary scrub.
    std::string rendered = render_column(field);
    // ONE guard, BOTH properties, applied to BOTH branches above (the verbatim
    // shape branch and the scrub fallback). The length bound must live HERE and
    // not only inside is_provenance_id_shape: scrub() leaves an all-digit or
    // all-letter run of ANY length untouched, and broker order ids are numeric, so
    // the fallback was an unbounded broker-controlled write into an alert body, a
    // ledger hash preimage and the JSONL file. An oversized block overruns
    // Telegram's 4096-char message limit, failing every channel and SUPPRESSING
    // the Critical alert outright.
    if (rendered.size() > kMaxProvenanceIdChars || !is_block_safe(rendered)) {
      rendered.assign(kRedactionMarker);  // fail closed: a wrong id is worse than none
    }
    block += block.empty() ? " [" : " ";
    block.append(field.key);
    block += '=';
    block += rendered;
  }
  if (!block.empty()) {
    block += ']';
  }
  return block;  // "" when nothing survived -> the caller's body is unchanged
}

}  // namespace broker_exec::domain
