#include "broker_exec/modes/killswitch.hpp"

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/modes/posture.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

using broker_exec::errors::ErrorCategory;
using broker_exec::modes::KillCommand;
using broker_exec::modes::KillController;
using broker_exec::modes::KillState;
using broker_exec::modes::KillType;
using broker_exec::modes::Posture;
using broker_exec::modes::PostureCoordinator;

namespace ports = broker_exec::ports;

namespace {

// An in-memory persist seam: records every command it is asked to make durable
// and can be flipped to fail (to exercise the fail-closed path). The recorded
// vector doubles as the replay store.
class MemoryStore {
 public:
  broker_exec::Result<ports::Ok> persist(const KillCommand& cmd) {
    if (fail_) {
      return broker_exec::fail(broker_exec::errors::make_error(
          ErrorCategory::Internal, "store unavailable"));
    }
    saved_.push_back(cmd);
    return ports::ok();
  }

  [[nodiscard]] broker_exec::Result<std::vector<KillCommand>> load() const { return saved_; }

  [[nodiscard]] const std::vector<KillCommand>& saved() const noexcept { return saved_; }
  void set_fail(bool fail) noexcept { fail_ = fail; }

 private:
  std::vector<KillCommand> saved_;
  bool fail_ = false;
};

// A fake authenticator: the operator secret is "op-secret".
[[nodiscard]] bool fake_authenticate(std::string_view token) { return token == "op-secret"; }

// Build a controller wired to `store` with the fake authenticator.
[[nodiscard]] KillController make_controller(MemoryStore& store) {
  return KillController(fake_authenticate,
                        [&store](const KillCommand& c) { return store.persist(c); });
}

}  // namespace

TEST_CASE("a soft kill blocks entries but allows exits (AC-1)", "[modes][killswitch]") {
  KillState state;
  state.apply({KillType::Soft, ""});

  CHECK(state.blocks_entries());
  CHECK(state.allows_risk_reducing_exits());
  CHECK(state.posture() == Posture::SoftKill);
  CHECK_FALSE(state.panic_active());
  CHECK(state.any_active());
}

TEST_CASE("a strategy kill scopes to its strategy until a broader kill", "[modes][killswitch]") {
  KillState state;
  state.apply({KillType::Strategy, "alpha"});

  CHECK(state.blocks_strategy("alpha"));
  CHECK_FALSE(state.blocks_strategy("beta"));
  CHECK(state.blocks_entries());  // every kill blocks new entries

  // A broader (Account) kill now blocks every strategy, including "beta".
  state.apply({KillType::Account, ""});
  CHECK(state.blocks_strategy("beta"));
  CHECK(state.blocks_strategy("alpha"));
}

TEST_CASE("a broker/account kill blocks entries", "[modes][killswitch]") {
  KillState broker;
  broker.apply({KillType::Broker, "kite"});
  CHECK(broker.blocks_entries());
  CHECK(broker.blocks_strategy("anything"));

  KillState account;
  account.apply({KillType::Account, ""});
  CHECK(account.blocks_entries());
  CHECK(account.blocks_strategy("anything"));
}

TEST_CASE("a panic kill closes the normal exit gate (AC-1/AC-2)", "[modes][killswitch]") {
  KillState state;
  state.apply({KillType::Panic, ""});

  CHECK(state.panic_active());
  CHECK(state.posture() == Posture::Panic);
  CHECK_FALSE(state.allows_risk_reducing_exits());  // square-off runs out-of-band
  CHECK(state.blocks_entries());
}

TEST_CASE("apply is idempotent — re-applying the same kill is a no-op", "[modes][killswitch]") {
  KillState state;
  state.apply({KillType::Soft, ""});
  state.apply({KillType::Soft, ""});  // same (type, scope) again

  CHECK(state.any_active());
  CHECK(state.posture() == Posture::SoftKill);
  // A distinct strategy kill plus a duplicate of it still leaves one effect each.
  state.apply({KillType::Strategy, "alpha"});
  state.apply({KillType::Strategy, "alpha"});
  CHECK(state.blocks_strategy("alpha"));
}

TEST_CASE("submit with a good token persists then enqueues (AC-1)", "[modes][killswitch]") {
  MemoryStore store;
  KillController controller = make_controller(store);

  const auto r = controller.submit({KillType::Soft, ""}, "op-secret");
  REQUIRE(r.has_value());

  // The persist seam recorded exactly the submitted command.
  REQUIRE(store.saved().size() == 1);
  CHECK(store.saved().front() == KillCommand{KillType::Soft, ""});

  // drain() returns exactly that one command; applying it flips the flag.
  const std::vector<KillCommand> batch = controller.drain();
  REQUIRE(batch.size() == 1);
  CHECK(batch.front() == KillCommand{KillType::Soft, ""});

  KillState state;
  state.apply(batch.front());
  CHECK(state.blocks_entries());
}

TEST_CASE("submit with a bad token is rejected — nothing persisted or enqueued (AC-1)",
          "[modes][killswitch]") {
  MemoryStore store;
  KillController controller = make_controller(store);

  const auto r = controller.submit({KillType::Panic, ""}, "wrong");
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().category == ErrorCategory::Auth);

  // An unauthenticated kill never takes effect: nothing durable, nothing queued.
  CHECK(store.saved().empty());
  CHECK(controller.drain().empty());
}

TEST_CASE("submit fails closed when persist fails — not enqueued (AC-3)",
          "[modes][killswitch]") {
  MemoryStore store;
  store.set_fail(true);
  KillController controller = make_controller(store);

  const auto r = controller.submit({KillType::Soft, ""}, "op-secret");
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().category == ErrorCategory::Internal);

  // Never ack a kill we could not persist: nothing recorded, nothing enqueued.
  CHECK(store.saved().empty());
  CHECK(controller.drain().empty());
}

TEST_CASE("replay re-applies persisted kills — still killed after restart (AC-3)",
          "[modes][killswitch]") {
  MemoryStore store;
  KillController controller = make_controller(store);

  REQUIRE(controller.submit({KillType::Account, ""}, "op-secret").has_value());
  REQUIRE(controller.submit({KillType::Panic, ""}, "op-secret").has_value());

  // A simulated restart: a FRESH KillState replays the persisted store.
  KillState recovered;
  const auto r = KillController::replay(recovered, [&store] { return store.load(); });
  REQUIRE(r.has_value());

  CHECK(recovered.blocks_entries());
  CHECK(recovered.panic_active());
  CHECK(recovered.posture() == Posture::Panic);
}

TEST_CASE("replay surfaces a load failure (AC-3)", "[modes][killswitch]") {
  KillState state;
  const auto r = KillController::replay(state, [] {
    return broker_exec::fail(
        broker_exec::errors::make_error(ErrorCategory::Internal, "load failed"));
  });
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().category == ErrorCategory::Internal);
  CHECK_FALSE(state.any_active());
}

TEST_CASE("a queued kill takes effect only after drain+apply (AC-2 drain timing)",
          "[modes][killswitch]") {
  MemoryStore store;
  KillController controller = make_controller(store);
  REQUIRE(controller.submit({KillType::Panic, ""}, "op-secret").has_value());

  // BEFORE drain, the main loop's KillState is unaffected (the kill sits queued).
  KillState state;
  CHECK_FALSE(state.blocks_entries());
  CHECK_FALSE(state.panic_active());

  // The loop drains and applies — the flag flips (effect on the next iteration).
  for (const KillCommand& c : controller.drain()) {
    state.apply(c);
  }
  CHECK(state.blocks_entries());
  CHECK(state.panic_active());
}

TEST_CASE("submit rejects a malformed/mis-scoped kill — nothing persisted or enqueued",
          "[modes][killswitch]") {
  MemoryStore store;
  KillController controller = make_controller(store);

  // A scoped kill (Strategy/Broker/Account) with an EMPTY scope is malformed.
  const auto no_scope = controller.submit({KillType::Strategy, ""}, "op-secret");
  REQUIRE_FALSE(no_scope.has_value());
  CHECK(no_scope.error().category == ErrorCategory::Validation);

  // An unscoped kill (Soft/Panic) that CARRIES a scope is malformed.
  const auto soft_scoped = controller.submit({KillType::Soft, "oops"}, "op-secret");
  REQUIRE_FALSE(soft_scoped.has_value());
  CHECK(soft_scoped.error().category == ErrorCategory::Validation);

  const auto panic_scoped = controller.submit({KillType::Panic, "x"}, "op-secret");
  REQUIRE_FALSE(panic_scoped.has_value());
  CHECK(panic_scoped.error().category == ErrorCategory::Validation);

  // Every rejection is fail-closed: nothing durable, nothing queued.
  CHECK(store.saved().empty());
  CHECK(controller.drain().empty());

  // A WELL-FORMED Strategy kill (non-empty scope) still succeeds end to end.
  const auto good = controller.submit({KillType::Strategy, "alpha"}, "op-secret");
  REQUIRE(good.has_value());
  REQUIRE(store.saved().size() == 1);
  CHECK(store.saved().front() == KillCommand{KillType::Strategy, "alpha"});

  const std::vector<KillCommand> batch = controller.drain();
  REQUIRE(batch.size() == 1);
  CHECK(batch.front() == KillCommand{KillType::Strategy, "alpha"});
}

TEST_CASE("to_string(KillType) is stable for all five kill types (NFR-8)", "[modes][killswitch]") {
  using broker_exec::modes::to_string;
  CHECK(to_string(KillType::Soft) == "Soft");
  CHECK(to_string(KillType::Strategy) == "Strategy");
  CHECK(to_string(KillType::Broker) == "Broker");
  CHECK(to_string(KillType::Account) == "Account");
  CHECK(to_string(KillType::Panic) == "Panic");
}

TEST_CASE("KillState.posture() feeds the coordinator's operator_floor", "[modes][killswitch]") {
  const PostureCoordinator coord;

  // A panic kill forces Panic even with no detector signals active.
  KillState panic;
  panic.apply({KillType::Panic, ""});
  CHECK(coord.evaluate({}, panic.posture()) == Posture::Panic);

  // A soft kill forces SoftKill as the operator floor.
  KillState soft;
  soft.apply({KillType::Soft, ""});
  CHECK(coord.evaluate({}, soft.posture()) == Posture::SoftKill);

  // No kill leaves the floor Normal.
  const KillState none;
  CHECK(coord.evaluate({}, none.posture()) == Posture::Normal);
}
