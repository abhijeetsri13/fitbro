#include "broker_exec/intentlog/intent_log.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

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
  std::string line = j.dump();
  line.push_back('\n');
  return line;
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
  if (name == "place_order") return IntentOp::PlaceOrder;
  if (name == "modify_order") return IntentOp::ModifyOrder;
  if (name == "cancel_order") return IntentOp::CancelOrder;
  if (name == "square_off") return IntentOp::SquareOff;
  if (name == "result") return IntentOp::Result;
  if (name == "child_slice") return IntentOp::ChildSlice;
  return std::nullopt;
}

IntentLog::IntentLog(std::FILE* file, std::filesystem::path path,
                     ports::ClockPort& clock) noexcept
    : file_(file), path_(std::move(path)), clock_(&clock) {}

IntentLog::IntentLog(IntentLog&& other) noexcept
    : file_(other.file_),
      path_(std::move(other.path_)),
      clock_(other.clock_),
      next_seq_(other.next_seq_),
      last_hash_(std::move(other.last_hash_)),
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
  record.client_ref = std::move(client_ref);
  record.op = op;
  record.payload_json = std::move(payload_json);

  const auto wall = clock_->now_wall().time_since_epoch();
  record.wall_ts_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(wall).count();

  record.prev_hash = last_hash_.empty() ? std::string(kGenesis) : last_hash_;
  record.hash = compute_hash(record);

  const std::string line = to_json_line(record);

  if (std::fwrite(line.data(), 1, line.size(), file_) != line.size()) {
    return fail(chain_error("intentlog: short write appending record seq " +
                            std::to_string(record.seq)));
  }
  // The single fsync on the hot path: flush the C buffer to the OS, then force
  // the OS write buffers to stable storage. The caller may socket-send only
  // after this returns successfully.
  if (std::fflush(file_) != 0) {
    return fail(chain_error("intentlog: fflush failed for record seq " +
                            std::to_string(record.seq)));
  }
  const int fd = platform::portable_fileno(file_);
  if (fd < 0 || !platform::durable_sync(fd)) {
    return fail(chain_error("intentlog: durable_sync failed for record seq " +
                            std::to_string(record.seq)));
  }

  // Durable — commit to in-memory state only now.
  last_hash_ = record.hash;
  next_seq_ = record.seq + 1;
  index_[record.client_ref] = record;
  return record;
}

Result<std::vector<IntentRecord>> IntentLog::replay() {
  std::vector<IntentRecord> records;

  std::FILE* in = std::fopen(path_.string().c_str(), "rb");
  if (in == nullptr) {
    // No file yet ⇒ nothing to replay; an empty index is the correct boot state.
    return records;
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

  std::size_t pos = 0;
  while (pos < contents.size()) {
    std::size_t eol = contents.find('\n', pos);
    if (eol == std::string::npos) {
      eol = contents.size();
    }
    std::string_view line(contents.data() + pos, eol - pos);
    pos = eol + 1;
    if (line.empty()) {
      continue;  // tolerate a trailing blank line
    }

    json parsed = json::parse(line, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
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

    prev_hash = record.hash;
    ++expected_seq;
    index_[record.client_ref] = record;
    records.push_back(std::move(record));
  }

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
  const auto it = index_.find(std::string(client_ref));
  if (it == index_.end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace broker_exec::intentlog
