# broker_exec::fillnorm — the CANONICAL FILL NORMALIZER (broker-neutral,
# fail-closed). Normalizes any broker order-event (Kite COMPLETE/OPEN/UPDATE/
# REJECTED/CANCELLED/TRIGGER PENDING + filled/pending qty; Kotak ordSt) into ONE
# canonical (state, filled, pending) DRIVEN OFF THE FILLED QUANTITY — never the
# event type — so a partial fill arriving as an "UPDATE" is read as PartiallyFilled
# and never as not-filled. Marks whether the snapshot came from an AUTHORITATIVE
# reconcile read vs a non-authoritative push: only a reconciled fill qty may size
# an exit. Pure-decision module: domain (OrderState/to_string + scrub) appears in
# the public surface so it is PUBLIC; brokerreason (the unknown-status ⇒ reconcile
# fail-safe) is an implementation detail so it is PRIVATE. NO new Conan dep.

add_library(broker_exec_fillnorm STATIC
  fill_normalizer.cpp
)
add_library(broker_exec::fillnorm ALIAS broker_exec_fillnorm)

target_include_directories(broker_exec_fillnorm
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_features(broker_exec_fillnorm PUBLIC cxx_std_20)

# domain::OrderState + domain::to_string appear in the public header (FillSnapshot,
# detail), so domain is PUBLIC. brokerreason::classify_status is used only inside
# the .cpp for the unrecognized-status ⇒ reconcile fallback, so it links PRIVATE.
# The warnings/sanitizers policy stays PRIVATE.
target_link_libraries(broker_exec_fillnorm
  PUBLIC
    broker_exec_domain
  PRIVATE
    broker_exec_brokerreason
    broker_exec_warnings
    broker_exec_sanitizers)

if(BROKER_EXEC_BUILD_TESTS)
  add_executable(broker_exec_fillnorm_tests fill_normalizer_test.cpp)
  target_link_libraries(broker_exec_fillnorm_tests
    PRIVATE
      broker_exec_fillnorm
      Catch2::Catch2WithMain
      broker_exec_warnings
      broker_exec_sanitizers)
  add_test(NAME broker_exec_fillnorm_tests COMMAND broker_exec_fillnorm_tests)
endif()
