#pragma once

// broker_exec::intentlog — the write-ahead intent log (Story 1.5, FR-8, NFR-1).
//
// Every order intent is durably recorded BEFORE any broker send: dispatch()
// (Story 1.9) appends a record, the record is fsync'd to stable storage, and
// only then is the socket write attempted. This is what makes "a crash never
// produces a duplicate or an un-enumerable order" structurally true — on boot we
// replay the log head→tail, rebuild the in-memory client_ref index, and
// enumerate every order that might have been sent.
//
// Integrity: records form a SHA-256 hash chain. Each record's `prev_hash` is the
// previous record's `hash` ("GENESIS" for the first), and `hash` is SHA-256 over
// this record's canonical bytes (which include `prev_hash`). Tampering with any
// past record breaks the chain and is detected on replay, naming the first bad
// seq. See the canonical-serialization contract in intent_log.cpp — it is stable
// and consumed by Story 1.7 (client-ref idempotency) on replay.
//
// UTF-8 CANONICAL BY CONSTRUCTION (IMP-17): the two CALLER-SUPPLIED byte strings
// in a record — `client_ref` and `payload_json` — are normalised to valid UTF-8
// (ill-formed sequences -> U+FFFD, one per maximal subpart) EXACTLY ONCE, at the
// top of append(), BEFORE they become either the hash preimage or the stored JSON.
// The two are therefore THE SAME BYTES and cannot diverge. This closes a THROW ON
// THE ORDER HOT PATH: nlohmann's default dump handler is `strict` and raises
// json::type_error.316 on the first ill-formed byte, which would escape append()'s
// no-throw Result<T> boundary between "decided to trade" and "durably recorded
// that we decided". NORMALISE, NOT REJECT — an untrusted byte (a strategy name, an
// instrument symbol) must never be able to fail an order's write-ahead record.
// Valid UTF-8 (ASCII or multi-byte) passes through byte-identical, so NO existing
// record's hash moves and this is NOT a schema-version bump.
//
// The normaliser is broker_exec::domain::canonical_text
// (include/broker_exec/domain/utf8.hpp), SHARED with ledger::Ledger, which has the
// identical preimage/stored-bytes duality over its own hash chain. Exactly ONE
// implementation exists in the tree, on purpose.
//
// Durability seam: this module uses C `std::FILE*` (opened "ab"/"rb") and the
// portable `platform::durable_sync` / `platform::portable_fileno` primitives, so
// the single fsync on the append hot path works identically on every OS. No OS
// APIs, no `#ifdef` here (binding cross-platform convention).
//
// Cross-platform: C++20 standard library + nlohmann_json + the vendored SHA-256
// only. Paths via std::filesystem. No OS APIs, no `#ifdef`.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "broker_exec/ports/clock_port.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::intentlog {

// The kinds of intent recorded. Stable names — they appear verbatim in the
// on-disk JSON and are part of the canonical hashed bytes, so renaming a value
// (or its string) is a breaking change to the log format.
enum class IntentOp { PlaceOrder, ModifyOrder, CancelOrder, SquareOff, Result, ChildSlice };

// Stable lowercase-ish wire names for IntentOp (used in the JSON line AND in the
// canonical hash input). Renaming is a breaking format change.
[[nodiscard]] std::string_view to_string(IntentOp op) noexcept;

// Parse a wire name back to IntentOp. Returns std::nullopt on an unknown name
// (e.g. a record from a newer/foreign log). Used by replay.
[[nodiscard]] std::optional<IntentOp> intent_op_from_string(std::string_view name) noexcept;

// The current on-disk schema version. Bump on any change to the record shape or
// the canonical serialization.
inline constexpr int kSchemaVersion = 1;

// One appended intent. Written as a single JSON line; the integrity fields
// (prev_hash/hash) are filled by IntentLog::append(), never by the caller.
// The two caller-provided fields are stored in their UTF-8-CANONICAL form (see
// the IMP-17 note above): what append() returns is what was hashed AND what is on
// disk, so a record round-trips byte-identically through replay().
struct IntentRecord {
  int schema_version = kSchemaVersion;
  std::int64_t seq = 0;       // monotonic, starts at 1
  std::string client_ref;     // caller-provided (canonicalised); idempotency key (1.7)
  IntentOp op = IntentOp::PlaceOrder;
  std::string payload_json;   // opaque JSON object as a string; caller-provided (canonicalised)
  std::int64_t wall_ts_ns = 0;  // wall-clock ns since epoch, from ClockPort
  std::string prev_hash;      // hex SHA-256 of previous record's hash, or "GENESIS"
  std::string hash;           // hex SHA-256 over this record's canonical content
};

// The append-only, fsync-on-write, hash-chained intent log.
//
// Lifecycle: open() (does NOT auto-replay) → replay() once on boot to rebuild
// the index → append()/last_for() during operation. open() and replay() are
// fallible (Result<T>); append() is fallible (I/O). last_for()/next_seq() are
// pure in-memory lookups.
class IntentLog {
 public:
  IntentLog(const IntentLog&) = delete;
  IntentLog& operator=(const IntentLog&) = delete;
  IntentLog(IntentLog&&) noexcept;
  IntentLog& operator=(IntentLog&&) noexcept;
  ~IntentLog();

  // Open (creating the file if absent) at `path`; `clock` supplies append
  // timestamps and must outlive this log. Does NOT auto-replay — call replay()
  // on boot before append() to rebuild the index from any existing records.
  [[nodiscard]] static Result<IntentLog> open(std::filesystem::path path,
                                              ports::ClockPort& clock);

  // Append one intent. Normalises `client_ref` and `payload_json` to valid UTF-8
  // (IMP-17 — see the note at the top of this file), fills
  // seq/wall_ts_ns/prev_hash/hash, serializes one JSON line, fflush +
  // durable_sync (the single fsync on the hot path), THEN returns — so the caller
  // may socket-send only after durability. Updates the in-memory index and
  // next_seq. Returns the fully-populated record, carrying the CANONICAL text.
  //
  // NEVER THROWS, and never fails on encoding. Every failure is a Result Error
  // (I/O, moved-from log); an ill-formed UTF-8 byte in either caller string is
  // normalised and RECORDED, never rejected and never propagated as an exception.
  [[nodiscard]] Result<IntentRecord> append(IntentOp op, std::string client_ref,
                                            std::string payload_json);

  // Replay head→tail. Verifies the SHA-256 hash chain; on a broken chain returns
  // an Error (ErrorCategory::Internal) naming the first bad seq. On success
  // returns every record in order and rebuilds the in-memory client_ref index +
  // next_seq. Safe to call exactly once on boot (before the first append).
  [[nodiscard]] Result<std::vector<IntentRecord>> replay();

  // The latest record seen for `client_ref` (after replay/append), if any. The
  // query is canonicalised the same way append() canonicalises the stored ref, so
  // a caller that appended with an ill-formed ref and asks with the same bytes
  // still finds its own record (identity for every valid-UTF-8 ref).
  [[nodiscard]] std::optional<IntentRecord> last_for(std::string_view client_ref) const;

  // The seq the next append() will assign (1 on an empty log). Diagnostics only.
  [[nodiscard]] std::int64_t next_seq() const noexcept { return next_seq_; }

 private:
  IntentLog(std::FILE* file, std::filesystem::path path, ports::ClockPort& clock) noexcept;

  std::FILE* file_ = nullptr;
  std::filesystem::path path_;
  ports::ClockPort* clock_ = nullptr;
  std::int64_t next_seq_ = 1;
  std::string last_hash_;  // hash of the most recent record, "" ⇒ next prev is GENESIS
  std::unordered_map<std::string, IntentRecord> index_;  // client_ref → latest record
};

}  // namespace broker_exec::intentlog
