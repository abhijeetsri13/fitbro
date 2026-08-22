#include "broker_exec/intentlog/intent_log.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "broker_exec/domain/utf8.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/platform/durable.hpp"
#include "sha256.hpp"

// Write-ahead intent log implementation (Story 1.5).
//
// ── Canonical hash serialization (the contract Story 1.7 replays against) ──
//
// The record `hash` is SHA-256 over a deterministic byte stream of the record's
// fields EXCEPT `hash` itself. The stream is field-ordered and unambiguous: each
// variable-length string is preceded by its byte length so no two distinct field
// sets can collide via concatenation. The order and framing are FROZEN — any
// change is a schema-version bump.
//
//   canonical(record) =
//     "v"  + dec(schema_version)            + "\n"
//     "s"  + dec(seq)                       + "\n"
//     "o"  + to_string(op)                  + "\n"
//     "t"  + dec(wall_ts_ns)                + "\n"
//     "c"  + dec(len(client_ref))   + ":" + client_ref   + "\n"
//     "p"  + dec(len(payload_json)) + ":" + payload_json  + "\n"
//     "h"  + dec(len(prev_hash))    + ":" + prev_hash     + "\n"
//
// `prev_hash` is the previous record's `hash`, or the literal "GENESIS" for seq
// 1. Because `prev_hash` is part of the canonical bytes, every record's hash
// transitively commits to the entire prefix of the log — tampering with any past
// record changes its hash, which breaks the next record's prev_hash linkage and
// is caught on replay.
//
// The JSON line on disk is a *projection* of the same fields (human/diff
// friendly); the hash is computed over the canonical form above, NOT over the
// JSON text, so JSON key ordering / whitespace can never affect integrity.
//
// ── UTF-8 CANONICAL BY CONSTRUCTION (IMP-17) ─────────────────────────────────
//
// The projection above only holds if the JSON writer reproduces the SAME BYTES
// the canonical form hashed, and for two of those fields — `client_ref` and
// `payload_json` — the bytes are CALLER-SUPPLIED (a strategy name, an instrument
// symbol, an operator-provided ref; see idempotency::intent_payload_json). An
// ill-formed UTF-8 byte in either one used to have two failure modes, both bad:
//
//   * WITH THE DEFAULT DUMP HANDLER (which is error_handler_t::strict, what this
//     file used) nlohmann THROWS json::type_error.316. append() returns
//     Result<IntentRecord> — a NO-THROW BOUNDARY — and sits on the ORDER DISPATCH
//     HOT PATH (runtime/dispatcher.cpp). This log is the DURABLE TRUTH, fsync'd
//     BEFORE the broker socket write, so a throw here is a crash between "decided
//     to trade" and "recorded that we decided".
//   * WITH error_handler_t::replace ALONE the throw goes away but the ill-formed
//     sequence is rewritten to U+FFFD ON THE WAY OUT, so the line on disk is not
//     the text we hashed and replay() reports "record tampered" on our own record.
//
// The fix is the SAME ONE the ledger uses, from the SAME implementation:
// domain::canonical_text() normalises caller text to valid UTF-8 EXACTLY ONCE, at
// the top of append(), BEFORE the canonical bytes are built. From there the
// hashed bytes and the written bytes are the same std::string by construction, and
// the dump (which now also passes error_handler_t::replace, belt and braces) has
// nothing left to replace. Valid UTF-8 — ASCII and multi-byte alike — is
// byte-identical through canonical_text, so NO existing record's hash moves and
// this is NOT a schema-version bump.
//
// NORMALISE, NOT REJECT: an untrusted byte must not be able to fail an order's
// write-ahead record. See include/broker_exec/domain/utf8.hpp for the contract.

namespace broker_exec::intentlog {

namespace {

using json = nlohmann::json;

constexpr std::string_view kGenesis = "GENESIS";

[[nodiscard]] errors::Error chain_error(std::string message) {
  return errors::make_error(errors::ErrorCategory::Internal, std::move(message));
}

// Append "<len>:<value>" framing for a variable-length field.
void append_framed(std::string& out, std::string_view value) {
  out += std::to_string(value.size());
  out += ':';
  out.append(value.data(), value.size());
}

// Build the canonical byte string the record hash is taken over (excludes hash).
[[nodiscard]] std::string canonical_bytes(const IntentRecord& record) {
  std::string out;
  out.reserve(64 + record.client_ref.size() + record.payload_json.size());
  out += 'v';
  out += std::to_string(record.schema_version);
  out += '\n';
  out += 's';
  out += std::to_string(record.seq);
  out += '\n';
  out += 'o';
  out += to_string(record.op);
  out += '\n';
  out += 't';
  out += std::to_string(record.wall_ts_ns);
  out += '\n';
  out += 'c';
  append_framed(out, record.client_ref);
  out += '\n';
  out += 'p';
  append_framed(out, record.payload_json);
  out += '\n';
  out += 'h';
  append_framed(out, record.prev_hash);
  out += '\n';
  return out;
}

[[nodiscard]] std::string compute_hash(const IntentRecord& record) {
  return sha256_hex(canonical_bytes(record));
}

// Serialize one record to its single-line JSON form (no embedded newlines).
[[nodiscard]] std::string to_json_line(const IntentRecord& record) {
  json j;
  j["schema_version"] = record.schema_version;
  j["seq"] = record.seq;
  j["client_ref"] = record.client_ref;
  j["op"] = std::string(to_string(record.op));
  // payload_json is opaque caller-provided text. We store it as a JSON *string*
  // (not a parsed sub-object) so replay round-trips the exact bytes back via
  // nlohmann's escape/unescape — the recomputed hash over those bytes therefore
  // always matches. Integrity is over payload_json's bytes (canonical form), so
  // this projection choice never affects the chain.
  j["payload"] = record.payload_json;
  j["wall_ts_ns"] = record.wall_ts_ns;
  j["prev_hash"] = record.prev_hash;
  j["hash"] = record.hash;
  // error_handler_t::replace, NOT the default (which is ::strict and THROWS
  // json::type_error.316 on an ill-formed UTF-8 byte — across append()'s no-throw
  // Result boundary, on the order hot path). This is BELT AND BRACES: append()
  // has already run every caller-supplied field through domain::canonical_text,
  // so by construction there is nothing here to replace and the bytes written are
  // exactly the bytes hashed. Keeping the handler anyway makes the NO-THROW
  // contract independent of the normaliser being correct — if the normaliser ever
  // regressed we would get a (loud, detectable) hash mismatch on replay instead of
  // an exception escaping mid-dispatch. `op` is a fixed ASCII enum name and the
  // hashes are hex, so no other field can reach the handler at all.
  std::string line = j.dump(-1, ' ', /*ensure_ascii=*/false, json::error_handler_t::replace);
  line.push_back('\n');
  return line;
}

// Push everything buffered for `file` all the way to stable storage. Used on
// the append hot path and by replay()'s tail repair, so that a repair we have
// already returned to the caller cannot itself be lost to the next power cut.
[[nodiscard]] bool flush_and_sync(std::FILE* file) {
  if (file == nullptr || std::fflush(file) != 0) {
    return false;
  }
  const int fd = platform::portable_fileno(file);
  return fd >= 0 && platform::durable_sync(fd);
}

}  // namespace

std::string_view to_string(IntentOp op) noexcept {
  switch (op) {
    case IntentOp::PlaceOrder:
      return "place_order";
    case IntentOp::ModifyOrder:
      return "modify_order";
    case IntentOp::CancelOrder:
      return "cancel_order";
    case IntentOp::SquareOff:
      return "square_off";
    case IntentOp::Result:
      return "result";
    case IntentOp::ChildSlice:
      return "child_slice";
  }
  return "unknown";
}

std::optional<IntentOp> intent_op_from_string(std::string_view name) noexcept {
  if (name == "place_order")
    return IntentOp::PlaceOrder;
  if (name == "modify_order")
    return IntentOp::ModifyOrder;
  if (name == "cancel_order")
    return IntentOp::CancelOrder;
  if (name == "square_off")
    return IntentOp::SquareOff;
  if (name == "result")
    return IntentOp::Result;
  if (name == "child_slice")
    return IntentOp::ChildSlice;
  return std::nullopt;
}

IntentLog::IntentLog(std::FILE* file, std::filesystem::path path, ports::ClockPort& clock) noexcept
    : file_(file), path_(std::move(path)), clock_(&clock) {}

IntentLog::IntentLog(IntentLog&& other) noexcept
    : file_(other.file_),
      path_(std::move(other.path_)),
      clock_(other.clock_),
      next_seq_(other.next_seq_),
      last_hash_(std::move(other.last_hash_)),
      torn_tail_(other.torn_tail_),
      index_(std::move(other.index_)) {
  other.file_ = nullptr;
  other.clock_ = nullptr;
}

IntentLog& IntentLog::operator=(IntentLog&& other) noexcept {
  if (this != &other) {
    if (file_ != nullptr) {
      std::fclose(file_);
    }
    file_ = other.file_;
    path_ = std::move(other.path_);
    clock_ = other.clock_;
    next_seq_ = other.next_seq_;
    last_hash_ = std::move(other.last_hash_);
    torn_tail_ = other.torn_tail_;
    index_ = std::move(other.index_);
    other.file_ = nullptr;
    other.clock_ = nullptr;
  }
  return *this;
}

IntentLog::~IntentLog() {
  if (file_ != nullptr) {
    std::fclose(file_);
  }
}

Result<IntentLog> IntentLog::open(std::filesystem::path path, ports::ClockPort& clock) {
  // Open for binary append; create if absent. "ab" positions writes at EOF and
  // never truncates an existing log. We read it back for replay() via a separate
  // "rb" stream so the append cursor is untouched.
  std::FILE* file = std::fopen(path.string().c_str(), "ab");
  if (file == nullptr) {
    return fail(chain_error("intentlog: failed to open log file for append: " + path.string()));
  }

  // ── THE DIRECTORY ENTRY IS PART OF THE DURABILITY PROMISE (IMP-14) ─────────
  //
  // append() fsyncs the file DESCRIPTOR and the dispatcher treats that return as
  // licence to hit the broker socket. On POSIX that fsync commits the file's
  // CONTENTS but NOT the parent directory entry that names a newly created file.
  // On a fresh data dir the first record can therefore be durable while the file
  // is not: after a power cut intent.log is absent or zero-length, replay() sees
  // no file, reports the empty-boot state, IdempotencyIndex::rebuild_from_log
  // rebuilds nothing — and every order that is live at the exchange is
  // un-enumerable and its signal re-fires into a SECOND live order.
  //
  // One sync of the containing directory, here, closes that. Once per open() is
  // enough: the entry only has to be CREATED durably, and every later append goes
  // to a name that is already committed. This is off the hot path (boot only), so
  // it costs nothing per order. Windows is an honest no-op inside the seam
  // (durable.hpp) — NTFS journals the metadata and _commit already covers it.
  //
  // An empty parent_path() means a bare relative filename, whose directory is the
  // CWD; ask for "." rather than "" so the sync is actually performed instead of
  // silently failing on an unopenable empty path.
  const std::filesystem::path parent = path.parent_path();
  const std::filesystem::path dir = parent.empty() ? std::filesystem::path(".") : parent;
  if (!platform::durable_sync_directory(dir)) {
    // FAIL CLOSED. Handing back a log whose name may not survive a crash would be
    // handing back append()'s durability promise without the durability. Better a
    // refused boot than an order we cannot enumerate afterwards.
    std::fclose(file);
    return fail(chain_error("intentlog: durable_sync_directory failed for the log's directory: " +
                            dir.string()));
  }

  return IntentLog(file, std::move(path), clock);
}

Result<IntentRecord> IntentLog::append(IntentOp op, std::string client_ref,
                                       std::string payload_json) {
  if (file_ == nullptr || clock_ == nullptr) {
    return fail(chain_error("intentlog: append on a moved-from / closed log"));
  }

  IntentRecord record;
  record.schema_version = kSchemaVersion;
  record.seq = next_seq_;
  record.op = op;
  // ── THE IMP-17 CHOKE POINT: PREIMAGE == STORED BYTES, BY CONSTRUCTION ───────
  //
  // The ONLY two caller-supplied byte strings in a record, normalised to valid
  // UTF-8 HERE, ONCE, and BEFORE anything reads them. `record.client_ref` and
  // `record.payload_json` are from this line on BOTH the hash preimage (via
  // canonical_bytes() below) AND the text to_json_line() writes — the same two
  // std::strings, with no transform in between, so they cannot diverge and the
  // serialiser's replace handler is provably a no-op. Every other field is
  // machine-generated ASCII (a hex hash, "GENESIS", a fixed enum name, integers).
  //
  // NEVER REJECTS: an ill-formed byte yields U+FFFD (one per maximal subpart), not
  // a failed append — refusing to record an order intent because a strategy name
  // carried a stray byte would leave a possibly-sent order un-enumerable, which is
  // the exact failure this log exists to prevent. Valid UTF-8 passes through
  // byte-identical, so every record ever written keeps its hash.
  record.client_ref = domain::canonical_text(client_ref);
  record.payload_json = domain::canonical_text(payload_json);

  const auto wall = clock_->now_wall().time_since_epoch();
  record.wall_ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(wall).count();

  record.prev_hash = last_hash_.empty() ? std::string(kGenesis) : last_hash_;
  record.hash = compute_hash(record);

  const std::string line = to_json_line(record);

  if (std::fwrite(line.data(), 1, line.size(), file_) != line.size()) {
    return fail(
        chain_error("intentlog: short write appending record seq " + std::to_string(record.seq)));
  }
  // The single fsync on the hot path: flush the C buffer to the OS, then force
  // the OS write buffers to stable storage. The caller may socket-send only
  // after this returns successfully. The directory entry that names this file was
  // committed once, in open() (IMP-14), so the record's path is durable too.
  if (!flush_and_sync(file_)) {
    return fail(
        chain_error("intentlog: durable_sync failed for record seq " + std::to_string(record.seq)));
  }

  // Durable — commit to in-memory state only now.
  last_hash_ = record.hash;
  next_seq_ = record.seq + 1;
  index_[record.client_ref] = record;
  return record;
}

Result<std::vector<IntentRecord>> IntentLog::replay() {
  // Every replay reports its own tail state; a repair from a previous call must
  // never be read as a fact about this one.
  torn_tail_ = TornTail{};
  std::vector<IntentRecord> records;

  std::FILE* in = std::fopen(path_.string().c_str(), "rb");
  if (in == nullptr) {
    // "ABSENT" AND "PRESENT BUT UNREADABLE" ARE NOT THE SAME BOOT STATE (IMP-14).
    // Only the first one means "nothing was ever written here". Treating an
    // unreadable existing log as an empty one hands rebuild_from_log an empty
    // index while the records naming live orders sit right there on disk — the
    // exact silent-duplicate path. So: absent ⇒ empty boot; anything else, up to
    // and including "the filesystem would not tell us", ⇒ refuse.
    std::error_code ec;
    if (!std::filesystem::exists(path_, ec) && !ec) {
      return records;
    }
    return fail(chain_error("intentlog: log file exists but could not be opened for replay: " +
                            path_.string()));
  }

  // Read the whole file, then split on '\n'. Records are single JSON lines with
  // no embedded newlines (nlohmann's compact dump() emits none, and payloads are
  // embedded as JSON values/strings without raw newlines).
  std::string contents;
  {
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
      contents.append(buf, n);
    }
  }
  std::fclose(in);

  std::int64_t expected_seq = 1;
  std::string prev_hash(kGenesis);

  // Set when the FINAL chunk parsed and verified but its newline never made it to
  // disk. That record is genuine and is kept — but the file must still be repaired
  // before anyone appends to it (see the repair block after the loop).
  bool tail_missing_newline = false;

  std::size_t pos = 0;
  while (pos < contents.size()) {
    const std::size_t line_start = pos;
    std::size_t eol = contents.find('\n', pos);
    // ── THE ONE CHUNK ELIGIBLE FOR TORN-WRITE LENIENCY (IMP-13) ──────────────
    //
    // append() writes each record with its terminating newline in a SINGLE
    // fwrite, so an UNTERMINATED chunk can only be the residue of a write that
    // never completed — a crash or power loss between fwrite/fflush and
    // durable_sync. `find` returning npos also proves nothing follows it, so this
    // is necessarily the last chunk in the file. Both halves matter: a malformed
    // line that IS terminated, or that has data after it, was written whole and
    // then damaged, which is tampering or corruption and stays FATAL below.
    const bool unterminated = (eol == std::string::npos);
    if (unterminated) {
      eol = contents.size();
    }
    std::string_view line(contents.data() + pos, eol - pos);
    pos = eol + 1;
    if (line.empty()) {
      continue;  // tolerate a trailing blank line
    }

    json parsed = json::parse(line, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
      if (unterminated) {
        // ── TORN TAIL: DISCARD THE FRAGMENT, KEEP THE VERIFIED PREFIX ─────────
        //
        // The fragment is provably safe to drop: append() had not returned, so
        // dispatch() never reached run_pre_send_barrier() or the broker send, and
        // no order exists for it. Failing the WHOLE replay over it — what this
        // used to do — throws away records 1..N-1, which DO name live orders, and
        // bricks every subsequent boot identically; the operator's only way out
        // is to move the log aside, and an empty idempotency index then re-places
        // every signal. The prefix is hash-chain-verified at this point, so
        // returning it is not a tolerance, it is the whole verified truth.
        //
        // Truncate to `line_start`, the byte after the last newline, so the file
        // ends on a record boundary again. Without this the NEXT append() would
        // be glued onto the fragment and the log would become unreplayable for
        // good — the recoverable failure turned into the permanent one.
        std::error_code ec;
        std::filesystem::resize_file(path_, static_cast<std::uintmax_t>(line_start), ec);
        if (ec) {
          return fail(chain_error("intentlog: could not truncate a torn trailing record at byte " +
                                  std::to_string(line_start) + ": " + ec.message()));
        }
        // "ab" writes at end-of-file per write, so the append cursor follows the
        // truncation. Sync so the repair itself survives the next power cut.
        if (!flush_and_sync(file_)) {
          return fail(
              chain_error("intentlog: durable_sync failed after truncating a torn "
                          "trailing record at byte " +
                          std::to_string(line_start)));
        }
        torn_tail_.repaired = true;
        torn_tail_.discarded_bytes = static_cast<std::uint64_t>(line.size());
        torn_tail_.truncated_to = static_cast<std::uint64_t>(line_start);
        break;  // nothing can follow an unterminated chunk
      }
      return fail(chain_error("intentlog: malformed JSON at record index " +
                              std::to_string(records.size())));
    }

    IntentRecord record;
    record.schema_version = parsed.value("schema_version", 0);
    record.seq = parsed.value("seq", std::int64_t{0});
    record.client_ref = parsed.value("client_ref", std::string{});
    record.payload_json = parsed.value("payload", std::string{});
    record.wall_ts_ns = parsed.value("wall_ts_ns", std::int64_t{0});
    record.prev_hash = parsed.value("prev_hash", std::string{});
    record.hash = parsed.value("hash", std::string{});

    const std::string op_name = parsed.value("op", std::string{});
    const std::optional<IntentOp> op = intent_op_from_string(op_name);
    if (!op.has_value()) {
      return fail(chain_error("intentlog: unknown op '" + op_name + "' at seq " +
                              std::to_string(record.seq)));
    }
    record.op = *op;

    // Chain check 1: monotonic seq starting at 1.
    if (record.seq != expected_seq) {
      return fail(chain_error("intentlog: broken chain at seq " + std::to_string(record.seq) +
                              " (expected seq " + std::to_string(expected_seq) + ")"));
    }
    // Chain check 2: this record's prev_hash must equal the running prev_hash.
    if (record.prev_hash != prev_hash) {
      return fail(chain_error("intentlog: broken chain at seq " + std::to_string(record.seq) +
                              " (prev_hash mismatch)"));
    }
    // Chain check 3: recompute the hash over the canonical bytes; a tampered
    // field (or a forged hash) is caught here.
    const std::string recomputed = compute_hash(record);
    if (recomputed != record.hash) {
      return fail(chain_error("intentlog: broken chain at seq " + std::to_string(record.seq) +
                              " (hash mismatch — record tampered)"));
    }

    // A complete, chain-verified record whose trailing newline was lost. A torn
    // write cannot produce a syntactically complete object AND a matching hash, so
    // this is a real record that simply lost its last byte — keep it (dropping a
    // verified intent would make a possibly-sent order un-enumerable) and repair
    // the separator after the loop. Note this branch is unreachable for a record
    // that FAILED verification: those still fall through to the fatal returns
    // above, terminated or not, because only a PARSE failure is torn-write
    // evidence.
    if (unterminated) {
      tail_missing_newline = true;
    }

    prev_hash = record.hash;
    ++expected_seq;
    index_[record.client_ref] = record;
    records.push_back(std::move(record));
  }

  if (tail_missing_newline) {
    // Restore the record separator the crash swallowed. Nothing is discarded here
    // — `records` already carries the last record — but without the newline the
    // next append() would concatenate onto it and produce one unparseable line,
    // permanently breaking replay for a log that is otherwise fully intact.
    if (file_ == nullptr || std::fputc('\n', file_) == EOF || !flush_and_sync(file_)) {
      return fail(
          chain_error("intentlog: could not restore the missing record separator on the "
                      "final line of " +
                      path_.string()));
    }
    torn_tail_.repaired = true;
    torn_tail_.truncated_to = static_cast<std::uint64_t>(contents.size() + 1);
  }
  torn_tail_.records_kept = static_cast<std::int64_t>(records.size());

  // Rebuild the append cursor from the verified tail.
  if (!records.empty()) {
    last_hash_ = records.back().hash;
    next_seq_ = records.back().seq + 1;
  } else {
    last_hash_.clear();
    next_seq_ = 1;
  }
  return records;
}

std::optional<IntentRecord> IntentLog::last_for(std::string_view client_ref) const {
  // Look up by the CANONICAL ref, because that is the only key the index can
  // contain: append() canonicalises before inserting, and replay() reads back the
  // canonical bytes from disk. Canonicalising the QUERY too is what keeps the
  // idempotency lookup total — a caller that appended with an ill-formed ref and
  // then asks with the same (still ill-formed) bytes must find its own record, or
  // restart-dedup would silently miss and the order could be placed twice. Free
  // for every real ref: canonical_text is the identity on valid UTF-8, and is
  // idempotent, so an already-canonical query hashes to the same key.
  const auto it = index_.find(domain::canonical_text(client_ref));
  if (it == index_.end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace broker_exec::intentlog
