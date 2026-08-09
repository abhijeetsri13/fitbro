#pragma once

// broker_exec::idempotency — client-ref generation and idempotency (Story 1.7,
// FR-7, NFR-1, NFR-3).
//
// WHAT THIS IS: the layer that makes "a repeated request never creates a second
// order" structurally true. It mints a unique, deterministic-format client-ref
// per order *signal*, detects a duplicate signal via a stable signature hash +
// an in-memory index, and composes with the `UNIQUE(client_ref)` backstop in the
// SQLite projection (Story 1.6) so dedup survives a restart.
//
// THE THREE LAYERS OF DEDUP (defense in depth):
//   1. Signal signature  — a deterministic SHA-256 over the order-defining
//      fields of an OrderIntent. The same signal always hashes to the same hex,
//      so a duplicate strategy signal is caught BEFORE a client-ref is minted.
//   2. In-memory index   — signature -> client_ref, rebuilt from the intent log
//      on boot (rebuild_from_log) so restart-dedup works without the DB.
//   3. UNIQUE(client_ref) — the store's hard backstop: even if (1) and (2) are
//      somehow bypassed, inserting a duplicate client_ref fails closed.
//
// CLIENT-REF FORMAT (binding — Stories 1.8/2.9 parse these):
//   parent : "<strategy>-<sig8>-<uuid>"   sig8 = first 8 hex chars of signature
//   child  : "<parent>#<k>"               k >= 1, deterministic slice ref
// The child form is reserved here for the freeze-slicer (Story 2.9) and the FSM
// (Story 1.8); is_child_ref / parent_of let the FSM recover a parent from a child.
//
// REBUILD-FROM-LOG ASSUMPTION (documented, not guessed — see rebuild_from_log):
// the intent-log `payload_json` is OPAQUE to this module (the log imposes no
// schema). To recompute a signature on replay we therefore define a canonical
// PlaceOrder payload here (intent_payload_json) that the live dispatch path is
// expected to use. rebuild_from_log keys on what is reliably recoverable from
// that payload and skips records it cannot interpret (it never guesses).
//
// CROSS-PLATFORM: C++20 stdlib + nlohmann_json (payload parse) + the vendored
// SHA-256 from broker_exec::intentlog. UUID v4 via <random> (std::random_device
// seeding a std::mt19937_64) — never time-based. No OS APIs, no `#ifdef`.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "broker_exec/domain/types.hpp"
#include "broker_exec/intentlog/intent_log.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/store/store.hpp"

namespace broker_exec::idempotency {

// ── Signal signature ───────────────────────────────────────────────────────

// Deterministic SHA-256 signature (lowercase 64-char hex) of an order *signal*.
//
// It hashes the stable, order-defining fields of the intent — strategy, symbol,
// side, quantity, price, order_type, product, and the TRIGGER price when the
// intent carries one — in a fixed canonical form, so the SAME signal always
// produces the SAME signature and any one field change yields a different one.
// The client_ref is deliberately EXCLUDED (it is the output of idempotency, not
// an input) as is any non-order-defining field. Reuses intentlog::sha256_hex;
// this is an integrity/dedup hash, not a secret.
//
// UPGRADE COMPATIBILITY (IMP-11) — WHAT IS PRESERVED, AND WHAT IS NOT.
//
// PRESERVED — every NON-STOP order (Market / Limit). The trigger contributes to
// the canonical bytes ONLY when present, so an intent without one hashes to
// exactly the signature the pre-IMP-11 build produced. A replayed pre-upgrade
// Market/Limit signal still matches its own intent-log record, so the restart
// dedupe holds. A golden hex literal in idempotency_test.cpp pins this; if it
// ever fails, the legacy dedupe has silently broken.
//
// NOT PRESERVED — STOP orders (SL / SL-M). SAY IT PLAINLY: a pre-IMP-11 stop
// carried its activation level in `price`, because there was nowhere else to put
// it. The same economic order now carries that level in `trigger_price` (and an
// SL-M's `price` is canonicalized to 0, since neither adapter transmits it), so
// its signature NECESSARILY changes across the upgrade. THE CONSEQUENCE IS REAL:
// a working stop placed by the old binary, replayed by the new one, will not
// match its own record — reserve() sees a fresh signal and the order can be
// PLACED A SECOND TIME. No signature scheme can avoid this; the two builds
// genuinely disagree about which field holds the stop level.
//
// The mitigation is therefore operational, not cryptographic, and it is enforced
// at COLD BOOT rather than left to a release note: session::require_no_legacy_stops
// (wired as SafeStartContext::legacy_stop_check) refuses to start when the
// projection still holds a WORKING stop with no trigger — the exact fingerprint
// of a pre-upgrade stop — and tells the operator to flatten or cancel stops
// first. See docs/upgrade-imp-11-stops.md for the procedure and its rollback.
//
// Two stops that differ only in trigger level are (correctly) different signals.
[[nodiscard]] std::string signal_signature(const domain::OrderIntent& intent);

// The first 8 hex chars of a signature (the `sig8` embedded in a client-ref).
// `signature_hex` must be a hex signature as returned by signal_signature.
[[nodiscard]] std::string sig8_of(std::string_view signature_hex);

// The canonical PlaceOrder payload JSON for the intent log. The live dispatch
// path SHOULD record this as the PlaceOrder `payload_json` so that
// rebuild_from_log can recover the signal on replay. It is a compact JSON object
// carrying the order-defining fields AND the precomputed "sig" (so rebuild does
// not need to reconstruct domain types). See rebuild_from_log for how it is read.
//
// SCHEMA-VERSION TOLERANT (IMP-11): `trigger_price_paise` is emitted only when the
// intent has a trigger, and the payload schema marker is UNCHANGED — a record
// written before the field existed simply lacks the key and replays to nullopt.
// (Bumping the marker instead would make rebuild_from_log skip every committed
// pre-IMP-11 record, emptying the dedupe index on the first boot after upgrade.)
//
// BINDING ON EVERY SCHEMA-1 READER: `trigger_price_paise` is OPTIONAL. A reader
// must treat its absence as "no trigger" and MUST NOT reject or skip a record for
// lacking it — schema 1 now spans both spellings, in both directions (an older
// binary reading a newer record must likewise ignore the unknown key, not fail).
//
// NOTE ON RECOMPUTING `sig` FROM THIS PAYLOAD: `price_paise` records the intent's
// literal price, INCLUDING for an SL-M where the signature canonicalizes it to 0.
// A recomputing reader must apply that rule itself; `order_type` is in the
// payload precisely so it can.
[[nodiscard]] std::string intent_payload_json(const domain::OrderIntent& intent);

// ── Client-ref construction / parsing ──────────────────────────────────────

// Build a parent client-ref: "<strategy>-<sig8>-<uuid>". `signature_hex` is a
// full hex signature (sig8 is taken as its first 8 chars); `uuid` is a canonical
// 8-4-4-4-12 lowercase-hex UUID (from a UuidGenerator).
//
// ── THE STRATEGY CHARSET IS BINDING ON CALLERS, AND CURRENTLY UNENFORCED ──────
//
// `strategy` is concatenated in AS-IS. Nothing here (and nothing anywhere else in
// this library) constrains it, so a caller can put any bytes into every client_ref
// it mints. That has a REAL and non-obvious cost downstream, because the resulting
// ref is what an operator alert and the ledger must be able to name:
//
//   REQUIRED: `strategy` should be drawn from [A-Za-z0-9_-] AND each of its
//   '-'/'_'-separated segments should be HOMOGENEOUS — all letters, or all
//   hex/digits. Not "nice to have": domain::is_provenance_id_shape (redaction.hpp)
//   admits a whole client_ref into an alert or ledger block ONLY if EVERY segment
//   is homogeneous, and the strategy name is the ref's FIRST segment.
//
//   CONSEQUENCE OF VIOLATING IT — the ref, not just the name, is destroyed:
//     * "alpha", "ironcondor", "conformance" (all letters)  -> ref survives intact.
//     * "S1", "strat2", "v2beta" (letter+digit in ONE run)  -> the ref is NOT
//       id-shaped, falls back to domain::scrub(), and a ~50-char run mixing letters
//       and digits is exactly what scrub() redacts. EVERY alert about an order from
//       that strategy names it `client_ref=***REDACTED***`. Spell it "S-1" or
//       "strat-2" and it is fine again.
//     * a SPACE ("iron condor v2") -> additionally rejected wholesale by the block
//       grammar guard, so `strategy=***REDACTED***` too.
//
// NOT ENFORCED HERE, DELIBERATELY. The only boundary that could reject a bad name
// is reserve()/Dispatcher::place(), where rejecting means REFUSING TO PLACE AN
// ORDER — a trading-behaviour change that needs its own story and its own operator
// migration, not a silent side effect of a redaction fix. Until then this contract
// is documentation plus a pinning test (redaction_test.cpp, "DOCUMENTED, PINNED:
// an ordinary strategy name is WHOLLY REDACTED in every block").
[[nodiscard]] std::string make_client_ref(std::string_view strategy,
                                          std::string_view signature_hex, std::string_view uuid);

// Deterministic child slice ref: "<parent>#<k>" with k >= 1 (k < 1 is clamped to
// 1). Re-deriving a child for the same (parent, k) is bit-identical — the basis
// for Story 2.9's bit-identical re-slicing on replay + UNIQUE(client_ref) dedupe.
[[nodiscard]] std::string child_ref(std::string_view parent_client_ref, int k);

// True iff `client_ref` is a child slice ref (contains a '#' with a non-empty
// parent and non-empty suffix on each side).
[[nodiscard]] bool is_child_ref(std::string_view client_ref) noexcept;

// The parent of a child ref ("<parent>#<k>" -> "<parent>"); "" if not a child.
// Lets the FSM (Story 1.8) recover the parent that owns a child slice.
[[nodiscard]] std::string parent_of(std::string_view child_client_ref);

// ── UUID v4 generation (injectable seam) ───────────────────────────────────

// Generates UUIDs for client-refs. Abstract so tests inject a deterministic
// generator (the production default is random_device-seeded; see RandomUuidGenerator).
class UuidGenerator {
 public:
  UuidGenerator() = default;
  UuidGenerator(const UuidGenerator&) = default;
  UuidGenerator& operator=(const UuidGenerator&) = default;
  UuidGenerator(UuidGenerator&&) = default;
  UuidGenerator& operator=(UuidGenerator&&) = default;
  virtual ~UuidGenerator() = default;

  // A canonical 8-4-4-4-12 lowercase-hex UUID v4 (version/variant bits set).
  [[nodiscard]] virtual std::string next() = 0;
};

// ── The idempotency index (signature -> client_ref) ────────────────────────

// In-memory map from a signal signature to the client_ref that was minted for
// it. Rebuilt from the intent log on boot so the same signal submitted before a
// restart still dedups. Not thread-safe by design — the engine's decision core
// is single-threaded (NFR-2); the index is touched only on the main loop.
class IdempotencyIndex {
 public:
  // Rebuild from a head→tail slice of intent-log records (typically the result
  // of IntentLog::replay()). Only PlaceOrder records are considered; each is
  // mapped signature -> client_ref by parsing its canonical payload (see the
  // rebuild assumption at the top of this header). Records whose payload cannot
  // be interpreted as a canonical intent payload are skipped (never guessed).
  // Clears any prior contents first. Returns the number of (signature->ref)
  // mappings recovered.
  std::size_t rebuild_from_log(const std::vector<intentlog::IntentRecord>& records);

  // The client_ref previously minted for this intent's signal, if any.
  [[nodiscard]] std::optional<std::string> existing_ref(const domain::OrderIntent& intent) const;

  // The client_ref previously minted for a known signature, if any.
  [[nodiscard]] std::optional<std::string> existing_ref_for_signature(
      std::string_view signature_hex) const;

  // Register a (signal -> client_ref) mapping from the live path. Idempotent:
  // re-registering the same signature keeps the first client_ref (first writer
  // wins) so a benign re-register cannot rebind a signal to a new order.
  void register_ref(const domain::OrderIntent& intent, std::string client_ref);

  // Register by an already-computed signature (avoids re-hashing).
  void register_signature(std::string signature_hex, std::string client_ref);

  [[nodiscard]] std::size_t size() const noexcept { return by_signature_.size(); }

 private:
  std::unordered_map<std::string, std::string> by_signature_;  // signature -> client_ref
};

// ── High-level reserve (the single idempotent entry point) ─────────────────

// The outcome of reserve(). Exactly one order corresponds to a given signal:
//   is_new == true  -> a fresh client_ref was minted + registered; `existing` is
//                      empty; the caller proceeds to dispatch a NEW order.
//   is_new == false -> the signal was already submitted; `client_ref` is the
//                      prior ref and `existing` carries the stored Order if the
//                      projection has it (it may be empty if the order was
//                      registered but not yet projected). The caller MUST NOT
//                      dispatch again — it returns the existing order.
struct Reservation {
  std::string client_ref;
  bool is_new{false};
  std::optional<domain::Order> existing;
};

// Reserve a client-ref for `intent` idempotently. Order of checks:
//   1. Compute the signal signature.
//   2. If the in-memory index already has it -> return existing (is_new=false),
//      attaching the stored Order from the projection if present.
//   3. Else mint "<strategy>-<sig8>-<uuid>". If that exact client_ref already
//      exists in the store (UNIQUE(client_ref) backstop / a torn restart),
//      return it as existing (is_new=false).
//   4. Else register the mapping in the index and return is_new=true.
//
// `strategy` is the owning strategy id embedded in the ref; it is normally
// `intent.strategy`. Fallible only on a store read error (propagated as-is).
[[nodiscard]] Result<Reservation> reserve(IdempotencyIndex& index, store::Store& store,
                                          UuidGenerator& uuids, std::string_view strategy,
                                          const domain::OrderIntent& intent);

}  // namespace broker_exec::idempotency
