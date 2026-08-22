#pragma once

// broker_exec::sessionguard — the MID-SESSION RE-AUTH GUARD (fail-closed auth
// safety hardening).
//
// WHY THIS EXISTS: a Kite access token is not just a START-OF-DAY concern. It is
// flushed daily AND can be invalidated MID-SESSION — a second login elsewhere, a
// password change, an operator revoke — at which point the broker answers a live
// trading call with TokenException / 401 / "token is invalid or has expired".
// An engine that keeps PLACING ENTRIES against a dead token does the worst thing
// possible: it crash-loops, or it strands/duplicates positions while blindly
// resending. Kite has NO headless refresh, so the ONLY correct response is to
// flip the posture to NeedsReauth, FREEZE new entries, raise the operator alert,
// and wait for a human re-establish — never auto-retry the auth itself.
//
// THE LOAD-BEARING INVARIANT (read this twice): EXITS AND RECONCILE READS STAY
// ALLOWED EVEN WHEN RE-AUTH IS NEEDED. Freezing everything would TRAP an open
// position behind the dead token — the single most dangerous outcome. An exit is
// risk-REDUCING (it can only unwind exposure, never open new), and a reconcile
// read is how the engine learns the true book to act on post-reauth; both are
// retried safely once the session is re-established. So the freeze is surgical:
// it stops new RISK (entries) and nothing else. We also NEVER blindly retry the
// auth: a dead token is a normalized STATE the operator clears, not an Error to
// loop on.
//
// HOW IT DETECTS: this module owns no transport. It is fed the caller's last
// known session state plus the raw broker error TEXT seen on a mid-session call,
// and it REUSES brokerreason::classify_rejection as the single auth-failure
// signal — a reject that classifies to RejectReason::SessionExpired means the
// token is auth-dead, regardless of which broker phrased it. That keeps the
// "what counts as an auth failure" decision in the ONE versioned classifier
// table instead of a second, drifting copy of "token"/"401"/"unauthorized".
//
// REDACTION: `broker_error_text` is UNTRUSTED broker text and may embed a
// token-shaped secret. This module NEVER embeds it in the emitted posture detail
// or in any Error — the detail is built only from the canonical SessionState
// name and is additionally run through domain::scrub (defense in depth).
//
// SCOPE: a pure-decision module mirroring the sibling `modes` gate — enum +
// to_string + a no-throw assess/require pair, no I/O. It does not perform the
// re-auth (that is the operator/establisher path); it decides the POSTURE and
// gates ops against it.
//
// Conventions: namespace broker_exec::sessionguard; no-throw (Result<Ok>); NO
// float; integer enums only. Cross-platform: C++20 standard library only — NO OS
// APIs, NO `#ifdef`. Depends inward on `session` (SessionState in the public
// header), `errors` (SessionExpired/Validation + ReEstablishSession), and
// `ports` (Ok); the brokerreason classifier is an implementation detail of the
// .cpp. Fail-closed throughout: a defaulted posture FREEZES entries.

#include <string>
#include <string_view>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/session/session_state.hpp"

namespace broker_exec::sessionguard {

// The kind of operation the guard is asked to gate. The distinction is the whole
// point: only an Entry is frozen on a re-auth posture; an Exit reduces risk and a
// ReconcileRead is needed to recover state, so both are ALWAYS allowed.
// Renaming a returned to_string name is a breaking observability change.
enum class OpClass {
  Entry,         // open / add to a position — new RISK; the ONLY class ever frozen
  Exit,          // risk-REDUCING unwind — always allowed (can only reduce exposure)
  ReconcileRead  // a read to learn the true book — always allowed (retried post-reauth)
};

// The resolved mid-session auth posture. Derived from the (possibly
// auth-overridden) effective SessionState. The booleans are the contract the
// runtime acts on; `detail` is redaction-safe and names the state only.
//
// FAIL-CLOSED DEFAULTS: a default-constructed SessionPosture is the MOST
// restrictive safe posture — Failed, entries frozen, alert raised — so a
// forgotten assignment freezes new risk rather than silently opening it. Exits
// and reconcile reads default to allowed because withholding them would strand an
// open position; the safe default for risk-REDUCING work is "permitted".
struct SessionPosture {
  session::SessionState state = session::SessionState::Failed;  // effective state
  bool freeze_entries = true;         // true unless Healthy — stop new RISK
  bool allow_exits = true;            // exits stay allowed even on NeedsReauth/Failed
  bool allow_reconcile_reads = true;  // reads stay allowed (retried post-reauth)
  bool alert = true;                  // raise the operator alert on a non-Healthy state
  std::string detail;                 // redaction-safe; names the state, never the raw text
};

// Stable, log/serialization-friendly names (observability contract).
[[nodiscard]] std::string_view to_string(OpClass op) noexcept;

// Classify a broker response/error seen MID-SESSION into a posture.
//
//   effective state = classify_rejection(broker_error_text) == SessionExpired
//                       ? NeedsReauth         // the broker says the token is dead
//                       : current;            // otherwise trust the caller's last state
//
// So an auth-dead reject OVERRIDES a caller who still thinks it is Healthy (that
// is the mid-session death this module exists to catch); any non-auth reject
// (margin, circuit, ...) leaves the posture on `current` — a margin reject is not
// an auth failure and must NOT freeze entries. The per-state mapping:
//
//   Healthy     -> freeze_entries=false, allow_exits=true, allow_reconcile_reads=true,
//                  alert=false  (everything flows).
//   NeedsReauth -> freeze_entries=TRUE,  allow_exits=true, allow_reconcile_reads=true,
//                  alert=true   (auth dead: stop new entries, but keep exits/reads so a
//                  position can still be unwound and the book reconciled post-reauth).
//   Failed      -> freeze_entries=TRUE,  allow_exits=true, allow_reconcile_reads=true,
//                  alert=true   (fail-closed; exits still permitted as the safest action).
//
// `detail` names the effective state only and is scrubbed — it NEVER echoes
// broker_error_text (which may carry a token). No-throw across the boundary (may
// allocate the detail string).
[[nodiscard]] SessionPosture assess_session(session::SessionState current,
                                            std::string_view broker_error_text);

// The GATE CHOKEPOINT: authorize one operation against a posture.
//   * Exit and ReconcileRead are ALWAYS ok() — a re-auth posture NEVER blocks
//     them (an exit reduces risk; a read is needed to reconcile). This holds even
//     under Failed / a defaulted posture.
//   * Entry is ok() ONLY when !freeze_entries (i.e. a Healthy posture). When
//     entries are frozen it returns a typed Error naming the op and instructing a
//     session re-establish (category SessionExpired, action ReEstablishSession),
//     redaction-safe, no secrets.
[[nodiscard]] Result<ports::Ok> require_op_allowed(const SessionPosture& posture, OpClass op);

// True iff the posture's effective state is NeedsReauth (the operator must
// re-establish the session). Distinct from Failed, which is a non-auth fail-closed
// freeze.
[[nodiscard]] bool is_reauth_needed(const SessionPosture& posture) noexcept;

}  // namespace broker_exec::sessionguard
