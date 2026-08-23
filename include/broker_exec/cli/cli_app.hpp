#pragma once

// broker_exec::cli operator CLI — a thin shell over the public API (Story 4.6,
// AC-1, FR-36).
//
// The CLI owns NO business logic. Every verb is a shell over an INJECTED seam
// (`OperatorApi`): the composition root wires each `std::function` to the real
// module method (tier-2), and this layer only parses argv, dispatches, scrubs and
// prints. This mirrors the established injected-seam pattern (alerting POST seam,
// marketdata tick seam, kite HttpClient seam).
//
// READ-ONLY EXCEPT `kill` (AC-1): of the seven verbs only `kill` may mutate. The
// read-only verbs must be wired to read-only API methods — `is_mutating(Verb)`
// makes the one mutating verb explicit, and `dispatch` itself NEVER mutates: it
// only forwards to the seam the caller supplied. A null seam callback FAILS CLOSED
// (a typed Error, never a crash, never UB) — a missing wire must not silently no-op.
//
// Cross-platform: CLI11 is confined to cli_app.cpp (its headers never enter this
// public header). No-throw across the boundary: every verb returns `Result<std::
// string>`; `run_cli` returns a process exit code. No float. C++20 standard library.

#include <functional>
#include <string>
#include <string_view>

#include "broker_exec/result.hpp"

namespace broker_exec::cli {

// The injected public-API seam — one callback per operator verb. Each returns a
// `Result<std::string>`: a human/audit line on success (which the CLI scrubs before
// printing) or a typed Error. The composition root supplies these; a null callback
// is treated as "not wired" and dispatch fails closed. Read-only callbacks MUST be
// wired to read-only API methods — only `kill` may reach a mutating path.
struct OperatorApi {
  std::function<Result<std::string>()> status;             // read-only: engine status
  std::function<Result<std::string>()> reconcile;          // read-only: reconcile report
  std::function<Result<std::string>()> replay_intent_log;  // read-only: replay verification
  std::function<Result<std::string>()> safe_start_check;   // read-only: safe-start gate
  std::function<Result<std::string>()> send_test_alert;    // read-only: emit a test alert
  std::function<Result<std::string>()> verify_ip;          // read-only: egress-IP check
  std::function<Result<std::string>()> kill;               // MUTATING: kill switch
};

// The operator verbs. Exactly one (`Kill`) is mutating; the rest are read-only.
enum class Verb {
  Status,
  Reconcile,
  ReplayIntentLog,
  SafeStartCheck,
  SendTestAlert,
  VerifyIp,
  Kill
};

// Stable, log/serialization-friendly verb name (the subcommand spelling). Renaming
// a returned name is a breaking operator-contract change.
[[nodiscard]] std::string_view to_string(Verb verb) noexcept;

// True ONLY for `Kill` — the single mutating verb (AC-1). Every other verb is
// read-only and must never reach a mutating path. This is the explicit guard.
[[nodiscard]] bool is_mutating(Verb verb) noexcept;

// Route a verb to its seam callback and return the (caller-scrubbed) result line.
// A null callback for the requested verb FAILS CLOSED with a typed Error — no
// crash, no UB. `dispatch` itself performs no module work and never mutates; it
// only forwards to the supplied seam.
[[nodiscard]] Result<std::string> dispatch(Verb verb, const OperatorApi& api);

// Build the CLI11 app (one subcommand per verb), parse `argv`, dispatch the selected
// verb through `api`, print the SCRUBBED result line (or error), and return a process
// exit code (0 on success, non-zero on error/parse failure). CLI11 is confined to
// this function's translation unit.
[[nodiscard]] int run_cli(int argc, char** argv, const OperatorApi& api);

}  // namespace broker_exec::cli
