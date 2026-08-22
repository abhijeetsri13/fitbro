#include "sha256.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>

using broker_exec::intentlog::Sha256;
using broker_exec::intentlog::sha256_hex;

// NIST FIPS 180-4 / standard known-answer vectors. These pin the vendored
// implementation to the spec so the hash chain is reproducible across compilers
// and platforms.
TEST_CASE("sha256 NIST known-answer vectors", "[intentlog][sha256]") {
  // Empty string.
  REQUIRE(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  // "abc" (FIPS 180-4 example 1).
  REQUIRE(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  // 448-bit message (FIPS 180-4 example 2).
  REQUIRE(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

  // One million 'a' characters (classic SHA-256 long-message vector).
  std::string million_a(1000000, 'a');
  REQUIRE(sha256_hex(million_a) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sha256 streaming update matches one-shot", "[intentlog][sha256]") {
  Sha256 hasher;
  hasher.update("abcdbcdecdefdefgefghfghighijhijk");
  hasher.update("ijkljklmklmnlmnomnopnopq");
  REQUIRE(hasher.hex() == sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"));
}
