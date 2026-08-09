#include "broker_exec/ledger/ledger.hpp"

#include <openssl/evp.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "broker_exec/domain/redaction.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/platform/durable.hpp"

namespace broker_exec::ledger {

namespace fs = std::filesystem;

using ::broker_exec::errors::Error;
using ::broker_exec::errors::ErrorCategory;
using ::broker_exec::errors::make_error;

namespace {

using json = nlohmann::json;

constexpr std::size_t kEd25519KeyBytes = 32;

// ── OpenSSL RAII owners ──────────────────────────────────────────────────────
// Each EVP context/PKEY is freed on every path (success or early return), so no
// OpenSSL object can leak across our no-throw boundary (mirrors token_store.cpp).

class MdCtx {
 public:
  MdCtx() noexcept : ctx_(EVP_MD_CTX_new()) {}
  ~MdCtx() {
    if (ctx_ != nullptr) {
      EVP_MD_CTX_free(ctx_);
    }
  }
  MdCtx(const MdCtx&) = delete;
  MdCtx& operator=(const MdCtx&) = delete;
  MdCtx(MdCtx&&) = delete;
  MdCtx& operator=(MdCtx&&) = delete;

  [[nodiscard]] EVP_MD_CTX* get() const noexcept { return ctx_; }
  [[nodiscard]] explicit operator bool() const noexcept { return ctx_ != nullptr; }

 private:
  EVP_MD_CTX* ctx_;
};

class PkeyCtx {
 public:
  explicit PkeyCtx(int id) noexcept : ctx_(EVP_PKEY_CTX_new_id(id, nullptr)) {}
  ~PkeyCtx() {
    if (ctx_ != nullptr) {
      EVP_PKEY_CTX_free(ctx_);
    }
  }
  PkeyCtx(const PkeyCtx&) = delete;
  PkeyCtx& operator=(const PkeyCtx&) = delete;
  PkeyCtx(PkeyCtx&&) = delete;
  PkeyCtx& operator=(PkeyCtx&&) = delete;

  [[nodiscard]] EVP_PKEY_CTX* get() const noexcept { return ctx_; }
  [[nodiscard]] explicit operator bool() const noexcept { return ctx_ != nullptr; }

 private:
  EVP_PKEY_CTX* ctx_;
};

class Pkey {
 public:
  Pkey() noexcept = default;
  explicit Pkey(EVP_PKEY* pkey) noexcept : pkey_(pkey) {}
  ~Pkey() {
    if (pkey_ != nullptr) {
      EVP_PKEY_free(pkey_);
    }
  }
  Pkey(const Pkey&) = delete;
  Pkey& operator=(const Pkey&) = delete;
  Pkey(Pkey&&) = delete;
  Pkey& operator=(Pkey&&) = delete;

  [[nodiscard]] EVP_PKEY* get() const noexcept { return pkey_; }
  [[nodiscard]] EVP_PKEY** addr() noexcept { return &pkey_; }
  [[nodiscard]] explicit operator bool() const noexcept { return pkey_ != nullptr; }

 private:
  EVP_PKEY* pkey_ = nullptr;
};

// ── small helpers ────────────────────────────────────────────────────────────

[[nodiscard]] const unsigned char* as_u8(const char* p) noexcept {
  return reinterpret_cast<const unsigned char*>(p);
}

[[nodiscard]] Error crypto_error(std::string message) {
  return make_error(ErrorCategory::Internal, std::move(message));
}

// Lowercase hex of a byte span.
[[nodiscard]] std::string to_hex(const unsigned char* data, std::size_t len) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (std::size_t i = 0; i < len; ++i) {
    out.push_back(kDigits[data[i] >> 4]);
    out.push_back(kDigits[data[i] & 0x0F]);
  }
  return out;
}

[[nodiscard]] std::string to_hex(const std::vector<unsigned char>& bytes) {
  return to_hex(bytes.data(), bytes.size());
}

// Decode a lowercase/uppercase hex string into bytes. Returns false (and leaves
// `out` unspecified) on an odd length or any non-hex digit — used to fail CLOSED
// when a checkpoint's signature/public-key field is garbage rather than hex.
[[nodiscard]] bool from_hex(std::string_view hex, std::vector<unsigned char>& out) {
  if (hex.size() % 2 != 0) {
    return false;
  }
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  out.clear();
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int hi = nibble(hex[i]);
    const int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    out.push_back(static_cast<unsigned char>((hi << 4) | lo));
  }
  return true;
}

// SHA-256 of `data`, lowercase hex. Internal; OpenSSL EVP with RAII on the ctx.
[[nodiscard]] std::string sha256_hex(std::string_view data) {
  MdCtx ctx;
  if (!ctx) {
    return std::string{};
  }
  if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
    return std::string{};
  }
  if (!data.empty() &&
      EVP_DigestUpdate(ctx.get(), as_u8(data.data()), data.size()) != 1) {
    return std::string{};
  }
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_len = 0;
  if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &digest_len) != 1) {
    return std::string{};
  }
  return to_hex(digest.data(), digest_len);
}

// Compact, non-throwing JSON render (arbitrary bytes -> U+FFFD, never throws
// json::type_error across the boundary; only std::bad_alloc could escape).
[[nodiscard]] std::string dump_compact(const json& value) {
  return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

// Format a wall-clock instant as ISO-8601 UTC ("YYYY-MM-DDTHH:MM:SSZ") purely
// from std::chrono — NO localtime/strftime, NO `#ifdef`, identical everywhere.
[[nodiscard]] std::string pad(long long value, std::size_t width) {
  std::string digits = std::to_string(value);
  while (digits.size() < width) {
    digits.insert(digits.begin(), '0');
  }
  return digits;
}

[[nodiscard]] std::string to_iso8601_utc(std::chrono::system_clock::time_point tp) {
  using namespace std::chrono;
  const sys_days day = floor<days>(tp);
  const year_month_day ymd{day};
  const hh_mm_ss<seconds> tod{floor<seconds>(tp - day)};

  const long long year = static_cast<long long>(static_cast<int>(ymd.year()));
  const long long month = static_cast<long long>(static_cast<unsigned>(ymd.month()));
  const long long dom = static_cast<long long>(static_cast<unsigned>(ymd.day()));

  return pad(year, 4) + '-' + pad(month, 2) + '-' + pad(dom, 2) + 'T' +
         pad(static_cast<long long>(tod.hours().count()), 2) + ':' +
         pad(static_cast<long long>(tod.minutes().count()), 2) + ':' +
         pad(static_cast<long long>(tod.seconds().count()), 2) + 'Z';
}

// Render a ProvenanceContext as the shared ` [k=v k=v]` block (IMP-16). Delegates
// to THE single definition in domain so the ledger and the alert sink can never
// drift in how a typed column is redacted. Returns "" when every column is empty,
// which is what makes an empty context byte- (and therefore hash-) identical to a
// plain append.
[[nodiscard]] std::string provenance_block(const ProvenanceContext& provenance) {
  return domain::render_provenance_block({
      {"client_ref", provenance.client_ref},
      {"broker_order_id", provenance.broker_order_id},
      {"strategy", provenance.strategy},
  });
}

// Build the one-line JSON record persisted/parsed for a chain entry.
[[nodiscard]] std::string entry_to_line(const LedgerEntry& entry) {
  json out = json::object();
  out["seq"] = entry.seq;
  out["prev_hash"] = entry.prev_hash;
  out["payload"] = entry.payload;
  out["hash"] = entry.hash;
  return dump_compact(out);
}

}  // namespace

Ledger::Ledger(const ports::ClockPort& clock, std::filesystem::path path) noexcept
    : clock_(&clock), path_(std::move(path)) {}

Result<LedgerEntry> Ledger::append(std::string payload) {
  // An EMPTY context renders "" (see provenance_block), so this is byte-identical
  // to the pre-IMP-16 behaviour: same stored payload, same hash preimage, same
  // hash. Existing chains are unaffected by the new overload's existence.
  return append(std::move(payload), ProvenanceContext{});
}

Result<LedgerEntry> Ledger::append(std::string payload, const ProvenanceContext& provenance) {
  // SCRUB FIRST (SEC-3): the persisted payload and the hash preimage must carry
  // no token-shaped secret. Everything downstream sees only the scrubbed text.
  // The scrub call and its argument are UNCHANGED from before IMP-16 — free-form
  // redaction is not relaxed by one byte.
  //
  // The typed provenance is rendered SEPARATELY (whole-column allowlist) and
  // appended AFTER the scrub. `safe` is then both the persisted payload and the
  // hash preimage, exactly as it always was, so the chain stays self-consistent:
  // verify_chain() recomputes sha256(prev_hash + STORED payload) and sees the same
  // bytes we hashed here.
  const std::string safe = domain::scrub(payload) + provenance_block(provenance);

  // ── KNOWN LIMITATION (pre-existing, NOT introduced by IMP-16) — NEXT STORY ────
  //
  // THE PREIMAGE AND THE STORED BYTES CAN DIVERGE ON INVALID UTF-8. We hash the
  // RAW bytes of `safe`, but entry_to_line() serialises the same string through
  // nlohmann with json::error_handler_t::replace, which rewrites every ill-formed
  // UTF-8 sequence as U+FFFD before it reaches the file. So for a payload carrying
  // an invalid byte (a lone 0x80-0xFF, a truncated multi-byte run) the LINE ON DISK
  // is not the text we hashed. load() then reads back the replaced form and
  // verify_chain() recomputes sha256(prev_hash + STORED payload), which no longer
  // matches `hash` — the chain reports itself BROKEN at that entry, and a broken
  // audit chain is a safe-start blocker. A self-inflicted tamper signal, not a
  // missed one, but it is still a denial-of-audit an untrusted byte can trigger.
  //
  // THE FIX BELONGS IN ITS OWN STORY: `payload` must be UTF-8-VALIDATED (rejected,
  // or replaced ONCE, up front) BEFORE it becomes both the stored text and the
  // preimage, so the two are the same bytes by construction. Doing it here would
  // change the hash of any chain that already contains such an entry, which is a
  // migration, not a patch.
  //
  // RESIDUAL VECTOR, NAMED PRECISELY: broker JSON is not the exposure — nlohmann
  // has already validated anything parsed from a broker response. What remains is
  // CALLER-SUPPLIED and STORE-SUPPLIED text reaching append() as raw bytes.
  // IMP-16's typed provenance columns are NOT a way in: the block guard in
  // domain::render_provenance_block is an allowlist over [A-Za-z0-9_#-*], so no
  // byte >= 0x80 can enter through a provenance column at all.
  LedgerEntry entry;
  entry.seq = static_cast<std::int64_t>(entries_.size());
  entry.prev_hash = entries_.empty() ? std::string{} : entries_.back().hash;
  entry.payload = safe;
  entry.hash = sha256_hex(entry.prev_hash + safe);
  if (entry.hash.empty()) {
    return fail(crypto_error("ledger: SHA-256 hashing failed"));
  }

  const std::string line = entry_to_line(entry) + '\n';

  // Append one JSON line and fsync through the platform durability seam, so a
  // crash after return cannot lose a record that we reported as written.
  std::FILE* fp = std::fopen(path_.string().c_str(), "ab");
  if (fp == nullptr) {
    return fail(make_error(ErrorCategory::Internal, "ledger: failed to open ledger file for append"));
  }
  const std::size_t written = std::fwrite(line.data(), 1, line.size(), fp);
  if (written != line.size() || std::fflush(fp) != 0) {
    std::fclose(fp);
    return fail(make_error(ErrorCategory::Internal, "ledger: failed to write ledger entry"));
  }
  const bool synced = platform::durable_sync(platform::portable_fileno(fp));
  if (std::fclose(fp) != 0 || !synced) {
    return fail(make_error(ErrorCategory::Internal, "ledger: failed to durably sync ledger entry"));
  }

  entries_.push_back(entry);
  return entry;
}

Result<ports::Ok> Ledger::verify_chain() const {
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    const LedgerEntry& entry = entries_[i];
    const std::string expected_hash = sha256_hex(entry.prev_hash + entry.payload);
    const bool hash_ok = !expected_hash.empty() && entry.hash == expected_hash;
    const bool link_ok =
        (i == 0) ? entry.prev_hash.empty() : entry.prev_hash == entries_[i - 1].hash;
    const bool seq_ok = entry.seq == static_cast<std::int64_t>(i);
    if (!hash_ok || !link_ok || !seq_ok) {
      return fail(make_error(ErrorCategory::Validation,
                             "ledger chain broken at seq " + std::to_string(i)));
    }
  }
  return ports::ok();
}

Result<ports::Ok> Ledger::load() {
  entries_.clear();

  std::ifstream in(path_, std::ios::binary);
  if (!in) {
    return fail(make_error(ErrorCategory::Validation, "ledger: ledger file not found"));
  }

  // Read every line first so we can identify the final content line: a failed
  // trailing append (a partial fwrite before fsync/fclose) can leave ONE torn,
  // un-synced last line, which we tolerate (skip) rather than reject the whole
  // ledger. A malformed line that is NOT the last content line stays FATAL —
  // that is a real mid-file tamper.
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(std::move(line));
  }
  if (in.bad()) {
    return fail(make_error(ErrorCategory::Internal, "ledger: failed to read ledger file"));
  }

  // Index of the last non-blank line — the only line eligible for torn-write
  // leniency. Left at lines.size() (never a valid index) when there is none.
  std::size_t last_content = lines.size();
  for (std::size_t i = lines.size(); i-- > 0;) {
    if (!lines[i].empty()) {
      last_content = i;
      break;
    }
  }

  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].empty()) {
      continue;  // tolerate a trailing/blank line
    }
    // Non-throwing parse: a corrupt/garbled line is a defined Error, not a throw.
    const json parsed = json::parse(lines[i], nullptr, false);
    // TYPE-CHECK every field before extraction. nlohmann value()/get<> throws
    // type_error.302 on a wrong-type field (e.g. a payload that is a NUMBER),
    // which would escape our no-throw Result boundary. Require each field to be
    // present AND of the expected type; never let a json access throw.
    const bool well_formed =
        !parsed.is_discarded() && parsed.is_object() &&
        parsed.contains("seq") && parsed["seq"].is_number_integer() &&
        parsed.contains("prev_hash") && parsed["prev_hash"].is_string() &&
        parsed.contains("payload") && parsed["payload"].is_string() &&
        parsed.contains("hash") && parsed["hash"].is_string();
    if (!well_formed) {
      if (i == last_content) {
        // Torn trailing write: skip the single un-synced last record rather than
        // refusing the whole ledger (the caller verify_chain()s after load).
        continue;
      }
      return fail(make_error(ErrorCategory::Validation,
                             "ledger: malformed entry on line " + std::to_string(i + 1)));
    }
    LedgerEntry entry;
    entry.seq = parsed["seq"].get<std::int64_t>();
    entry.prev_hash = parsed["prev_hash"].get<std::string>();
    entry.payload = parsed["payload"].get<std::string>();
    entry.hash = parsed["hash"].get<std::string>();
    entries_.push_back(std::move(entry));
  }
  return ports::ok();
}

std::string Ledger::head_hash() const {
  return entries_.empty() ? std::string{} : entries_.back().hash;
}

std::size_t Ledger::size() const noexcept { return entries_.size(); }

Result<Ed25519KeyPair> Ledger::generate_keypair() {
  PkeyCtx ctx(EVP_PKEY_ED25519);
  if (!ctx) {
    return fail(crypto_error("ledger: Ed25519 key context allocation failed"));
  }
  if (EVP_PKEY_keygen_init(ctx.get()) != 1) {
    return fail(crypto_error("ledger: Ed25519 keygen init failed"));
  }
  Pkey pkey;
  if (EVP_PKEY_keygen(ctx.get(), pkey.addr()) != 1 || !pkey) {
    return fail(crypto_error("ledger: Ed25519 key generation failed"));
  }

  Ed25519KeyPair kp;
  kp.public_key.resize(kEd25519KeyBytes);
  kp.private_key.resize(kEd25519KeyBytes);
  std::size_t pub_len = kEd25519KeyBytes;
  std::size_t prv_len = kEd25519KeyBytes;
  if (EVP_PKEY_get_raw_public_key(pkey.get(), kp.public_key.data(), &pub_len) != 1 ||
      EVP_PKEY_get_raw_private_key(pkey.get(), kp.private_key.data(), &prv_len) != 1 ||
      pub_len != kEd25519KeyBytes || prv_len != kEd25519KeyBytes) {
    return fail(crypto_error("ledger: Ed25519 raw key export failed"));
  }
  return kp;
}

Result<std::vector<unsigned char>> Ledger::sign_head(
    const std::vector<unsigned char>& private_key) const {
  const std::string head = head_hash();
  if (head.empty()) {
    return fail(make_error(ErrorCategory::Validation, "ledger empty, nothing to sign"));
  }
  if (private_key.size() != kEd25519KeyBytes) {
    return fail(make_error(ErrorCategory::Validation, "ledger: Ed25519 private key must be 32 bytes"));
  }

  Pkey pkey(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, private_key.data(),
                                         private_key.size()));
  if (!pkey) {
    return fail(crypto_error("ledger: Ed25519 private key import failed"));
  }
  MdCtx ctx;
  if (!ctx) {
    return fail(crypto_error("ledger: sign context allocation failed"));
  }
  // Ed25519 is a pure (one-shot) signature: a null md, EVP_DigestSign over the
  // whole message at once.
  if (EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, pkey.get()) != 1) {
    return fail(crypto_error("ledger: sign init failed"));
  }
  std::size_t sig_len = 0;
  if (EVP_DigestSign(ctx.get(), nullptr, &sig_len, as_u8(head.data()), head.size()) != 1) {
    return fail(crypto_error("ledger: signature length probe failed"));
  }
  std::vector<unsigned char> signature(sig_len);
  if (EVP_DigestSign(ctx.get(), signature.data(), &sig_len, as_u8(head.data()), head.size()) != 1) {
    return fail(crypto_error("ledger: signing failed"));
  }
  signature.resize(sig_len);
  return signature;
}

Result<ports::Ok> Ledger::verify_head(std::string_view head_hash,
                                      const std::vector<unsigned char>& signature,
                                      const std::vector<unsigned char>& public_key) {
  if (public_key.size() != kEd25519KeyBytes) {
    return fail(make_error(ErrorCategory::Validation, "ledger: Ed25519 public key must be 32 bytes"));
  }
  Pkey pkey(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, public_key.data(),
                                        public_key.size()));
  if (!pkey) {
    return fail(crypto_error("ledger: Ed25519 public key import failed"));
  }
  MdCtx ctx;
  if (!ctx) {
    return fail(crypto_error("ledger: verify context allocation failed"));
  }
  if (EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, pkey.get()) != 1) {
    return fail(crypto_error("ledger: verify init failed"));
  }
  // FAIL CLOSED: only an exact 1 from OpenSSL is acceptance; a wrong key or a
  // tampered head yields 0 (or <0 on error), both -> Error.
  const int rc = EVP_DigestVerify(ctx.get(), signature.data(), signature.size(),
                                  as_u8(head_hash.data()), head_hash.size());
  if (rc != 1) {
    return fail(make_error(ErrorCategory::Validation,
                           "ledger: signature verification failed (wrong key or tampered head)"));
  }
  return ports::ok();
}

Result<ports::Ok> Ledger::require_key_match(const std::vector<unsigned char>& expected,
                                            const std::vector<unsigned char>& actual) {
  // Public keys, not secrets — a plain compare is fine. Fail CLOSED on any
  // difference: a changed signing key is a safe-start blocker (AC-3).
  if (expected != actual) {
    return fail(make_error(ErrorCategory::Auth, "ledger signing key mismatch"));
  }
  return ports::ok();
}

Result<ports::Ok> Ledger::write_public_key(const std::vector<unsigned char>& public_key,
                                           const std::filesystem::path& dir) const {
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    return fail(make_error(ErrorCategory::Internal, "ledger: failed to create public-key directory"));
  }
  const fs::path file = dir / "ledger_public_key.hex";
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) {
    return fail(make_error(ErrorCategory::Internal, "ledger: failed to open public-key file"));
  }
  const std::string hex = to_hex(public_key);
  out.write(hex.data(), static_cast<std::streamsize>(hex.size()));
  out.flush();
  if (!out) {
    return fail(make_error(ErrorCategory::Internal, "ledger: failed to write public-key file"));
  }
  return ports::ok();
}

Result<EodReport> Ledger::eod_report(const std::vector<unsigned char>& private_key,
                                     const std::vector<unsigned char>& public_key) const {
  auto signature = sign_head(private_key);
  if (!signature) {
    return fail(std::move(signature).error());
  }
  EodReport report;
  report.entry_count = static_cast<std::int64_t>(size());
  report.head_hash = head_hash();
  report.signature = std::move(signature).value();
  report.public_key = public_key;
  return report;
}

fs::path Ledger::checkpoint_path() const {
  // Sibling of the ledger file: <ledger path> + ".checkpoint" (a filename-suffix
  // concat, so e.g. ".../ledger.jsonl" -> ".../ledger.jsonl.checkpoint").
  fs::path cp = path_;
  cp += ".checkpoint";
  return cp;
}

Result<ports::Ok> Ledger::write_checkpoint(const std::vector<unsigned char>& private_key,
                                           const std::vector<unsigned char>& public_key) const {
  // An empty ledger has no head to sign -> a defined Error in the checkpoint
  // vocabulary (guarded before eod_report so the message is checkpoint-specific).
  if (head_hash().empty()) {
    return fail(make_error(ErrorCategory::Validation, "ledger empty, nothing to checkpoint"));
  }

  // The checkpoint payload IS the EOD bundle {entry_count, head_hash, Ed25519
  // signature over head_hash, public_key}: reuse eod_report() so the signed
  // high-water mark and the EOD report are byte-for-byte the same construction.
  auto report = eod_report(private_key, public_key);
  if (!report) {
    return fail(std::move(report).error());
  }
  const std::string bundle = report.value().to_json();

  // ATOMIC + DURABLE publish (same discipline as append()): write the full
  // payload to a temp sibling, fsync it, then rename it over the checkpoint path.
  // A crash mid-write leaves the OLD checkpoint intact — a reader never observes
  // a half-written high-water mark.
  const fs::path target = checkpoint_path();
  fs::path tmp = target;
  tmp += ".tmp";

  std::FILE* fp = std::fopen(tmp.string().c_str(), "wb");
  if (fp == nullptr) {
    return fail(make_error(ErrorCategory::Internal, "ledger: failed to open checkpoint temp file"));
  }
  const std::size_t written = std::fwrite(bundle.data(), 1, bundle.size(), fp);
  if (written != bundle.size() || std::fflush(fp) != 0) {
    std::fclose(fp);
    return fail(make_error(ErrorCategory::Internal, "ledger: failed to write checkpoint"));
  }
  const bool synced = platform::durable_sync(platform::portable_fileno(fp));
  if (std::fclose(fp) != 0 || !synced) {
    return fail(make_error(ErrorCategory::Internal, "ledger: failed to durably sync checkpoint"));
  }
  std::error_code ec;
  fs::rename(tmp, target, ec);
  if (ec) {
    return fail(
        make_error(ErrorCategory::Internal, "ledger: failed to atomically publish checkpoint"));
  }
  return ports::ok();
}

Result<ports::Ok> Ledger::verify_against_checkpoint(
    const std::vector<unsigned char>& pinned_public_key) const {
  const fs::path cp = checkpoint_path();
  std::error_code ec;
  const bool present = fs::exists(cp, ec);
  if (ec) {
    return fail(make_error(ErrorCategory::Internal, "ledger: failed to stat checkpoint file"));
  }
  if (!present) {
    // NO BASELINE: no checkpoint has ever been written, so there is genuinely no
    // prior signed state to measure a truncation against. This is the only safe
    // default and is NOT fail-open — truncation simply cannot be detected before
    // the first checkpoint arms it.
    return ports::ok();
  }

  // The checkpoint EXISTS, so from here every failure is FAIL CLOSED: a present
  // checkpoint that won't parse / won't verify is itself evidence of tamper.
  std::ifstream in(cp, std::ios::binary);
  if (!in) {
    return fail(make_error(ErrorCategory::Internal, "ledger: failed to open checkpoint file"));
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) {
    return fail(make_error(ErrorCategory::Internal, "ledger: failed to read checkpoint file"));
  }

  // Non-throwing, TYPE-CHECKED parse (same discipline as load()): require each
  // field present AND of the expected type so no json access can throw across the
  // no-throw boundary. A malformed checkpoint -> Validation Error, never ok().
  const json parsed = json::parse(text, nullptr, false);
  const bool well_formed =
      !parsed.is_discarded() && parsed.is_object() &&
      parsed.contains("entry_count") && parsed["entry_count"].is_number_integer() &&
      parsed.contains("head_hash") && parsed["head_hash"].is_string() &&
      parsed.contains("signature") && parsed["signature"].is_string() &&
      parsed.contains("public_key") && parsed["public_key"].is_string();
  if (!well_formed) {
    return fail(
        make_error(ErrorCategory::Validation, "ledger: checkpoint file malformed or unparseable"));
  }

  const std::int64_t cp_size = parsed["entry_count"].get<std::int64_t>();
  const std::string cp_head = parsed["head_hash"].get<std::string>();
  std::vector<unsigned char> signature;
  std::vector<unsigned char> public_key;
  if (!from_hex(parsed["signature"].get<std::string>(), signature) ||
      !from_hex(parsed["public_key"].get<std::string>(), public_key)) {
    return fail(
        make_error(ErrorCategory::Validation, "ledger: checkpoint file malformed or unparseable"));
  }

  // A valid checkpoint always signs at least the genesis entry; size<1 is
  // nonsensical (and would underflow the index guard below).
  if (cp_size < 1) {
    return fail(
        make_error(ErrorCategory::Validation, "ledger: checkpoint records a non-positive size"));
  }

  // KEY PINNING (anti-substitution — the core anti-tamper anchor): the public_key
  // embedded in the checkpoint is attacker-writable, so verifying the signature
  // under it would be circular — a non-key-holder could generate a fresh keypair,
  // re-sign a truncated/rolled-back chain, and embed their own public key, passing
  // every check. The embedded key MUST therefore match a PINNED key the caller
  // trusts out-of-band (the EOD-published key / ledger_public_key.hex provisioned
  // under restricted perms). Fail closed on mismatch, BEFORE trusting anything.
  if (auto pinned_ok = require_key_match(pinned_public_key, public_key); !pinned_ok) {
    return fail(make_error(ErrorCategory::Validation,
                           "checkpoint public key does not match the pinned key (substitution)"));
  }

  // AUTHENTICITY: the signed head must verify under the PINNED key (== embedded,
  // just matched). A forged or edited checkpoint (signature or head flipped) fails.
  if (auto authentic = verify_head(cp_head, signature, pinned_public_key); !authentic) {
    return fail(make_error(ErrorCategory::Validation, "checkpoint signature invalid"));
  }

  // TRUNCATION: the live chain must still be at least as long as the signed
  // high-water mark; a shorter chain means the tail was chopped.
  if (static_cast<std::int64_t>(size()) < cp_size) {
    return fail(make_error(ErrorCategory::Validation,
                           "ledger TRUNCATED: have " + std::to_string(size()) +
                               " entries, checkpoint signed " + std::to_string(cp_size)));
  }

  // ROLLBACK / SUBSTITUTION: the historical entry at the high-water index must
  // still carry the signed head hash. (size>=cp_size and cp_size>=1 keep the
  // index in range.) Growth is fine — appended entries do not change this one.
  const LedgerEntry& at_mark = entries_[static_cast<std::size_t>(cp_size) - 1];
  if (at_mark.hash != cp_head) {
    return fail(make_error(ErrorCategory::Validation,
                           "ledger ROLLED BACK/SUBSTITUTED: entry " + std::to_string(cp_size - 1) +
                               " hash diverges from the signed checkpoint"));
  }
  return ports::ok();
}

PositionHeartbeat Ledger::make_heartbeat(std::string_view exposure_summary,
                                         std::chrono::system_clock::time_point ts) {
  // Empty context -> empty block -> `exposure` (and to_json) byte-identical to the
  // pre-IMP-16 output.
  return make_heartbeat(exposure_summary, ts, ProvenanceContext{});
}

PositionHeartbeat Ledger::make_heartbeat(std::string_view exposure_summary,
                                         std::chrono::system_clock::time_point ts,
                                         const ProvenanceContext& provenance) {
  PositionHeartbeat hb;
  hb.ts = to_iso8601_utc(ts);
  // Same scrub as before (no secret reaches the operator), plus the typed columns
  // rendered through the whole-column allowlist and appended after it.
  hb.exposure = domain::scrub(exposure_summary) + provenance_block(provenance);
  return hb;
}

std::string EodReport::to_json() const {
  json out = json::object();
  out["entry_count"] = entry_count;
  out["head_hash"] = head_hash;
  out["signature"] = to_hex(signature);
  out["public_key"] = to_hex(public_key);
  return dump_compact(out);
}

std::string PositionHeartbeat::to_json() const {
  json out = json::object();
  out["ts"] = ts;
  out["exposure"] = exposure;
  return dump_compact(out);
}

}  // namespace broker_exec::ledger
