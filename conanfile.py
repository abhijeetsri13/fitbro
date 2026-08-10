from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMakeDeps, cmake_layout


class BrokerExecConan(ConanFile):
    """Conan v2 recipe for the broker-neutral trading execution library.

    Dependencies are added per-module as each story that needs them lands, so a
    clean checkout never builds a transport stack it does not yet use. The full
    target set (per architecture.md) is: cpr/libcurl, ixwebsocket, nlohmann_json,
    tomlplusplus, spdlog/fmt, openssl, libsodium, sqlite3, cli11, cpp-httplib,
    catch2.
    """

    name = "broker_exec"
    version = "0.1.0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeToolchain", "CMakeDeps"

    def requirements(self):
        # Runtime deps, added per-module as stories land. SQLite is vendored
        # (third_party/sqlite3) rather than taken from Conan, to guarantee a
        # static, byte-identical, DLL-free build across Linux/macOS/Windows.
        self.requires("nlohmann_json/3.11.3")  # intent-log JSON lines (Story 1.5)
        self.requires("tomlplusplus/3.4.0")  # typed configuration (Story 2.1)
        self.requires("openssl/3.2.1")  # AES-256-GCM token encryption at rest (Story 2.2)
        self.requires("cpr/1.10.5")  # Kite Connect REST transport over libcurl (Story 2.3)
        self.requires("spdlog/1.14.1")  # structured JSON logging + redaction (Story 4.1)
        # Story 4.6 — thin operator CLI + localhost health endpoint (both header-only).
        self.requires("cli11/2.4.2")  # operator CLI subcommands
        self.requires("cpp-httplib/0.15.3")  # localhost health endpoint transport
        # Test framework.
        self.test_requires("catch2/3.5.2")

    def layout(self):
        cmake_layout(self)
