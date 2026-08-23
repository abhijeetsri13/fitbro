#include "broker_exec/feedsub/resubscribe_guard.hpp"

#include <algorithm>
#include <utility>

namespace broker_exec::feedsub {

std::string_view to_string(TokenFeedState state) noexcept {
  switch (state) {
    case TokenFeedState::Subscribed:
      return "subscribed";
    case TokenFeedState::AwaitingFirstTick:
      return "awaiting_first_tick";
    case TokenFeedState::Live:
      return "live";
    case TokenFeedState::MuteAfterResubscribe:
      return "mute_after_resubscribe";
  }
  return "unknown";
}

ResubscribeGuard::ResubscribeGuard(const ports::ClockPort& clock,
                                   std::chrono::milliseconds first_tick_deadline)
    : clock_(clock),
      // Fail-closed clamp: a non-positive deadline would disable the mute check
      // (already-overdue or never-fire). Pull it up to the positive floor so the
      // connected-but-mute detection stays armed no matter what is configured.
      first_tick_deadline_(std::max(first_tick_deadline, kMinDeadline)) {}

TokenFeedState ResubscribeGuard::resolve(const Entry& entry) const {
  // The ONLY place the lazy mute transition is computed. An AwaitingFirstTick
  // token whose deadline has STRICTLY passed (now_steady() > deadline) is
  // connected-but-mute; at exactly the deadline it is still awaiting (strict `>`).
  if (entry.phase == TokenFeedState::AwaitingFirstTick &&
      clock_.now_steady() > entry.first_tick_deadline) {
    return TokenFeedState::MuteAfterResubscribe;
  }
  return entry.phase;
}

void ResubscribeGuard::subscribe(const std::string& token) {
  // Idempotent: only INSERT when absent (map::emplace does not overwrite), so a
  // token already in the set keeps its current phase/deadline. A new token starts
  // Subscribed — armed only by the next on_reconnect(), never tradable until then.
  tokens_.emplace(token, Entry{});
}

void ResubscribeGuard::unsubscribe(const std::string& token) {
  tokens_.erase(token);
}

std::vector<std::string> ResubscribeGuard::on_reconnect() {
  // Re-issue the FULL desired set: the caller must send a subscribe frame for
  // every token here (never trust the socket to remember across a 1006 drop).
  // Arm verification at the same instant: each token -> AwaitingFirstTick with a
  // fresh deadline, so a previously-Live token must re-prove it ticks.
  const std::chrono::steady_clock::time_point deadline = clock_.now_steady() + first_tick_deadline_;

  std::vector<std::string> resubscribe;
  resubscribe.reserve(tokens_.size());
  for (auto& [token, entry] : tokens_) {  // std::map -> deterministic (sorted) order
    entry.phase = TokenFeedState::AwaitingFirstTick;
    entry.first_tick_deadline = deadline;
    resubscribe.push_back(token);
  }
  return resubscribe;
}

void ResubscribeGuard::record_tick(const std::string& token) {
  // A tick proves the resubscribe delivered -> Live. Ignore a tick for an
  // unknown/unsubscribed token: it must NOT create an entry or any state.
  const auto it = tokens_.find(token);
  if (it == tokens_.end()) {
    return;
  }
  it->second.phase = TokenFeedState::Live;
}

TokenFeedState ResubscribeGuard::state_of(const std::string& token) const {
  const auto it = tokens_.find(token);
  if (it == tokens_.end()) {
    // Unknown token: the inert, not-tradable, not-mute baseline.
    return TokenFeedState::Subscribed;
  }
  return resolve(it->second);
}

std::vector<std::string> ResubscribeGuard::mute_tokens() const {
  std::vector<std::string> mute;
  for (const auto& [token, entry] : tokens_) {  // deterministic (sorted) order
    if (resolve(entry) == TokenFeedState::MuteAfterResubscribe) {
      mute.push_back(token);
    }
  }
  return mute;
}

bool ResubscribeGuard::is_tradable(const std::string& token) const {
  // Fail-closed: ONLY a post-resubscribe Live token trades. Awaiting / mute /
  // never-armed Subscribed / unknown are all non-tradable.
  return state_of(token) == TokenFeedState::Live;
}

}  // namespace broker_exec::feedsub
