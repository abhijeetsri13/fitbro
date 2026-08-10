# broker_exec::brokerreason — the VERSIONED CANONICAL rejection/status classifier
# (broker-neutral, fail-closed). ONE versioned table maps raw broker reject text +
# status to a canonical reason + retry posture; the fail-closed default treats any
# unclassifiable reject as do-not-retry + reconcile + alert (never safe-to-retry).
# Pure-decision module: depends inward only on errors (SuggestedAction bridge,
# PUBLIC — it appears in the public header) and domain (scrub, redaction-safe
# detail). NO new Conan dep.

add_library(broker_exec_brokerreason STATIC
  rejection_classifier.cpp
)
add_library(broker_exec::brokerreason ALIAS broker_exec_brokerreason)

target_include_directories(broker_exec_brokerreason
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_brokerreason PUBLIC cxx_std_20)

# errors::SuggestedAction appears in the public header (to_suggested_action), so
# errors is PUBLIC. domain::scrub is an implementation detail of the .cpp, but the
# classifier's redaction-safety contract is part of its public guarantee and other
# pure modules link domain PUBLIC similarly; keep it PUBLIC for the scrub seam.
target_link_libraries(broker_exec_brokerreason
  PUBLIC
    broker_exec_errors
    broker_exec_domain
  PRIVATE
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_brokerreason_tests rejection_classifier_test.cpp)
  target_link_libraries(broker_exec_brokerreason_tests
    PRIVATE
      broker_exec_brokerreason
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_brokerreason_tests COMMAND broker_exec_brokerreason_tests)
endif()
