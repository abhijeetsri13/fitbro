#include "broker_exec/modes/killswitch.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/ports_common.hpp"

namespace broker_exec::modes {

namespace {

// A kill is "broad" if it blocks every strategy regardless of scope. Only a
// Strategy kill is scope-specific; all other types stop the whole account/broker
// and therefore block any strategy.
[[nodiscard]] bool is_broad(KillType type) noexcept { return type != KillType::Strategy; }

// Reject a malformed/mis-scoped kill before it can take effect. Only a Strategy
// kill is per-target — it is meaningless without a scope (the strategy id its
// `blocks_strategy` filter keys on), so an empty-scope Strategy kill over-blocks
// entries while the per-strategy flag never trips: the last line of defense
// refuses it loudly. Soft and Panic are process-wide and must NOT carry a scope.
// Broker/Account are process-wide in the process-per-account model (the broker/
// account is implicit), so their scope is OPTIONAL — informational, not required.
[[nodiscard]] Result<ports::Ok> validate(const KillCommand& cmd) {
  if (cmd.type == KillType::Strategy && cmd.scope.empty()) {
    return fail(errors::make_error(errors::ErrorCategory::Validation,
                                   "kill rejected: Strategy kill requires a scope"));
  }
  if ((cmd.type == KillType::Soft || cmd.type == KillType::Panic) && !cmd.scope.empty()) {
    return fail(errors::make_error(
        errors::ErrorCategory::Validation,
        "kill rejected: " + std::string(to_string(cmd.type)) + " kill must not carry a scope"));
  }
  return ports::ok();
}

}  // namespace

std::string_view to_string(KillType type) noexcept {
  switch (type) {
    case KillType::Soft:
      return "Soft";
    case KillType::Strategy:
      return "Strategy";
    case KillType::Broker:
      return "Broker";
    case KillType::Account:
      return "Account";
    case KillType::Panic:
      return "Panic";
  }
  return "Unknown";
}

bool operator==(const KillCommand& lhs, const KillCommand& rhs) noexcept {
  return lhs.type == rhs.type && lhs.scope == rhs.scope;
}

bool operator!=(const KillCommand& lhs, const KillCommand& rhs) noexcept { return !(lhs == rhs); }

void KillState::apply(const KillCommand& cmd) {
  // Idempotent: an equal (type, scope) command already in the active set is a
  // no-op (re-applying a kill — e.g. on a replay or a re-submit — never changes
  // the effect).
  if (std::find(active_.begin(), active_.end(), cmd) == active_.end()) {
    active_.push_back(cmd);
  }
}

bool KillState::panic_active() const noexcept {
  return std::any_of(active_.begin(), active_.end(),
                     [](const KillCommand& c) noexcept { return c.type == KillType::Panic; });
}

bool KillState::any_active() const noexcept { return !active_.empty(); }

bool KillState::blocks_entries() const noexcept { return any_active(); }

bool KillState::blocks_strategy(std::string_view strategy) const {
  for (const KillCommand& c : active_) {
    // A broad kill blocks every strategy; a Strategy kill blocks only its scope.
    if (is_broad(c.type) || c.scope == strategy) {
      return true;
    }
  }
  return false;
}

bool KillState::allows_risk_reducing_exits() const noexcept { return !panic_active(); }

Posture KillState::posture() const noexcept {
  if (panic_active()) {
    return Posture::Panic;
  }
  return any_active() ? Posture::SoftKill : Posture::Normal;
}

KillController::KillController(std::function<bool(std::string_view)> authenticate,
                              std::function<Result<ports::Ok>(const KillCommand&)> persist)
    : authenticate_(std::move(authenticate)), persist_(std::move(persist)) {}

Result<ports::Ok> KillController::submit(const KillCommand& cmd, std::string_view auth_token) {
  // Order: validate -> authenticate -> persist -> enqueue (fail-closed at each
  // step). Called from the SINGLE control-plane thread; the queue mutex guards
  // only the submit-push / loop-drain handoff (see the header threading contract).

  // 1) Validate the command FIRST — before authenticate — so a malformed or
  //    mis-scoped kill is rejected loudly even when the token is bad. Fail-closed:
  //    NOTHING persisted, NOTHING enqueued.
  if (auto valid = validate(cmd); !valid) {
    return fail(std::move(valid.error()));
  }

  // 2) Authenticate. An unauthenticated control command is rejected with NOTHING
  //    persisted and NOTHING enqueued — it never takes effect.
  if (!authenticate_(auth_token)) {
    return fail(errors::make_error(errors::ErrorCategory::Auth,
                                   "kill rejected: unauthenticated control command"));
  }

  // 3) Persist BEFORE ack (AC-3 durability). Fail-closed: if we cannot make the
  //    kill durable, refuse to ack and do NOT enqueue. Surface the seam's own
  //    Error (preserving its category/message — redaction-safe per the Error
  //    contract), matching replay()'s passthrough.
  if (auto persisted = persist_(cmd); !persisted) {
    return fail(std::move(persisted.error()));
  }

  // 4) Enqueue onto the thread-safe handoff queue for the main loop to drain.
  {
    const std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.push_back(cmd);
  }
  return ports::ok();
}

std::vector<KillCommand> KillController::drain() {
  const std::lock_guard<std::mutex> lock(queue_mutex_);
  std::vector<KillCommand> batch = std::move(queue_);
  queue_.clear();  // `move` leaves it valid-but-unspecified; clear to a known state.
  return batch;
}

Result<ports::Ok> KillController::replay(
    KillState& state, const std::function<Result<std::vector<KillCommand>>()>& load) {
  auto loaded = load();
  if (!loaded) {
    return fail(loaded.error());
  }
  for (const KillCommand& c : loaded.value()) {
    state.apply(c);  // idempotent; re-applies the persisted kills (still killed).
  }
  return ports::ok();
}

}  // namespace broker_exec::modes
