# broker-exec — THE EXECUTABLE (IMP-20).
#
# THE TARGET NAME IS DELIBERATELY `broker-exec`, NOT `broker_exec_main`. This is a
# PROGRAM, not a module, and the name is a deployment contract: the committed
# deploy/systemd/broker-exec@.service invokes
# `/usr/local/bin/broker-exec run --account %i ...`. The "one CMake target per
# module named broker_exec_<module>" rule in docs/conventions.md governs LIBRARY
# targets — an executable whose name systemd hard-codes is the one place that rule
# would produce the wrong answer, so the deviation is stated here rather than
# hidden. (OUTPUT_NAME is set explicitly too, so a generator that decorates target
# names still emits exactly `broker-exec`.)
#
# main.cpp is a thin shell over broker_exec::boot::boot(): it parses argv with
# CLI11 (the same usage pattern as src/cli/cli_app.cpp, and CLI11 stays confined
# to that one translation unit), constructs the REAL components, and maps the
# returned ExitClass to a process exit code.
#
# It links the concrete production seams the library never links for itself —
# the cpr/libcurl Kite transport (broker_exec_kite), the env-backed secret
# provider (broker_exec_secrets), the SQLite projection (broker_exec_store), the
# encrypted token store and the daily Kite session establisher
# (broker_exec_session). NO new Conan dependency: CLI11 is already used by
# broker_exec_cli, and cpr/OpenSSL/SQLite arrive through the existing targets.

add_executable(broker-exec main.cpp)

set_target_properties(broker-exec PROPERTIES OUTPUT_NAME broker-exec)

# A developer machine with vcpkg's GLOBAL MSBuild integration enabled
# (`vcpkg integrate install`) has every MSBuild project silently linked against
# vcpkg's own libraries. That injects a SECOND zlib — vcpkg's import lib — beside
# the static one Conan already supplies through libcurl, and the executable then
# fails to link with LNK2005/LNK1169 on every zlib symbol. Only this target is
# affected: it is the one that links the cpr/libcurl transport into a program.
#
# Opting out per-target keeps the DEFAULT build working on such a machine, rather
# than leaving `-p:VcpkgEnabled=false` as folklore a newcomer has to rediscover
# from a wall of duplicate-symbol errors. Harmless everywhere else: on a machine
# without the integration the property is simply unread, and non-MSBuild
# generators ignore VS_GLOBAL_* entirely. Every dependency here comes from Conan,
# so there is nothing this opt-out can take away.
set_target_properties(broker-exec PROPERTIES VS_GLOBAL_VcpkgEnabled false)

target_compile_features(broker-exec PRIVATE cxx_std_20)

target_link_libraries(broker-exec
  PRIVATE
    broker_exec_boot          # the sequence + the exit-code contract
    broker_exec_accounts      # AccountDataDir + SharedRefdataCache
    broker_exec_capabilities  # the declared capability list
    broker_exec_cli           # HealthState + the localhost health endpoint
    broker_exec_clock         # SystemClock (the ONLY real-clock call site)
    broker_exec_composition   # make_broker / make_engine
    broker_exec_config
    broker_exec_domain        # domain::scrub on every operator-visible line
    broker_exec_kite          # CprHttpClient + KiteRestClient (the real transport)
    broker_exec_ledger
    broker_exec_modes         # KillState / PostureCoordinator
    broker_exec_refdata
    broker_exec_secrets       # EnvSecretProvider + TokenStore
    broker_exec_session       # KiteSessionEstablisher
    broker_exec_store
    CLI11::CLI11              # confined to main.cpp, exactly as in src/cli
    broker_exec_warnings
    broker_exec_sanitizers)
