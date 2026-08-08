#include "broker_exec/supervisor/supervisor_plan.hpp"

#include <utility>

namespace broker_exec::supervisor {

SupervisorPlan::SupervisorPlan(BackoffConfig backoff) : backoff_(backoff) {}

bool SupervisorPlan::register_account(const std::string& account_id) {
  if (account_id.empty()) {
    return false;  // an unnamed account never gets a slot
  }
  accounts_.try_emplace(account_id);
  return true;
}

SupervisorDecision SupervisorPlan::on_exit(const std::string& account_id, int exit_code,
                                           std::chrono::steady_clock::time_point now) {
  if (account_id.empty()) {
    // FAIL CLOSED. We cannot attribute this exit to an account, so we cannot
    // reason about its crash history — escalate rather than guess, and do not
    // create a phantom registry entry.
    return SupervisorDecision{SupervisorAction::NoRestartEscalate, 0, true,
                              "empty account id - cannot attribute exit, escalated"};
  }

  // An exit from an account we were not told about is still a real exit: adopt it
  // rather than silently dropping the decision.
  AccountSupervision& account = accounts_[account_id];

  const ExitReason reason = exit_reason_from_code(exit_code);
  account.last_exit_code = exit_code;
  account.last_reason = reason;
  account.down = true;
  account.restart_due.reset();

  // ── The per-account counter. NOTHING here reads another account's entry —
  // that is the isolation guarantee, and it is structural, not a convention.
  switch (reason) {
    case ExitReason::Crash:
      ++account.consecutive_crashes;
      break;
    case ExitReason::CleanShutdown:
      account.consecutive_crashes = 0;  // an intended stop ends the crash streak
      break;
    case ExitReason::FailClosedNeedsHuman:
      // Leave the streak alone: it is evidence for the operator, and the decision
      // is terminal regardless of its value.
      break;
  }

  const SupervisorDecision decision = decide(reason, account.consecutive_crashes, backoff_);

  if (decision.raise_absence_alarm) {
    account.absence_alarm = true;
  }
  if (decision.action == SupervisorAction::NoRestartEscalate) {
    account.escalated = true;
  }
  if (decision.action == SupervisorAction::RestartWithBackoff) {
    // Injected time: the restart instant is derived from the caller's monotonic
    // `now`, never from an ambient clock.
    account.restart_due = now + std::chrono::seconds{decision.backoff_seconds};
  }

  return decision;
}

void SupervisorPlan::note_started(const std::string& account_id,
                                  std::chrono::steady_clock::time_point /*now*/) {
  if (account_id.empty()) {
    return;
  }
  AccountSupervision& account = accounts_[account_id];
  account.down = false;
  account.restart_due.reset();
}

void SupervisorPlan::note_healthy(const std::string& account_id) {
  const auto it = accounts_.find(account_id);
  if (it == accounts_.end()) {
    return;
  }
  AccountSupervision& account = it->second;
  account.consecutive_crashes = 0;
  account.absence_alarm = false;
  account.escalated = false;
}

void SupervisorPlan::clear_alarm(const std::string& account_id) {
  const auto it = accounts_.find(account_id);
  if (it == accounts_.end()) {
    return;
  }
  // Deliberately does NOT reset consecutive_crashes: acknowledging an alarm is
  // not the same as the account having proved it can run.
  it->second.absence_alarm = false;
  it->second.escalated = false;
}

bool SupervisorPlan::may_restart(const std::string& account_id,
                                 std::chrono::steady_clock::time_point now) const {
  const auto it = accounts_.find(account_id);
  if (it == accounts_.end()) {
    return false;  // unknown account: never restart something we cannot reason about
  }
  const AccountSupervision& account = it->second;
  if (!account.down || account.escalated) {
    return false;
  }
  if (!account.restart_due.has_value()) {
    return false;  // it exited but no restart was scheduled (clean stop / escalation)
  }
  return now >= *account.restart_due;
}

std::optional<AccountSupervision> SupervisorPlan::state_of(const std::string& account_id) const {
  const auto it = accounts_.find(account_id);
  if (it == accounts_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<std::string> SupervisorPlan::accounts() const {
  std::vector<std::string> ids;
  ids.reserve(accounts_.size());
  for (const auto& entry : accounts_) {
    ids.push_back(entry.first);
  }
  return ids;
}

PlanSummary SupervisorPlan::summary() const {
  PlanSummary summary;
  summary.accounts_total = static_cast<int>(accounts_.size());
  for (const auto& entry : accounts_) {
    const AccountSupervision& account = entry.second;
    if (account.down) {
      ++summary.accounts_down;
    }
    if (account.absence_alarm) {
      ++summary.accounts_alarmed;
    }
    if (account.escalated) {
      ++summary.accounts_escalated;
    }
  }
  summary.any_escalated = summary.accounts_escalated > 0;
  return summary;
}

}  // namespace broker_exec::supervisor
