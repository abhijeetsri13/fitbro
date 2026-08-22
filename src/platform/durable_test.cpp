#include "broker_exec/platform/durable.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using broker_exec::platform::durable_sync;
using broker_exec::platform::durable_sync_directory;
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

TEST_CASE("durable_sync_directory commits the entry a rename created", "[platform]") {
  // The publish half of write-temp -> sync -> rename. Syncing the file is not
  // enough: on POSIX the rename is a change to the *directory*, and an unsynced
  // directory can come back after a crash still pointing at the old entry.
  const fs::path dir = fs::temp_directory_path() / "broker_exec_durable_dir_test";
  fs::remove_all(dir);
  REQUIRE(fs::create_directories(dir));

  const fs::path tmp = dir / "payload.tmp";
  const fs::path target = dir / "payload";
  {
    std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
    REQUIRE(f != nullptr);
    REQUIRE(std::fwrite("x", 1, 1, f) == 1);
    REQUIRE(std::fflush(f) == 0);
    REQUIRE(durable_sync(portable_fileno(f)));
    REQUIRE(std::fclose(f) == 0);
  }
  fs::rename(tmp, target);

  CHECK(durable_sync_directory(dir));

  fs::remove_all(dir);
}

TEST_CASE("durable_sync_directory reports failure for a path that is not a directory",
          "[platform]") {
  const fs::path missing = fs::temp_directory_path() / "broker_exec_no_such_dir_zzz";
  fs::remove_all(missing);
#ifdef _WIN32
  // Documented no-op: Win32 exposes no directory handle to flush, so the seam
  // reports success because the operation is inapplicable (see durable.hpp).
  CHECK(durable_sync_directory(missing));
#else
  CHECK_FALSE(durable_sync_directory(missing));
#endif
}
