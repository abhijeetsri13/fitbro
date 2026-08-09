#pragma once

// broker_exec::domain::scrub — the shared secret-shape redaction scrubber
// (Story 2.2, FR-35, SEC-3/SEC-6).
//
// A single, pure string transform that replaces token-shaped substrings with a
// fixed marker. It binds to every output sink that could leak a credential: log
// lines, exception/`Error` message text, and the intent-log/ledger persistence
// path. The contract is "no secret in any sink": run untrusted text through
// `scrub()` before it is written anywhere durable or observable.
//
// DOMAIN STAYS PURE: this lives in `domain` and uses the C++ standard library
// only — NO OpenSSL, NO crypto, NO OS APIs. Encryption is a `secrets`-module
// concern; redaction is plain, deterministic string logic so the pure core can
// scrub errors without taking a crypto/platform dependency.
//
// Conservative by design: it must never mangle ordinary prose, identifiers, or
// pure-numeric ids (timestamps, order ids). It redacts only shapes that are
// strongly secret-like (see redaction.cpp for the exact rules).
//
// Idempotent: scrub(scrub(x)) == scrub(x). The marker itself contains no
// token-shaped run and no `key=value` separator, so re-scrubbing is a no-op.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <cstddef>
#include <string>
#include <string_view>

namespace broker_exec::domain {

// The fixed replacement marker. Chosen so it never matches any redaction rule
// (no digits, no >=20-char alnum run, no `=`/`:` separator) — this is what makes
// scrub() idempotent.
inline constexpr std::string_view kRedactionMarker = "***REDACTED***";

// Return a copy of `text` with every token-shaped substring replaced by
// kRedactionMarker. Never throws as part of its logic (only a std::bad_alloc
// from allocation could escape, as with any std::string operation).
[[nodiscard]] std::string scrub(std::string_view text);

// ── Provenance-id exemption (IMP-15, FR-27/FR-29) ────────────────────────────
//
// scrub()'s bare high-entropy rule cannot tell a hex UUID apart from a
// 32-char broker access token, so it redacts BOTH. That is exactly right for
// free-form text and exactly wrong for the audit trail's TYPED PROVENANCE
// COLUMNS (strategy/broker/account/client_ref/broker_order_id): a redacted
// client_ref makes a logged line unlinkable to the store, the intent log and the
// ledger — which is the only reason that line exists.
//
// The exemption is therefore a SHAPE ALLOWLIST applied to a WHOLE TYPED COLUMN
// and to nothing else.
//
//   !!  NEVER apply this to free-form text, nor to a run WITHIN a larger
//   !!  string. The allowlist charset is deliberately the same charset a
//   !!  credential uses; it is safe ONLY because a typed column is the caller
//   !!  declaring that the ENTIRE value is an identifier. Used as a substring
//   !!  rule it would disable the bare high-entropy rule outright.
//
// The rule — ALL THREE must hold:
//   1. length 1..kMaxProvenanceIdChars. Every id this library mints and every id
//      a broker sends is far shorter; a blob is not an id.
//   2. every byte in [A-Za-z0-9_-#]: the charset of make_client_ref
//      (`<strategy>-<sig8>-<uuid>`), of the slicer's `<parent>#<k>` children and
//      of IMP-13's `<parent>#X` square-off exit refs. A '=', '.', '/', ':',
//      space or any other byte means this is not an id but a pasted URL / JSON /
//      `key=value` blob, and it is scrubbed.
//   3. STRUCTURE, in two halves that must BOTH hold:
//      3a. at least TWO non-empty alphanumeric SEGMENTS — i.e. a separator
//          standing strictly between two alphanumerics. A credential pasted whole
//          is ONE unbroken segment and fails here.
//      3b. every segment is HOMOGENEOUS: all-hex (either case), all-digits (a
//          subset of all-hex), or all-letters. 3a ALONE IS NOT ENOUGH, and this
//          is the whole point: `[A-Za-z0-9_-]` is exactly the base64url alphabet,
//          so a URL-safe credential that happens to contain one '-' or '_'
//          between alphanumerics satisfies 1, 2 and 3a. Such a run mixes digits
//          with non-hex letters INSIDE a segment, so 3b rejects it; every segment
//          of an id this library mints is a hex run (the sig8, each of the five
//          UUID groups), a decimal counter (`#3`) or a word (the strategy name,
//          `#X`).
//
// WHAT THIS DOES AND DOES NOT GUARANTEE — stated precisely, because the claim
// "a credential in a typed column stays redacted" is only true within these
// bounds:
//   * a base64url / JWT-segment / mixed-alphanumeric credential is rejected,
//     with or without '-'/'_' separators. That is the realistic broker-token
//     shape (Kite access/enc/request tokens are mixed alnum).
//   * RESIDUAL, KNOWN AND ACCEPTED: a value whose every segment is homogeneous
//     is admitted even if it is not really an id — concretely, PURE HEX WITH A
//     SEPARATOR (`deadbeefcafebabe-0123456789abcdef`) is indistinguishable from
//     a sig8+UUID ref and IS emitted verbatim from a typed column. A hex-only
//     credential in a provenance column is therefore not protected by shape.
//     Closing it would mean rejecting the minted client_ref itself, so it is
//     accepted deliberately rather than papered over.
//   * RESIDUAL, pre-existing: an ALL-LETTER value is admitted (`alpha-beta`).
//     This costs nothing new — scrub() has never redacted an all-letter run
//     either (see the documented limitation in redaction.cpp), so such a value
//     was already emitted verbatim before the exemption existed.
//   * a broker minting an unbroken >=20-char mixed alphanumeric order id would
//     be indistinguishable from a credential and stays redacted. Kite order ids
//     are pure-numeric and Kotak's are short, so nothing real is lost today; the
//     fix for such a broker would be a broker-specific typed rule, never a wider
//     allowlist.
//   * pure-numeric ids (a Kite 15-digit order id) never needed the exemption —
//     scrub() already leaves an all-digit run alone.
//   * an anomalous value in an id column (a token, a blob, a URL) fails the
//     allowlist and is scrubbed. An id column that stops looking like an id is
//     precisely when redaction is wanted.

// The longest value still considered an identifier. make_client_ref() mints
// `<strategy>-<8 hex sig8>-<uuid>` where <uuid> is the CANONICAL RFC-4122 v4
// text form, 8-4-4-4-12 WITH its four dashes (format_uuid_v4) — so a ref with a
// 5-char strategy is 51 chars and has SEVEN alphanumeric segments, e.g.
// `alpha-1a2b3c4d-deadbeef-cafe-4bab-8abe-0123456789ab`, optionally plus a
// `#<k>` / `#X` suffix. 128 leaves generous headroom for a long strategy name
// without admitting a pasted blob.
inline constexpr std::size_t kMaxProvenanceIdChars = 128;

// True iff `value` — as a WHOLE typed provenance column — matches the allowlist
// described above. Read the warning block first: this must never be applied to
// free-form text or to a substring. Never throws.
[[nodiscard]] bool is_provenance_id_shape(std::string_view value) noexcept;

// Redact one TYPED PROVENANCE COLUMN: the value is returned VERBATIM iff it is
// an id shape, and is otherwise scrubbed exactly as any other text would be.
// The single definition of the fail-closed rule, so no two consumers can drift.
// Never throws as part of its logic (only std::bad_alloc, as with any string op).
[[nodiscard]] std::string scrub_provenance_column(std::string_view value);

}  // namespace broker_exec::domain
