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
#include <initializer_list>
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
//
// THE `strategy` COLUMN — THE COST THAT MOTIVATED is_valid_strategy_name BELOW.
// `strategy` is caller-supplied and NOTHING inside this block constrains its
// charset (see idempotency::make_client_ref, which concatenates it into the
// client_ref as-is).
// A perfectly ordinary strategy name with a SPACE — "iron condor v2" — is
// therefore not id-shaped, survives scrub() untouched, and is then rejected
// WHOLESALE by the block guard, so it reaches the operator as
// `strategy=***REDACTED***` in EVERY alert and EVERY ledger entry.
//
//   THE BLOCK IS NOT THE PLACE TO FIX THAT. Loosening the guard to admit a space
//   would hand a broker-controlled `broker_order_id` the field separator, which is
//   precisely the forgery this design exists to prevent. The charset must instead
//   be constrained where a strategy name ENTERS the system.
//
// AND THE SPACE IS NOT THE WORST OF IT. A strategy name is the FIRST SEGMENT of
// every client_ref it mints, so it must also be HOMOGENEOUS (all letters, or all
// hex/digits) or the WHOLE REF stops being id-shaped: "S1" is a letter plus a
// digit in one unbroken run, which fails 3b, and the 48-char ref then falls back
// to scrub() and is destroyed. A strategy called "S1" makes every alert about its
// orders say `client_ref=***REDACTED***`. Spelling it "S-1" fixes it entirely.
//
// STATUS SINCE IMP-19: ENFORCED WHERE A NAME IS DECLARED, NOT WHERE ONE IS USED.
// The rule is `is_valid_strategy_name()` below (derived from
// is_provenance_id_shape, in this same file, so the two cannot drift), and it is
// applied at the two places a strategy name is CONFIGURATION rather than per-order
// data:
//   * config::load() rejects an invalid `strategies.names` entry outright, and
//   * session::require_valid_strategy_names(), wired as the REQUIRED safe-start
//     check `SafeStartContext::strategy_name_check`, refuses to start.
// Both name the offending name AND why, so the operator fixes it before trading.
//
//   WHAT THIS IS NOT: IT IS NOT CONTAINMENT. Do not read the above as "an invalid
//   name cannot reach a client_ref". `config::StrategiesConfig::names` is a
//   DECLARATION and has no other consumer in this tree; the name that ACTUALLY
//   mints a ref is `domain::OrderIntent::strategy`, which a host fills in directly
//   and which Dispatcher::place() never cross-checks against any list. THE LIBRARY
//   ITSELF TAKES THAT ROUTE: the square-off exit path sets
//   `exit.strategy = "square_off"` (src/adapters/kite/kite_broker_adapter.cpp and
//   src/adapters/kotak/kotak_broker_adapter.cpp) — a name no configuration list
//   contains. It happens to be a VALID name, which is the point: the bypass is not
//   hypothetical, it is in the shipping code and it is fine only by luck of
//   spelling. Closing that residual needs a check ON THE ORDER PATH, deliberately
//   NOT taken here because it would mean refusing to place an order mid-session.
//   SECONDARY (pre-existing, and true of all ten safe-start checks): nothing in
//   this repo constructs a SafeStartContext outside tests, so the cold-boot check
//   has no production caller yet either.
//
// The previous story deferred this because it framed the only boundary as
// reserve()/place(), where rejecting means REFUSING TO PLACE AN ORDER mid-session.
// That framing was wrong: a strategy name is known at startup, so it belongs in
// the cold-boot gate that already exists for exactly this class of "fail visibly
// before trading" problem. THE ORDER PATH IS UNCHANGED — reserve()/place() gained
// no new rejection, and both outcomes described above are still pinned by their
// test in redaction_test.cpp (they document what an UNVALIDATED name still costs).

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

// ── Strategy names (IMP-19) — the INPUT side of the id shape ─────────────────
//
// THE RULE IS DERIVED FROM is_provenance_id_shape ABOVE, NOT INVENTED. A strategy
// name is the FIRST SEGMENT (or first few segments) of every client_ref
// idempotency::make_client_ref mints — `<strategy>-<sig8>-<uuid>` — whose other
// segments are hex by construction, so the ref is id-shaped exactly when the
// strategy part contributes only well-formed segments. This predicate is that
// condition, stated on the name alone:
//
//   V1. ATTRIBUTABLE: NON-EMPTY, AND CARRYING AT LEAST ONE LETTER OR DIGIT. An
//       empty name mints `-<sig8>-<uuid>`, which is still id-shaped, but it renders
//       the `strategy=` column as nothing at all — an order nobody can attribute.
//       A name of nothing but separators (`-`, `___`, `-_-`) passes every OTHER
//       rule here — it mints an id-shaped ref and survives its own column — and is
//       EXACTLY as unattributable, so it is refused by the same rule rather than
//       left as an inconsistency. Both halves are rejected because they are
//       useless, not because they are unsafe. (Implementation note: the
//       alphanumeric half is checked AFTER V4 so that an ill-formed or
//       out-of-charset name is still DIAGNOSED as ill-formed or out-of-charset,
//       which is the more useful thing to tell the operator.)
//   V2. AT MOST kMaxStrategyNameChars BYTES — A POLICY NUMBER WITH HEADROOM, NOT
//       AN ARITHMETIC ONE. The DERIVED ceiling is 78: a parent ref is len(name)+46
//       chars (1 + sig8 + 1 + a 36-char canonical UUID), the widest child suffix in
//       the tree is the slicer's `#999` at 4 bytes, and 78+46+4 = 128 =
//       kMaxProvenanceIdChars exactly. 64 is a deliberate round number chosen BELOW
//       that: a strategy name is a human label, 64 is already generous for one, and
//       the remaining 14 bytes are headroom for a wider child suffix (a 5- or
//       6-digit slice counter) so nobody has to redo this arithmetic to add one.
//       Do not read 64 as derived — if the ref format or the child suffix grows it
//       is the 78 that moves first, and 64 has slack for a while yet. The child
//       suffixes must ALSO stay id-shaped: they are the client_ref an alert about a
//       slice carries.
//   V3. VALID UTF-8, in domain::canonical_text's sense — i.e. canonical_text(name)
//       == name. Implied by V4 (the charset is ASCII), but checked first and
//       separately so an ill-formed name is DIAGNOSED as ill-formed instead of as
//       a stray character. An ill-formed name is otherwise carried raw through the
//       store and the idempotency index and is only normalised at the log writer,
//       where hash preimage and stored bytes then disagree (the IMP-17 defect).
//   V4. EVERY BYTE IN [A-Za-z0-9_-]. That is the id charset MINUS '#', and the
//       exclusion is derived too: idempotency::is_child_ref/parent_of key on '#',
//       so a '#' inside a strategy name would make a PARENT ref parse as a CHILD
//       of a truncated parent. A space, '.', '/', ':' or '=' is rejected here for
//       the reason the block guard rejects it there ("iron condor v2").
//   V5. EVERY '-'/'_'-SEPARATED SEGMENT IS HOMOGENEOUS — all letters, or all hex
//       (all digits being a subset). This is rule 3b above, applied to the name:
//       `S1` is one letter plus one digit in a single run, so a ref minted from it
//       is NOT id-shaped and the whole ~48-char ref is destroyed by scrub().
//   V6. THE NAME SURVIVES ITS OWN COLUMN: scrub_provenance_column(name) == name.
//       Stated behaviourally, against the real scrubber, rather than restated as a
//       second charset rule — so it cannot drift from scrub(). It bites in exactly
//       one case V1-V5 leave open: a name that is a SINGLE segment is not
//       id-shaped (rule 3a wants two), so it falls to scrub(), whose bare
//       high-entropy rule destroys any >=20-char run mixing letters and digits.
//       A 20-char all-hex name like `abcdef0123456789abcd` mints a perfectly
//       id-shaped ref while its own `strategy=` column is redacted; splitting it
//       with one '-' fixes both.
//
// WHAT V1-V6 BUY, AND WHERE IT IS PROVEN: for every accepted name, BOTH
//   * `is_provenance_id_shape(make_client_ref(name, sig, uuid))`, and its `#<k>` /
//     `#X` children, and
//   * `scrub_provenance_column(name) == name`
// hold. The first is the actual contract and is proven with the REAL minter in
// idempotency_test.cpp ("IMP-19: every accepted strategy name mints an id-shaped
// ref") — a predicate tested in isolation would only prove itself. It is proven
// EXHAUSTIVELY, not by example: the companion case ("EXHAUSTIVE — every accepted
// name over a small alphabet round-trips") sweeps every string of length <=5 over
// {a,g,F,0,9,-,_} — one letter that is hex, one that is not, an uppercase, two
// digits, both separators — and checks the ref, its `#1`/`#999`/`#X` children and
// the name's own column for each name the rule accepts. LOOSEN V1-V6 AND THAT
// SWEEP IS WHAT FAILS, which is the point of writing it that way.
//
// WHAT IT COSTS, STATED PLAINLY: `momentum-v2` IS REJECTED. `v2` is one letter
// plus one digit, which is precisely the shape rule 3b exists to reject, and rule
// 3b is what keeps a base64url credential out of a typed column. THE ENTIRE COST
// IS ONE DASH, ONCE, AT CONFIG TIME, and the operator is not left to guess where:
// the rejection message PRINTS THE REPLACEMENT (`momentum-v2` -> "use
// 'momentum-v-2'"). `atm-straddle-9-20`, `alpha`, `S-1` and `IRON_CONDOR` are
// accepted unchanged.
//
// THE ALTERNATIVE — RELAXING 3b FOR THE "STRATEGY SLOT" — WAS CONSIDERED AND
// REJECTED. There are two versions of it and they fail for different reasons.
//
//   THE NARROW VERSION DOES NOT EVEN WORK. The strategy slot is not ONE segment;
//   it is as many segments as the operator writes. In `momentum-v2-<sig8>-<uuid>`
//   the offending segment `v2` is the SECOND, so a "first segment only" exemption
//   admits nothing that is not already admitted.
//
//   THE VERSION THAT WOULD WORK IS TAIL-ANCHORING: recognise the minted tail
//   `-<8 hex>-<8-4-4-4-12 hex>` and relax the head in front of it. That is a real
//   option — it is rejected on its own merits, below, and NOT on the argument
//   originally written here, which was false:
//     NOT A REASON (the claim this text used to make): "it would fix the wrong
//       half, because the bare `strategy=` column would still be destroyed by
//       scrub()". That is true ONLY for a name of >=20 token chars MIXING letters
//       and digits — `momentum-breakout-v2-intraday`, 29 chars. It is NOT true of
//       the flagship example: `momentum-v2` is 11 chars, under scrub()'s 20-char
//       high-entropy threshold, and renders fine as a bare column TODAY, as do
//       `iron-condor-v2` and `bnf-15m`. For exactly these names tail-anchoring
//       would have fixed the only thing that was broken. Do not reuse that
//       argument.
//     (a) THE TAIL IS UNAUTHENTICATED — ANYONE CAN FORGE IT FOR FREE. It is 46
//         bytes of fixed shape: no secret, no signature, nothing a third party
//         cannot type. Anchoring on it converts an UNCONDITIONAL structural
//         property ("every segment is homogeneous") into one GATED BY A SHAPE THAT
//         COSTS NOTHING TO MINT. That is the difference between a structural rule
//         and a password, and this predicate is not holding a password.
//     (b) THE PREDICATE IS SHARED WITH A BROKER-CONTROLLED COLUMN. The same
//         is_provenance_id_shape decides `broker_order_id` — in
//         render_provenance_block (src/domain/redaction.cpp) and in
//         structured_logger's sentinel swap. Under tail-anchoring THE BROKER would
//         choose which branch ITS OWN VALUE is evaluated under, by appending 46
//         forgeable bytes: into a Critical alert body, and into the ledger's hash
//         preimage. A guard whose subject selects its own rule is not a guard.
//     (c) THE HEAD RULE HAS NO SAFE FORM AT THIS ALTITUDE. The natural head rule
//         (charset + length) admits a 32-char Kite access token outright. Even the
//         careful one — "the head must itself survive scrub()" — admits a 16-char
//         mixed-alnum API key, because scrub()'s bare rule needs 20 chars before it
//         fires. The worked counterexample, admitted VERBATIM from a typed column
//         under tail-anchoring and REJECTED by the rule as shipped:
//
//           Xk29mZpQ7rTb4Lw8-1a2b3c4d-deadbeef-cafe-4bab-8abe-0123456789ab
//
//         (16-char credential head, then a perfectly well-formed sig8 and UUID.)
//     (d) DONE PROPERLY IT IS NOT A RELAXATION AT ALL — it is a SPLIT of
//         ProvenanceField::Kind::Id into MintedRef (first-party: this library built
//         the value) and ForeignId (broker-controlled), so a tail exemption could
//         apply to the first and never to the second. That is a story of its own —
//         a new column kind and every call site re-classified — not a loosened
//         predicate, and it is the only shape in which (a)-(c) are answerable.
// The cost of the rule as shipped is one '-' in a name; the cost of relaxing 3b in
// place is a weaker credential guarantee on a broker-controlled column. That trade
// is not close.
//
// RENAMING IS NOT FREE — SAY IT OUT LOUD. `strategy` is hashed into
// idempotency::signal_signature, so renaming `S1` to `S-1` CHANGES THE SIGNATURE
// OF EVERY SIGNAL that strategy emits: the restart dedupe will not recognise its
// own in-flight orders, exactly as in the IMP-11 stop upgrade. Rename BETWEEN
// sessions, FLAT, never with working orders outstanding. This is also why the
// enforcement REJECTS a bad name instead of silently normalising it — a silent
// rewrite would move every signature with no operator involved at all.
inline constexpr std::size_t kMaxStrategyNameChars = 64;

// True iff `name` satisfies V1-V6. Never throws as part of its logic (only a
// std::bad_alloc, as with any string operation — it is deliberately expressed in
// terms of canonical_text()/scrub_provenance_column() rather than reimplementing
// either, so it allocates and is NOT noexcept). Cheap enough for a config-time /
// cold-boot check, which are its only intended call sites.
[[nodiscard]] bool is_valid_strategy_name(std::string_view name);

// "" when `name` is valid; otherwise ONE operator-facing sentence naming the
// offender and the rule it broke, with a suggested spelling where one exists
// (e.g. "strategy name 'S1': segment 'S1' mixes letters and digits; spell it
// 'S-1'"). Suitable for an Error message: the echoed name is TRUNCATED to
// kMaxStrategyNameChars and every byte outside [A-Za-z0-9_-] is shown as '?', so
// a hostile name can neither forge a provenance block nor overrun an alert.
[[nodiscard]] std::string explain_invalid_strategy_name(std::string_view name);

// ── Instrument-symbol columns (a SEPARATE shape, deliberately) ────────────────
//
// A trading symbol is NOT an id shape and a plain reuse of the rule above would
// destroy it: `is_provenance_id_shape` demands >=2 alphanumeric SEGMENTS, and a
// symbol is ONE unbroken heterogeneous segment ("BANKNIFTY24JUN52000CE"). It
// would therefore fall through to scrub(), whose bare high-entropy rule redacts
// any >=20-char run mixing letters and digits — which is EXACTLY the option
// symbols this library trades:
//
//   NIFTY24JUN24000CE        17 chars -> survived scrub by luck of length
//   FINNIFTY24JUN23000CE     20 chars -> DESTROYED
//   BANKNIFTY24JUN52000CE    21 chars -> DESTROYED
//   MIDCPNIFTY24JUN12000CE   22 chars -> DESTROYED
//
// So a symbol gets its OWN, tighter allowlist. It is safe precisely because it is
// tighter than the id rule, not looser: an exchange symbol is UPPERCASE letters
// and digits only, while every realistic broker credential (Kite access/enc/
// request tokens, JWT segments, base64url) mixes CASE. A value that is not
// all-uppercase-alnum fails and takes the ordinary scrub, unchanged.
//
// RESIDUAL, STATED: a hypothetical all-uppercase-alnum credential of <= 32 chars
// in a symbol column would be admitted. No such shape exists among the brokers
// this library speaks to, and the alternative (redacting the instrument) blinds
// the operator to WHICH position is naked — the same "a missing id is worse than
// the residual" trade IMP-15 made. A symbol carrying any other byte (lowercase,
// '&' as in "M&M", '.', ' ') is NOT symbol-shaped; short ones still render via
// the scrub fallback, and anything the block grammar cannot carry is redacted.
inline constexpr std::size_t kMaxInstrumentSymbolChars = 32;

// True iff `value` — as a WHOLE typed column — is an instrument symbol shape:
// non-empty, at most kMaxInstrumentSymbolChars bytes, every byte in [A-Z0-9].
// Never throws.
[[nodiscard]] bool is_instrument_symbol_shape(std::string_view value) noexcept;

// Redact one TYPED SYMBOL COLUMN: verbatim iff it is a symbol shape, otherwise
// the ordinary scrub. The symbol-side twin of scrub_provenance_column().
[[nodiscard]] std::string scrub_symbol_column(std::string_view value);

// ── Structured provenance rendering for FREE-FORM bodies (IMP-16) ────────────
//
// IMP-15 kept typed provenance alive in the AUDIT LOG, whose fields are all
// typed. Alerts (ports::AlertSink) and the ledger (ledger::Ledger) take a
// FREE-FORM body instead, and a free-form body must NEVER get a substring
// exemption — the allowlist charset is deliberately the same charset a
// credential uses, so a substring rule there would disable scrub()'s bare
// high-entropy rule for every alert and every ledger payload.
//
// The answer is the shape IMP-15 already established, expressed for a body: the
// caller passes provenance as TYPED COLUMNS ALONGSIDE the body, the body is
// scrubbed EXACTLY as it was before (byte for byte, no relaxation), and the
// columns are rendered SEPARATELY through scrub_provenance_column() and appended
// AFTER that scrub. The body never gains an exemption; a column never travels
// through the free-form path.
//
// THE BLOCK GRAMMAR — ` [key=value key=value]`:
//   * The LEADING SPACE and the brackets are emitted ONLY when at least one field
//     survives. With nothing to render the result is the EMPTY STRING, so
//     `body + render_provenance_block(...)` is BYTE-IDENTICAL to `body` and a
//     caller that supplies no provenance is indistinguishable from the
//     pre-IMP-16 path (this is what makes the change non-breaking, and for the
//     ledger it is what makes an empty context hash-identical to a plain append).
//   * A field with an EMPTY value is omitted entirely — an absent id is never
//     rendered as a dangling `key=`.
//   * Each value goes through scrub_provenance_column() (or, for a Symbol field,
//     scrub_symbol_column()): emitted verbatim iff the WHOLE value matches that
//     column's shape, otherwise scrubbed. A token parked in an id column is still
//     redacted, exactly as IMP-15 requires.
//   * UNFORGEABLE STRUCTURE (fail closed) — AN ALLOWLIST, NOT A DENYLIST. THIS IS
//     LOAD-BEARING; DO NOT "OPTIMIZE" IT BACK INTO A LIST OF BANNED BYTES. A
//     rendered value is emitted ONLY if every one of its bytes is in the
//     provenance-id charset [A-Za-z0-9_#-] or is the '*' of kRedactionMarker;
//     anything else replaces the WHOLE value with kRedactionMarker. It is
//     additionally bounded by kMaxProvenanceIdChars.
//
//     WHY AN ALLOWLIST. `broker_order_id` is BROKER-CONTROLLED and scrub() passes
//     every byte it does not recognise through UNCHANGED, so a denylist of the
//     four ASCII bytes that spell the grammar ('[', ']', '=', <= 0x20) let EVERY
//     byte >= 0x7F reach the operator verbatim. Unicode has substitutes for the
//     blocked ASCII: U+00A0 NO-BREAK SPACE for the field separator, U+2028 LINE
//     SEPARATOR for the newline, U+FF3D FULLWIDTH ']' and U+202E RIGHT-TO-LEFT
//     OVERRIDE for the terminator. With those a broker could compose ARBITRARY
//     MULTI-WORD, MULTI-LINE PROSE that renders as extra lines in Telegram and in
//     most log viewers — one Critical alert made to look like two, or a forged
//     second provenance field. The allowlist ends that class of attack outright
//     rather than chasing code points, and it costs nothing legitimate: an
//     id-shaped value, a symbol-shaped value and kRedactionMarker are all inside
//     it by construction.
//
//     WHY BOUNDED HERE TOO. kMaxProvenanceIdChars bounds is_provenance_id_shape,
//     but the SCRUB FALLBACK was unbounded, and scrub() only redacts runs mixing
//     letters AND digits — so an all-digit or all-letter value of ANY length was
//     emitted in full. Broker order ids ARE numeric: a 5000-digit id produced a
//     5019-byte block, which overruns Telegram's 4096-char sendMessage limit, so
//     the POST fails and on a Telegram-only deployment deliver() returns Network
//     on every channel and the CRITICAL ALERT NEVER REACHES THE OPERATOR. That is
//     broker-controlled suppression of the fail-closed escalation path, so the
//     bound sits in the SAME guard as the charset check and applies to both
//     branches. The same unbounded bytes also entered the ledger hash preimage.
//   * Keys are first-party literals, but they are held as a view here and the
//     renderer is public API, so a dynamic key could break the grammar too: a key
//     is emitted only if every byte is in the provenance-id charset, and the whole
//     FIELD is dropped otherwise (a value we cannot attribute is worse than no
//     field). Keys are not scrubbed — they are not caller data.
//
// KNOWN LIMITATION, UNENFORCED BY DESIGN — THE BLOCK IS NOT AUTHENTICATED. The
// grammar is unforgeable from a COLUMN, not from the BODY. `body` is scrubbed but
// not restricted, so a body may legitimately spell a block-shaped substring
// (`... [client_ref=alpha-1]`), and a reader that greps for ` [k=v]` cannot tell
// that substring from the block this function appended. The block's authenticity
// therefore rests on an INVARIANT ON CALLERS, not on a check here: NO PRODUCTION
// BODY MAY CARRY BROKER- OR STRATEGY-SUPPLIED TEXT.
//
//   IT NO LONGER HOLDS "BY INSPECTION" — IMP-19 ENDED THAT, AND THE OLD WORDING
//   HERE IS STALE. Almost every body is still a first-party literal plus
//   integers/enums, but session::require_valid_strategy_names
//   (src/session/safe_start.cpp) builds an Error message that EMBEDS
//   CONFIG-SUPPLIED TEXT: the offending strategy name, the offending segment, and
//   the suggested spelling. It is safe — but by a CHECK, not by inspection.
//   domain::explain_invalid_strategy_name truncates the echo to
//   kMaxStrategyNameChars and reduces every byte outside [A-Za-z0-9_-] to '?'
//   before embedding it, so what reaches the body is inside the block charset by
//   construction (brute-forced in redaction_test.cpp). ANY NEW BODY THAT ECHOES
//   CALLER TEXT MUST DO THE SAME; nothing here will catch it if it does not.
//
// Making it structural would mean escaping or rejecting '[' in the body, which
// changes every alert's text; it is deliberately deferred rather than assumed.
//
// NOT idempotent under a second scrub: the emitted block is deliberately outside
// the scrub, so re-running scrub() over a rendered body WOULD redact the ids
// again. Render LAST — this is the final step before the body leaves the process.
struct ProvenanceField {
  // WHICH whole-column shape rule this field's value is measured against. An id
  // column (client_ref, broker_order_id, strategy) and an instrument symbol have
  // genuinely different shapes — see is_instrument_symbol_shape — and sharing one
  // rule would destroy one of them. Defaulted so every existing `{key, value}`
  // brace-init keeps meaning exactly what it meant before.
  enum class Kind { Id, Symbol };

  std::string_view key;
  std::string_view value;
  Kind kind = Kind::Id;
};

// Render `fields` as the block described above, or "" when every value is empty.
// The single definition of the rendering, shared by every free-form consumer so
// two sinks can never drift. Never throws as part of its logic (only
// std::bad_alloc, as with any string operation).
[[nodiscard]] std::string render_provenance_block(std::initializer_list<ProvenanceField> fields);

}  // namespace broker_exec::domain
