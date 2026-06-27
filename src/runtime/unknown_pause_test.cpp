#include "broker_exec/runtime/unknown_pause.hpp"

#include <catch2/catch_test_macros.hpp>

using broker_exec::runtime::UnknownPause;

namespace {
constexpr bool kEntry = false;          // a risk-INCREASING entry
constexpr bool kRiskReducingExit = true;  // a risk-REDUCING exit
}  // namespace

// ── Default posture: not paused, everything allowed ──────────────────────────
TEST_CASE("UnknownPause: fresh latch is not paused and allows entries", "[runtime][unknown][pause]") {
  UnknownPause pause;
  CHECK_FALSE(pause.is_paused());
  CHECK(pause.outstanding() == 0);
  CHECK(pause.allows(kEntry));
  CHECK(pause.allows(kRiskReducingExit));
}

// ── (e) blocks an entry while paused, allows a risk-reducing exit, clears ─────
TEST_CASE("UnknownPause: blocks entries while paused but always allows exits",
          "[runtime][unknown][pause]") {
  UnknownPause pause;
  pause.mark_unknown("alpha-ref-1");

  CHECK(pause.is_paused());
  CHECK(pause.outstanding() == 1);

  // The headline asymmetry: an entry is BLOCKED, a risk-reducing exit PASSES.
  CHECK_FALSE(pause.allows(kEntry));
  CHECK(pause.allows(kRiskReducingExit));

  // Resolving the UNKNOWN clears the pause -> entries flow again.
  pause.clear("alpha-ref-1");
  CHECK_FALSE(pause.is_paused());
  CHECK(pause.outstanding() == 0);
  CHECK(pause.allows(kEntry));
}

// ── Membership semantics: idempotent mark, no-op clear, multi-ref ────────────
TEST_CASE("UnknownPause: mark is idempotent and clear is per-ref", "[runtime][unknown][pause]") {
  UnknownPause pause;

  // Marking the same ref twice does not double-count (membership, not a counter).
  pause.mark_unknown("ref-A");
  pause.mark_unknown("ref-A");
  CHECK(pause.outstanding() == 1);

  // A second, distinct UNKNOWN keeps the process paused until BOTH resolve.
  pause.mark_unknown("ref-B");
  CHECK(pause.outstanding() == 2);
  CHECK(pause.is_paused());

  pause.clear("ref-A");
  CHECK(pause.outstanding() == 1);
  CHECK(pause.is_paused());  // ref-B still outstanding
  CHECK_FALSE(pause.allows(kEntry));

  // Clearing a never-marked ref is a no-op.
  pause.clear("ref-does-not-exist");
  CHECK(pause.outstanding() == 1);

  pause.clear("ref-B");
  CHECK_FALSE(pause.is_paused());
  CHECK(pause.allows(kEntry));
}
