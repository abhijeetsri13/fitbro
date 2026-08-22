#include "broker_exec/ledger/ledger.hpp"

#include <openssl/evp.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "broker_exec/domain/redaction.hpp"
#include "broker_exec/domain/utf8.hpp"
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
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
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
  if (!data.empty() && EVP_DigestUpdate(ctx.get(), as_u8(data.data()), data.size()) != 1) {
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

// ── IMP-17: THE ONE UTF-8 CANONICALISATION CHOKE POINT ───────────────────────
//
// EVERY string that becomes a ledger payload — or any other text field we render
// through dump_compact — goes through domain::canonical_text() EXACTLY ONCE, UP
// FRONT, and the value it returns IS, BY DEFINITION, both the hash preimage and
// the bytes that reach the file. There is no second transform between the two, so
// they cannot diverge. If you add a write path, it MUST call it; if you are
// tempted to hash one string and store another, that is the IMP-17 defect.
//
// WHY IT IS NEEDED. dump_compact() serialises with error_handler_t::replace,
// which rewrites every ill-formed UTF-8 sequence as U+FFFD on the way out. Any
// text NOT canonicalised first is therefore stored in a form that differs from
// the bytes we hashed, and verify_chain() — which recomputes over the STORED
// payload — would report the chain BROKEN. Canonicalising first makes the
// replacement a NO-OP at dump time, which is the entire fix.
//
// THE IMPLEMENTATION IS NOT HERE, DELIBERATELY. It was promoted to
// broker_exec::domain (include/broker_exec/domain/utf8.hpp, src/domain/utf8.cpp)
// so THE INTENT LOG SHARES IT: intentlog::IntentLog::append has the identical
// preimage/stored-bytes duality over its own SHA-256 chain, and two copies of a
// UTF-8 decoder would eventually disagree about what a "maximal subpart" is —
// at which point the two logs would canonicalise the same caller text into
// different bytes. ONE definition, two consumers. Read domain/utf8.hpp for the
// full contract (P1..P5) and for the scrub-ordering theorem cited in append().
// Every use below is spelled `domain::canonical_text` / `domain::kUtf8Replacement`
// in full, so `grep -rn canonical_text` shows at a glance that this file DEFINES
// nothing and only CALLS the shared one.

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
  // appended AFTER the scrub.
  //
  // ── THE IMP-17 INVARIANT: PREIMAGE == STORED BYTES, BY CONSTRUCTION ─────────
  //
  // canonical_text() runs LAST, at this single choke point, and its result is the
  // ONLY text that goes any further. `entry.payload` is then BOTH the persisted
  // payload AND the hash preimage — the hash below is taken from `entry.payload`
  // itself, not from a parallel variable — so the two are the same std::string and
  // cannot drift. Because the canonical form is valid UTF-8, entry_to_line()'s
  // dump-with-replace has nothing to replace and writes those exact bytes, load()
  // parses those exact bytes back, and verify_chain()'s
  // sha256(prev_hash + STORED payload) reproduces the hash we computed here. The
  // pre-IMP-17 divergence (hash the raw bytes, store the replaced form, then
  // report your own ledger BROKEN — a denial-of-audit any untrusted byte could
  // trigger) is closed at the source rather than tolerated downstream.
  //
  // WHY NORMALISE **AFTER** THE SCRUB, NOT BEFORE. Two reasons, in order:
  //   1. The invariant must hold for the FINAL string. Normalising earlier would
  //      leave scrub() and render_provenance_block() free to reintroduce a bad
  //      byte, so the guarantee would rest on auditing two other modules instead
  //      of on this one line. Last means unbypassable.
  //   2. THE ORDER IS LOAD-BEARING FOR REDACTION ITSELF — and the two orders are
  //      NOT interchangeable. State the theorem precisely, because an earlier
  //      version of this comment claimed the two orders "redact identically" and
  //      THAT CLAIM IS FALSE; believing it would let a future refactor hoist the
  //      canonicalisation and silently change what gets redacted.
  //
  //      TRUE (order-invariant): canonical_text preserves the ASCII subsequence
  //      exactly (domain/utf8.hpp P4) and never shrinks a run to nothing (P5), so
  //      it can neither JOIN two of scrub()'s ASCII [A-Za-z0-9_-] token runs nor
  //      SPLIT one. scrub() therefore tokenises bit-for-bit identically on either
  //      side of it, and every TOKEN-SHAPED rule — `key=value`, and the >=20-char
  //      high-entropy run — fires on exactly the same runs in either order.
  //
  //      FALSE (order-SENSITIVE): canonical_text is NOT LENGTH-PRESERVING. One
  //      ill-formed byte becomes THREE (U+FFFD), and domain::auth_context_before
  //      looks back a FIXED 10-BYTE window for an auth keyword — so normalising
  //      first MOVES the keyword relative to that byte window and flips the bare
  //      MPIN/TOTP digit-run rule, in BOTH directions. The counterexample that
  //      pins it: `mpin \x80\x80 1234` is REDACTED in the shipped order (the
  //      window still reaches "mpin") and is NOT redacted if canonicalisation runs
  //      first (the two U+FFFDs occupy 6 bytes and push "mpin" out of the window).
  //      The converse also exists — `passwordtokentotp\xC0\x80` + `12345678` leaks
  //      under the shipped order and is caught if normalisation runs first —
  //      so this is NOT an argument that scrub-first redacts more; it is the
  //      argument that the order is OBSERVABLE and must therefore be FIXED.
  //
  //      WHICH IS WHY: scrub() keeps running on the RAW bytes, exactly where it
  //      has always run and exactly what every existing redaction test pins, and
  //      canonicalisation stays LAST. The scrub call and its argument are
  //      UNCHANGED from before IMP-16/IMP-17 — free-form redaction is not relaxed
  //      or altered by one byte — and kRedactionMarker is pure ASCII so
  //      normalisation cannot touch a marker already emitted.
  //
  // RESIDUAL VECTOR, NAMED PRECISELY (unchanged, now handled rather than merely
  // named): broker JSON is not the exposure — nlohmann has already validated
  // anything parsed from a broker response. What reaches append() as raw bytes is
  // CALLER- and STORE-SUPPLIED text, and that is exactly what canonical_text()
  // covers. NOTE the scope of "AUTHORED": this choke point governs text the
  // process AUTHORS. Text PARSED BACK from the file by load() is covered by a
  // different guarantee — nlohmann's parser rejects a JSON string containing a raw
  // ill-formed byte — see the note on load() in ledger.hpp.
  // IMP-16's typed provenance columns were never a way in either: the
  // block guard in domain::render_provenance_block is an allowlist over
  // [A-Za-z0-9_#-*], so no byte >= 0x80 can enter through a provenance column at
  // all — but the general payload does not rely on that.
  LedgerEntry entry;
  entry.seq = static_cast<std::int64_t>(entries_.size());
  entry.prev_hash = entries_.empty() ? std::string{} : entries_.back().hash;
  entry.payload = domain::canonical_text(domain::scrub(payload) + provenance_block(provenance));
  // HASH WHAT WE STORE, LITERALLY: read the preimage back out of the field that
  // entry_to_line() will serialise. Mirrors verify_chain() exactly.
  entry.hash = sha256_hex(entry.prev_hash + entry.payload);
  if (entry.hash.empty()) {
    return fail(crypto_error("ledger: SHA-256 hashing failed"));
  }

  const std::string line = entry_to_line(entry) + '\n';

  // Append one JSON line and fsync through the platform durability seam, so a
  // crash after return cannot lose a record that we reported as written.
  std::FILE* fp = std::fopen(path_.string().c_str(), "ab");
  if (fp == nullptr) {
    return fail(
        make_error(ErrorCategory::Internal, "ledger: failed to open ledger file for append"));
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
      std::string message = "ledger chain broken at seq " + std::to_string(i);
      // ── TRIAGE FACTS — WORDING ONLY, NEVER A TOLERANCE ───────────────────────
      //
      // When the payload hash alone fails while the link and the seq are intact,
      // and the stored payload contains a U+FFFD, we append THE THREE OBSERVED
      // FACTS to the message. Nothing more.
      //
      // WE DO NOT ASSERT A CAUSE, AND THAT IS DELIBERATE. An earlier version of
      // this hint told the operator the entry "MAY PREDATE the IMP-17 fix". That
      // is an UNVERIFIABLE PROVENANCE HYPOTHESIS about a record we cannot date,
      // and ANY payload-only edit can trigger it simply by including the three
      // bytes EF BF BD — so the sentence is exactly the sentence an attacker
      // would choose to have printed next to their edit. Facts are safe to print;
      // a guess at history is not. The RUNBOOK owns the interpretation, where the
      // deployment's own timeline (when the binary shipped, when the file was last
      // written, what the checkpoint says) is actually available.
      //
      // This adds NO tolerance whatsoever: the call still returns the same
      // fail(Validation) with the same "seq <n>" prefix, so every caller and every
      // safe-start blocker behaves identically.
      if (!hash_ok && link_ok && seq_ok &&
          entry.payload.find(domain::kUtf8Replacement) != std::string::npos) {
        message +=
            " (observed: payload hash mismatch; link and seq intact; stored payload contains "
            "U+FFFD. These are facts for the runbook to interpret, NOT an exoneration — the entry "
            "is BROKEN and still blocks safe start.)";
      }
      return fail(make_error(ErrorCategory::Validation, std::move(message)));
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
    const bool well_formed = !parsed.is_discarded() && parsed.is_object() &&
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

std::size_t Ledger::size() const noexcept {
  return entries_.size();
}

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
    return fail(
        make_error(ErrorCategory::Validation, "ledger: Ed25519 private key must be 32 bytes"));
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
    return fail(
        make_error(ErrorCategory::Validation, "ledger: Ed25519 public key must be 32 bytes"));
  }
  Pkey pkey(
      EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, public_key.data(), public_key.size()));
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
    return fail(
        make_error(ErrorCategory::Internal, "ledger: failed to create public-key directory"));
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
  const bool well_formed = !parsed.is_discarded() && parsed.is_object() &&
                           parsed.contains("entry_count") &&
                           parsed["entry_count"].is_number_integer() &&
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
  // rendered through the whole-column allowlist and appended after it, and then —
  // IMP-17 — the SAME canonicalisation choke point append() uses, in the SAME
  // position (last). `exposure` is not itself hashed, but to_json() dumps it with
  // replace, so without this the struct in memory and the JSON the operator reads
  // would disagree on any ill-formed byte, and a caller that forwards `exposure`
  // into append() would carry that divergence straight into the chain.
  hb.exposure =
      domain::canonical_text(domain::scrub(exposure_summary) + provenance_block(provenance));
  return hb;
}

std::string EodReport::to_json() const {
  json out = json::object();
  out["entry_count"] = entry_count;
  // ── IMP-17 (B2): NO canonical_text HERE, AND THAT IS THE FIX, NOT AN OMISSION.
  //
  // `head_hash` is the ANTI-TAMPER ANCHOR, and its invariant is the SIGNED bytes
  // and the STORED bytes must be the same bytes. sign_head() signs the RAW
  // `head_hash()` string; eod_report() stores that same raw value; so to_json()
  // must WRITE that same raw value. Canonicalising in ONLY ONE of the two places
  // was the IMP-17 asymmetry itself — hash (here, sign) one string and store
  // another — reintroduced in the single place where it matters most:
  // write_checkpoint() persists these bytes as the signed high-water mark, and
  // verify_against_checkpoint() then (a) verifies the STORED head against a
  // signature taken over the RAW head and (b) compares it to a RAW in-memory
  // entry hash at :835. One value, produced once by head_hash(), now flows into
  // the signature, the struct and the JSON with NO transform anywhere.
  //
  // It bought nothing to remove: head_hash is lowercase hex from to_hex() (or a
  // string that verify_chain() would already reject), so canonical_text was
  // provably the identity on every value that can reach here. And it is not a
  // no-throw hole: dump_compact still serialises with error_handler_t::replace, so
  // a hand-filled EodReport carrying an ill-formed byte renders instead of
  // throwing — and then FAILS CLOSED at verify_head(), which is the correct
  // outcome for a head nobody signed. The signature/public_key fields are hex from
  // to_hex() and are ASCII by construction.
  out["head_hash"] = head_hash;
  out["signature"] = to_hex(signature);
  out["public_key"] = to_hex(public_key);
  return dump_compact(out);
}

std::string PositionHeartbeat::to_json() const {
  json out = json::object();
  // BOTH fields, for the SAME reason (IMP-17/C2). `ts` is our own ISO-8601 ASCII
  // and `exposure` is already canonical whenever the struct came from
  // make_heartbeat(), so canonicalising either is a NO-OP by construction
  // (canonical_text is idempotent). But PositionHeartbeat is a PLAIN AGGREGATE a
  // caller or a test can fill BY HAND, and this to_json() is the operator's view
  // of it — so every hand-fillable field gets the same treatment and the JSON the
  // operator reads can never differ from the field in memory. Treating one field
  // as trusted and the other as untrusted, in the same struct, is exactly the kind
  // of asymmetry IMP-17 exists to remove.
  //
  // (Unlike EodReport::head_hash above, NEITHER field here is signed or hashed, so
  // there is no second producer these bytes must agree with — canonicalising is
  // free, and the only correct choice is to do it consistently.)
  out["ts"] = domain::canonical_text(ts);
  out["exposure"] = domain::canonical_text(exposure);
  return dump_compact(out);
}

}  // namespace broker_exec::ledger
