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

}  // namespace broker_exec::domain
