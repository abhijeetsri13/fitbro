#pragma once

// broker_exec::session::SessionState — the normalized, broker-neutral result of
// a login/health probe (Story 2.4, AC-2/IBR-6).
//
// Establishment and expiry detection collapse every broker-specific session
// outcome into this fixed vocabulary so the safe-start gate (Story 2.13) and the
// alerting path (Epic 4) switch on a stable enum, never on raw broker text:
//
//   Healthy     — a live, usable session (the daily token authenticates).
//   NeedsReauth — the session is dead/expired; the operator must re-establish
//                 (Kite has NO headless refresh). NOT an error to retry silently.
//   Failed      — establishment itself failed for a non-auth reason.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <string_view>

namespace broker_exec::session {

enum class SessionState { Healthy, NeedsReauth, Failed };

// Stable, log/serialization-friendly names (observability contract). Renaming a
// returned string is a breaking change.
[[nodiscard]] constexpr std::string_view to_string(SessionState state) noexcept {
  switch (state) {
    case SessionState::Healthy:
      return "Healthy";
    case SessionState::NeedsReauth:
      return "NeedsReauth";
    case SessionState::Failed:
      return "Failed";
  }
  return "Unknown";
}

}  // namespace broker_exec::session
