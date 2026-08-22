#include "broker_exec/platform/durable.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using broker_exec::platform::durable_sync;
using broker_exec::platform::portable_fileno;

TEST_CASE("durable_sync flushes an open file and reports success", "[platform]") {
  const fs::path path = fs::temp_directory_path() / "broker_exec_durable_test.tmp";

  std::FILE* f = std::fopen(path.string().c_str(), "wb");
  REQUIRE(f != nullptr);

  const std::string payload = "intent-record\n";
  REQUIRE(std::fwrite(payload.data(), 1, payload.size(), f) == payload.size());
  REQUIRE(std::fflush(f) == 0);

  const int fd = portable_fileno(f);
  REQUIRE(fd >= 0);
  REQUIRE(durable_sync(fd));  // POSIX fsync / Windows _commit

  REQUIRE(std::fclose(f) == 0);
  REQUIRE(fs::file_size(path) == payload.size());
  fs::remove(path);
}

TEST_CASE("durable_sync rejects an invalid descriptor", "[platform]") {
  REQUIRE_FALSE(durable_sync(-1));
  REQUIRE(portable_fileno(nullptr) == -1);
}
