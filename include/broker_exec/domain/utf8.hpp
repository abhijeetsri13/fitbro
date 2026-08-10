#pragma once

// broker_exec::domain::canonical_text — THE ONE UTF-8 CANONICALISATION (IMP-17).
//
// ── WHAT PROBLEM THIS SOLVES ─────────────────────────────────────────────────
//
// Every durable record this library writes is a JSON line rendered by nlohmann,
// and every one of those renders MUST use `error_handler_t::replace`: the DEFAULT
// handler is `strict`, which THROWS json::type_error.316 on the first ill-formed
// UTF-8 byte — a throw across a `Result<T>` no-throw boundary, on the order hot
// path. But `replace` alone is not enough, because it rewrites ill-formed
// sequences as U+FFFD ON THE WAY OUT. Any text that was HASHED (or SIGNED) in its
// raw form and then STORED through such a dump is stored in a form that DIFFERS
// from the bytes that were hashed, so the record's own integrity check fails and
// the log reports ITSELF broken — a denial-of-audit / safe-start blocker that any
// untrusted byte could trigger.
//
// canonical_text() closes both halves at once. Run it EXACTLY ONCE, UP FRONT, on
// caller-supplied text, and the value it returns IS — by definition — both the
// hash/signature preimage AND the bytes that reach the file. There is no second
// transform between the two, so they cannot diverge, and the later dump has
// nothing left to replace.
//
//   THE RULE FOR EVERY CONSUMER:  canonicalise ONCE, then hash and store THE SAME
//   std::string. If you are ever tempted to hash one string and store another,
//   that is the IMP-17 defect.
//
// ── NORMALISE, NOT REJECT ────────────────────────────────────────────────────
//
// An ill-formed byte never fails the write. Losing an audit entry (or refusing an
// order intent) because a strategy name carried a stray byte is strictly worse
// than recording a sanitised one. Valid UTF-8 — ASCII and multi-byte alike —
// passes through BYTE-IDENTICAL, so no existing chain's hash can move.
//
// ── WHO USES IT (and why it lives HERE, in `domain`) ─────────────────────────
//
//   * ledger::Ledger::append / make_heartbeat  (src/ledger/ledger.cpp)
//   * intentlog::IntentLog::append             (src/intentlog/intent_log.cpp)
//
// Two hash chains, one text transform. This is PURE TEXT LOGIC with no ledger,
// crypto, OS or JSON dependency, so it belongs in the pure core alongside
// redaction — NOT copy-pasted into each writer, where the two copies would
// eventually disagree about what a "maximal subpart" is and the two logs would
// canonicalise the same bytes differently. THERE MUST BE EXACTLY ONE DEFINITION
// OF THIS FUNCTION IN THE TREE.
//
// ── THE PROPERTIES CALLERS ARE ALLOWED TO RELY ON ────────────────────────────
//
// These are the load-bearing facts; see redaction ordering below for the one that
// is most often got wrong.
//
//   P1. VALID UTF-8 IS A FIXED POINT. Well-formed input is returned byte for byte,
//       so canonical_text(x) == x for every payload this library writes today.
//   P2. THE OUTPUT IS ALWAYS VALID UTF-8, therefore a subsequent
//       dump(..., error_handler_t::replace) is provably a NO-OP: nlohmann's
//       decoder never reaches its reject state and copies the bytes through.
//   P3. IDEMPOTENT. canonical_text(canonical_text(x)) == canonical_text(x).
//       Follows from P1 + P2, and is what makes "normalise once" safe to assert
//       defensively a second time in a hand-fillable aggregate's to_json().
//   P4. THE ASCII SUBSEQUENCE IS PRESERVED EXACTLY. Every byte < 0x80 is copied
//       verbatim, in order, and NO ASCII byte is ever emitted for non-ASCII input
//       (U+FFFD is EF BF BD — every byte >= 0x80). So ASCII bytes are neither
//       created, destroyed, reordered nor introduced between other ASCII bytes.
//   P5. IT NEVER SHRINKS, AND NEVER DELETES A RUN TO NOTHING. Every ill-formed
//       maximal subpart of k >= 1 bytes becomes 3 bytes, so len(out) >= len(in)
//       and a non-ASCII run stays a non-empty non-ASCII run. IT IS NOT
//       LENGTH-PRESERVING — this is the property that makes the ordering rule
//       below necessary.
//
// ── ORDERING vs. domain::scrub() — READ THIS BEFORE MOVING EITHER CALL ────────
//
// Consumers scrub FIRST and canonicalise LAST. That order is FIXED, and the
// reason is precise:
//
//   ORDER-INVARIANT (safe either way): scrub()'s TOKEN-SHAPED rules — the
//   `key=value` rule and the >=20-char high-entropy run rule. A token run is a
//   maximal run of ASCII [A-Za-z0-9_-], and by P4+P5 normalisation can neither
//   JOIN two such runs (it never emits an ASCII byte between them, and never
//   empties the non-ASCII run separating them) nor SPLIT one (it never touches an
//   ASCII byte). So scrub() tokenises identically on either side of it and those
//   rules fire on exactly the same runs.
//
//   ORDER-SENSITIVE (NOT safe to reorder): the bare MPIN/TOTP digit-run rule.
//   domain::auth_context_before() looks back a FIXED 10-BYTE window for an auth
//   keyword, and by P5 normalisation is NOT length-preserving — one ill-formed
//   byte becomes three — so normalising first MOVES the keyword relative to that
//   byte window and can flip the decision IN EITHER DIRECTION:
//
//     "mpin \x80\x80 1234"  -- scrub-first REDACTS (the window still reaches
//                              "mpin"); normalise-first does NOT (the two U+FFFDs
//                              occupy 6 bytes and push it out of the window).
//     "passwordtokentotp\xC0\x80" "12345678"
//                           -- scrub-first does NOT redact ("totp" is embedded in
//                              a longer word, so the whole-word check rejects it);
//                              normalise-first DOES (the expansion separates it).
//
//   Hence: scrub() must keep running on the RAW bytes — that is the behaviour
//   every existing redaction test pins — and canonicalisation must stay LAST, so
//   the preimage==stored invariant holds for the FINAL string and cannot be
//   reintroduced by a transform running after it. Both of the counterexamples
//   above are pinned as tests (src/domain/utf8_test.cpp, src/ledger/ledger_test.cpp).
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`, no JSON.

#include <cstddef>
#include <string>
#include <string_view>

namespace broker_exec::domain {

// U+FFFD REPLACEMENT CHARACTER, spelled BYTE-EXACTLY so it is independent of the
// source file's encoding. This is the only non-ASCII literal in the module.
inline constexpr std::string_view kUtf8Replacement = "\xEF\xBF\xBD";

// Return `text` with every ill-formed UTF-8 sequence replaced by U+FFFD. Valid
// UTF-8 is returned BYTE-IDENTICAL (P1).
//
// THE REPLACEMENT SEMANTICS ARE nlohmann's, DELIBERATELY: one U+FFFD per MAXIMAL
// SUBPART (the Unicode-recommended practice that serializer.hpp's `replace`
// handler implements). Concretely, on hitting a byte that cannot continue the
// sequence in progress we emit ONE U+FFFD for the bytes consumed so far and
// RE-READ the offending byte as a fresh lead — a byte can be wrong for its
// predecessor and still fine on its own (`\xE2\x82` + `\xE2\x82\xB9` -> U+FFFD +
// rupee sign, not two replacements). A truncated run at end-of-string yields
// exactly one U+FFFD. Matching those semantics EXACTLY is what makes P2 and P3
// true, and P2 is the whole point: it is what makes the later dump a no-op.
//
// Never throws as part of its logic (only std::bad_alloc, as with any string op).
[[nodiscard]] std::string canonical_text(std::string_view text);

}  // namespace broker_exec::domain
