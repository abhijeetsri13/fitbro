#pragma once

// broker_exec::brokerreason — the VERSIONED CANONICAL rejection/status classifier
// (broker-neutral, fail-closed safety hardening).
//
// WHY THIS EXISTS: brokers (Kite, Kotak Neo, ...) reject orders with FREE-TEXT
// messages that drift over time and differ across brokers, and they report order
// status with under-documented, inconsistent codes (e.g. Kotak `ordSt`). Naive
// string matching scattered through the codebase produces WRONG retry decisions:
// blind retries that DUPLICATE live orders, freeze-quantity/circuit/margin
// rejects mishandled, expired sessions mistaken for transient glitches. The fix
// is ONE versioned table that maps raw broker rejection text + status to a small
// CANONICAL reason vocabulary and a deliberate RETRY POSTURE — with a FAIL-CLOSED
// default: anything we cannot classify is treated as a mutating action that must
// NOT be blindly resent (do-not-retry + reconcile + alert), NEVER as safe.
//
// THE CORE SAFETY PROPERTY (read this twice): an unclassifiable or empty reject
// must NEVER resolve to "safe to retry". Unknown ⇒ DoNotRetry + alert. An
// unrecognized STATUS ⇒ Indeterminate + ReconcileFirst (force a reconcile rather
// than guess). Adding/relaxing a mapping is a deliberate, reviewed change; the
// default must always stay on the safe (do-not-resend) side.
//
// REDACTION: `raw_message` is UNTRUSTED broker text and may embed account- or
// token-shaped data. This module NEVER retains or echoes the raw text. The
// `canonical_detail` it emits is built only from canonical reason/posture words
// and is additionally run through `domain::scrub` (defense in depth). Nothing
// token-shaped from the input can survive into a log line or Error.
//
// SCOPE: this is a pure-decision module. It does NOT own the canonical
// `OrderState` mapping (that lives in the per-broker adapters); it is the
// broker-neutral *reason + posture* classifier and the *unknown-status ⇒
// reconcile* fail-safe. It performs no I/O.
//
// Conventions: namespace broker_exec::brokerreason; no-throw across the boundary
// (may allocate a std::string — only std::bad_alloc could escape, as with any
// std::string op); NO float; integer enums only. Cross-platform: C++20 standard
// library only — NO OS APIs, NO `#ifdef`.

#include <string>
#include <string_view>

#include "broker_exec/errors/error.hpp"

namespace broker_exec::brokerreason {

// Explicit table version. Broker reject strings change over time, so the mapping
// table is versioned: bump this whenever the keyword table or its postures
// change, so logs/telemetry can attribute a classification to a known table
// revision. Renames of the to_string names below are likewise breaking.
inline constexpr int kClassifierVersion = 1;

// The canonical, broker-neutral reason vocabulary. Strategy/runtime code switches
// on THIS, never on raw broker text. Renaming a returned to_string name is a
// breaking observability change.
enum class RejectReason {
  Margin,           // margin shortfall / insufficient funds (RMS margin reject)
  CircuitLimit,     // price outside the day's band (circuit / LPP / DPR / range)
  FreezeQuantity,   // qty above the exchange freeze limit — caller re-routes to slicer
  Illiquid,         // no liquidity / no trades in this instrument
  RmsBlock,         // broker RMS refused on risk grounds (instrument/account blocked)
  SessionExpired,   // token/session invalid or expired — re-establish the session
  RateLimited,      // broker throttled us (429 / too many requests) — back off
  OrderNotFound,    // modify/cancel referenced an unknown/invalid order id
  AlreadyComplete,  // the order is already terminal (complete/executed/cancelled)
  Indeterminate,    // AMBIGUOUS whether the order reached the exchange — the
                    // DUPLICATE-ORDER HAZARD (timeout / OMS gateway error). Never
                    // blindly retry; reconcile the orderbook first.
  Unknown           // we could NOT classify the text at all — fail closed.
};

// The deliberate retry posture for a classified reject. This is the contract the
// runtime acts on; it maps onto errors::SuggestedAction via to_suggested_action.
enum class RetryPosture {
  DoNotRetry,          // a MUTATING action that must NOT be blindly resent.
  ReconcileFirst,      // state is ambiguous — reconcile the orderbook before any
                       // decision (the no-duplicate-order safeguard).
  SafeToRetryReadOnly  // a READ that timed out may be retried (with backoff; the
                       // caller throttles). NEVER granted to a mutating reject.
};

// The classifier result. `canonical_detail` is SCRUBBED + redaction-safe: it
// names the canonical reason/posture only and NEVER contains any part of the raw
// broker text. `should_alert` flags conditions an operator should see (margin,
// circuit, freeze, RMS, session, indeterminate, and every Unknown fail-closed);
// transient expected conditions (rate-limit, already-complete) do not alert.
struct Classification {
  RejectReason reason = RejectReason::Unknown;
  RetryPosture posture = RetryPosture::DoNotRetry;  // fail-closed default
  bool should_alert = true;                         // fail-closed default
  std::string canonical_detail;                     // scrubbed; no raw broker text
};

// Stable, log/serialization-friendly names (observability contract).
[[nodiscard]] std::string_view to_string(RejectReason reason) noexcept;
[[nodiscard]] std::string_view to_string(RetryPosture posture) noexcept;

// Bridge the local posture onto the library-wide errors::SuggestedAction so the
// classifier output drops cleanly into a typed Error. DoNotRetry⇒DoNotRetry,
// ReconcileFirst⇒ReconcileFirst, SafeToRetryReadOnly⇒RetrySafe.
[[nodiscard]] errors::SuggestedAction to_suggested_action(RetryPosture posture) noexcept;

// Classify a raw broker REJECTION message into a canonical reason + retry posture
// via a case-insensitive keyword/substring scan over the versioned internal
// table. Does not throw across the boundary (may allocate the detail string).
//
// FAIL-CLOSED: EMPTY or UNMATCHED text ⇒ {Unknown, DoNotRetry, should_alert}.
// An unclassifiable reject is NEVER treated as safe-to-retry. This is the key
// safety default and the reason this module exists.
[[nodiscard]] Classification classify_rejection(std::string_view raw_message);

// Classify a raw broker order-STATUS string (Kite COMPLETE/REJECTED/CANCELLED/
// OPEN/UPDATE/TRIGGER PENDING/...; Kotak `ordSt` variants). This is intentionally
// focused: the canonical OrderState mapping itself lives in the adapters. The
// load-bearing behavior here is the fail-safe — any UNRECOGNIZED (or empty)
// status ⇒ {Indeterminate, ReconcileFirst, alert} so the caller forces a
// reconcile rather than guessing an order's fate.
[[nodiscard]] Classification classify_status(std::string_view raw_status);

}  // namespace broker_exec::brokerreason
