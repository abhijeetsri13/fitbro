# broker_exec::boot — THE PROCESS BOOT SEQUENCE AND THE EXIT-CODE CONTRACT
# (IMP-20).
#
# Until this module there was no `int main` in src/ and therefore no production
# caller for any of the safety machinery: no SafeStartContext was constructed
# outside a test, the ledger had no production caller, and the IMP-12 composition
# root was never invoked. Worse, deploy/systemd/broker-exec@.service pointed
# ExecStart at a `broker-exec` binary this repository did not build.
#
# This target is the boot half of the program: the cold-boot sequence, the ten
# safe-start checks wired to REAL components, and the exit-code contract the
# committed systemd unit's `RestartPreventExitStatus=70` depends on. `src/main`
# holds the executable, which is a thin shell over `boot()`.
#
# WHY IT LINKS SO MUCH, AND WHY THAT IS CORRECT: a composition root is the
# OUTERMOST layer. It is the one place allowed to know every module, so that
# nothing else has to. The hexagonal rule enforced in the top-level CMakeLists is
# the other direction (domain/ports must never link outward), and this target is
# added to BROKER_EXEC_OUTER_TARGETS there so an inner layer reaching it trips the
# configure-time check — it transitively links every adapter through
# broker_exec_composition.
#
# Everything below is PUBLIC because every one of them appears in boot.hpp:
#   accounts     -> AccountDataDir / DirPermissionFn in BootDeps + BootOutcome
#   cli          -> HealthState (the snapshot holder boot publishes into)
#   clock        -> SkewStallDetector (the clock check's real component)
#   composition  -> Broker/EngineDeps + Options + the two assemblies
#   config       -> config::Config, config::EnvLookup
#   domain       -> domain::Order (the reconciliation/legacy-stop checks)
#   errors       -> the Result<T>/Error vocabulary
#   ledger       -> ledger::Ledger
#   ports        -> ClockPort / SecretProvider / AlertSink / Ok
#   refdata      -> InstrumentMaster / TradingCalendar (the freshness checks)
#   session      -> SafeStartContext / SafeStartGate / SessionState
#   store        -> store::Store (the projection the last two checks read)
#   supervisor   -> the exit-code constants exit_code_for() must reproduce
#
# NO new Conan dependency: every one of these is first-party, and the transports
# arrive transitively through broker_exec_composition, which already links them.

add_library(broker_exec_boot STATIC
  boot.cpp
  boot_checks.cpp
)
add_library(broker_exec::boot ALIAS broker_exec_boot)

target_include_directories(broker_exec_boot
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_boot PUBLIC cxx_std_20)

target_link_libraries(broker_exec_boot
  PUBLIC
    broker_exec_accounts
    broker_exec_cli
    broker_exec_clock
    broker_exec_composition
    broker_exec_config
    broker_exec_domain
    broker_exec_errors
    broker_exec_ledger
    broker_exec_ports
    broker_exec_refdata
    broker_exec_session
    broker_exec_store
    broker_exec_supervisor
  PRIVATE
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  # The suite drives boot() over REAL components (a real Store, a real Ledger, a
  # real InstrumentMaster/TradingCalendar, a real SkewStallDetector, the real
  # composition root) with only the outermost seams faked — the HTTP transport,
  # the secret provider, the alert sink, the session probe, the health-endpoint
  # bind and the run phase. It binds no socket and issues no request.
  add_executable(broker_exec_boot_tests boot_test.cpp)
  target_link_libraries(broker_exec_boot_tests
    PRIVATE
      broker_exec_boot
      broker_exec_capabilities  # the declared capability list + FixtureCertification
      broker_exec_clock         # TestClock (deterministic ClockPort)
      broker_exec_domain        # Order/OrderState fixtures
      broker_exec_modes         # KillState / PostureCoordinator for the engine deps
      broker_exec_supervisor    # the exit-code round-trip
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_boot_tests COMMAND broker_exec_boot_tests)
endif()
