#include "broker_exec/secrets/token_store.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/platform/permissions.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::secrets {

namespace fs = std::filesystem;

using ::broker_exec::errors::Error;
using ::broker_exec::errors::ErrorCategory;
using ::broker_exec::errors::make_error;

namespace {

constexpr std::size_t kKeyBytes = 32;  // AES-256
constexpr std::size_t kIvBytes = 12;   // 96-bit GCM IV (the recommended size)
constexpr std::size_t kTagBytes = 16;  // 128-bit GCM auth tag

// RAII owner for an EVP_CIPHER_CTX: freed on every path (success or early
// return), so no OpenSSL context can leak across our no-throw boundary.
class CipherCtx {
 public:
  CipherCtx() noexcept : ctx_(EVP_CIPHER_CTX_new()) {}
  ~CipherCtx() {
    if (ctx_ != nullptr) {
      EVP_CIPHER_CTX_free(ctx_);
    }
  }
  CipherCtx(const CipherCtx&) = delete;
  CipherCtx& operator=(const CipherCtx&) = delete;
  CipherCtx(CipherCtx&&) = delete;
  CipherCtx& operator=(CipherCtx&&) = delete;

  [[nodiscard]] EVP_CIPHER_CTX* get() const noexcept { return ctx_; }
  [[nodiscard]] explicit operator bool() const noexcept { return ctx_ != nullptr; }

 private:
  EVP_CIPHER_CTX* ctx_;
};

// Crypto/op failures escalate (RaiseAlert) — a failed cipher op or a tag
// mismatch is a security-relevant event, never a value to silently retry.
[[nodiscard]] Error crypto_error(std::string message) {
  return make_error(ErrorCategory::Internal, std::move(message));
}

// A reinterpret helper kept in one place so the casts stay warning-clean.
[[nodiscard]] const unsigned char* as_u8(const char* p) noexcept {
  return reinterpret_cast<const unsigned char*>(p);
}
[[nodiscard]] unsigned char* as_u8(char* p) noexcept {
  return reinterpret_cast<unsigned char*>(p);
}

// Encrypt `pt` under the 256-bit `key`. Returns IV‖ciphertext‖tag on success.
[[nodiscard]] Result<std::string> aes_gcm_encrypt(std::string_view key, std::string_view pt) {
  unsigned char iv[kIvBytes];
  if (RAND_bytes(iv, static_cast<int>(kIvBytes)) != 1) {
    return fail(crypto_error("token store: secure IV generation failed"));
  }

  CipherCtx ctx;
  if (!ctx) {
    return fail(crypto_error("token store: cipher context allocation failed"));
  }

  if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kIvBytes), nullptr) !=
          1 ||
      EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, as_u8(key.data()), iv) != 1) {
    return fail(crypto_error("token store: encrypt init failed"));
  }

  std::string ct(pt.size(), '\0');
  int out_len = 0;
  if (!pt.empty()) {
    if (EVP_EncryptUpdate(ctx.get(), as_u8(ct.data()), &out_len, as_u8(pt.data()),
                          static_cast<int>(pt.size())) != 1) {
      return fail(crypto_error("token store: encrypt update failed"));
    }
  }

  int fin_len = 0;
  if (EVP_EncryptFinal_ex(ctx.get(), as_u8(ct.data()) + out_len, &fin_len) != 1) {
    return fail(crypto_error("token store: encrypt finalize failed"));
  }
  ct.resize(static_cast<std::size_t>(out_len + fin_len));

  unsigned char tag[kTagBytes];
  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagBytes), tag) != 1) {
    return fail(crypto_error("token store: GCM tag retrieval failed"));
  }

  std::string blob;
  blob.reserve(kIvBytes + ct.size() + kTagBytes);
  blob.append(reinterpret_cast<const char*>(iv), kIvBytes);
  blob.append(ct);
  blob.append(reinterpret_cast<const char*>(tag), kTagBytes);
  return blob;
}

// Decrypt and authenticate `blob` (IV‖ciphertext‖tag) under `key`. Fails closed
// on a short blob or a tag mismatch — never returns unauthenticated plaintext.
[[nodiscard]] Result<std::string> aes_gcm_decrypt(std::string_view key, std::string_view blob) {
  if (blob.size() < kIvBytes + kTagBytes) {
    return fail(make_error(ErrorCategory::Validation,
                           "token store: encrypted blob is too short to be valid"));
  }

  const unsigned char* iv = as_u8(blob.data());
  const std::size_t ct_len = blob.size() - kIvBytes - kTagBytes;
  const char* ct = blob.data() + kIvBytes;
  const unsigned char* tag = as_u8(blob.data() + kIvBytes + ct_len);

  CipherCtx ctx;
  if (!ctx) {
    return fail(crypto_error("token store: cipher context allocation failed"));
  }

  if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kIvBytes), nullptr) !=
          1 ||
      EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, as_u8(key.data()), iv) != 1) {
    return fail(crypto_error("token store: decrypt init failed"));
  }

  std::string pt(ct_len, '\0');
  int out_len = 0;
  if (ct_len > 0) {
    if (EVP_DecryptUpdate(ctx.get(), as_u8(pt.data()), &out_len, as_u8(ct),
                          static_cast<int>(ct_len)) != 1) {
      OPENSSL_cleanse(pt.data(), pt.size());
      return fail(crypto_error("token store: decrypt update failed"));
    }
  }

  // Hand OpenSSL the expected tag; EVP_DecryptFinal_ex returns >0 ONLY when it
  // verifies. The ctrl ptr is non-const in the API, hence the const_cast.
  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTagBytes),
                          const_cast<unsigned char*>(tag)) != 1) {
    OPENSSL_cleanse(pt.data(), pt.size());
    return fail(crypto_error("token store: GCM tag set failed"));
  }

  int fin_len = 0;
  const int rc = EVP_DecryptFinal_ex(ctx.get(), as_u8(pt.data()) + out_len, &fin_len);
  if (rc <= 0) {
    // FAIL CLOSED: tampered ciphertext/tag or wrong key. Discard any plaintext.
    OPENSSL_cleanse(pt.data(), pt.size());
    return fail(crypto_error("token store: authentication failed (tampered data or wrong key)"));
  }

  pt.resize(static_cast<std::size_t>(out_len + fin_len));
  return pt;
}

// Reject path components that could escape the data dir (traversal / separators).
[[nodiscard]] bool is_safe_component(std::string_view component) noexcept {
  if (component.empty() || component == "." || component == "..") {
    return false;
  }
  for (char c : component) {
    if (c == '/' || c == '\\' || c == '\0') {
      return false;
    }
  }
  return true;
}

}  // namespace

TokenStore::TokenStore(const ports::SecretProvider& key_source, std::filesystem::path data_dir)
    : key_source_(&key_source), data_dir_(std::move(data_dir)) {}

Result<std::string> TokenStore::resolve_key(std::string_view account) const {
  // Account-scoped key NAME (not the value): the key material lives in the
  // SecretProvider, never co-located with the ciphertext on disk (SEC-1).
  std::string key_name(account);
  key_name += ".token_key";

  auto resolved = key_source_->get(key_name);
  if (!resolved) {
    return resolved;  // provider Error is already redaction-safe
  }
  if (resolved.value().size() != kKeyBytes) {
    // Wrong-sized but still secret key material — zero it before discarding, as
    // the success paths do, so a rejected key does not linger in memory.
    OPENSSL_cleanse(resolved.value().data(), resolved.value().size());
    // Length is not secret, but we keep the message generic and never echo bytes.
    return fail(make_error(ErrorCategory::Validation,
                           "token store: per-account encryption key must be 32 bytes (256-bit)"));
  }
  return resolved;
}

Result<ports::Ok> TokenStore::save(std::string_view account, std::string_view name,
                                   std::string_view plaintext) const {
  if (!is_safe_component(account) || !is_safe_component(name)) {
    return fail(make_error(ErrorCategory::Validation,
                           "token store: account/name must be a single safe path component"));
  }

  auto key_result = resolve_key(account);
  if (!key_result) {
    return fail(std::move(key_result).error());
  }
  std::string key = std::move(key_result).value();

  auto blob_result = aes_gcm_encrypt(key, plaintext);
  OPENSSL_cleanse(key.data(), key.size());  // zero key material after use
  if (!blob_result) {
    return fail(std::move(blob_result).error());
  }
  const std::string& blob = blob_result.value();

  const fs::path account_dir = data_dir_ / std::string(account);
  std::error_code ec;
  fs::create_directories(account_dir, ec);
  if (ec) {
    return fail(make_error(ErrorCategory::Internal,
                           "token store: failed to create account directory"));
  }
  // Tighten the directory to owner-only (0700) before writing the blob. A failed
  // tighten on POSIX would leave the blob group/world-readable, so we must not
  // report success; the platform seam returns true on its Windows best-effort path.
  if (!platform::restrict_to_owner_dir(account_dir)) {
    return fail(make_error(ErrorCategory::Internal,
                           "token store: failed to restrict account directory permissions"));
  }

  const fs::path file = account_dir / (std::string(name) + ".enc");
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
      return fail(make_error(ErrorCategory::Internal,
                             "token store: failed to open token file for writing"));
    }
    out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
    out.flush();
    if (!out) {
      return fail(make_error(ErrorCategory::Internal, "token store: failed to write token file"));
    }
  }
  // Tighten the blob file to owner-only (0600). A failed tighten on POSIX would
  // leave the encrypted blob group/world-readable while save() falsely reports
  // success, so we surface it; the platform seam returns true on its Windows
  // best-effort path.
  if (!platform::restrict_to_owner_file(file)) {
    return fail(make_error(ErrorCategory::Internal,
                           "token store: failed to restrict token file permissions"));
  }

  return ports::ok();
}

Result<std::string> TokenStore::load(std::string_view account, std::string_view name) const {
  if (!is_safe_component(account) || !is_safe_component(name)) {
    return fail(make_error(ErrorCategory::Validation,
                           "token store: account/name must be a single safe path component"));
  }

  const fs::path file = data_dir_ / std::string(account) / (std::string(name) + ".enc");
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return fail(make_error(ErrorCategory::Validation, "token store: token file not found"));
  }
  std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) {
    return fail(make_error(ErrorCategory::Internal, "token store: failed to read token file"));
  }

  auto key_result = resolve_key(account);
  if (!key_result) {
    return fail(std::move(key_result).error());
  }
  std::string key = std::move(key_result).value();

  auto plaintext = aes_gcm_decrypt(key, blob);
  OPENSSL_cleanse(key.data(), key.size());  // zero key material after use
  return plaintext;
}

}  // namespace broker_exec::secrets
