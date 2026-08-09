#pragma once

// broker_exec::ports::AlertSink — the abstract operator-notification seam.
//
// The dead-man's-switch / escalation path (the `RaiseAlert` SuggestedAction)
// surfaces here. The core emits a level + message; a concrete sink (Telegram,
// email, webhook, ...) delivers it. `send_test_alert()` exists so startup/health
// checks can prove the channel works end-to-end before relying on it. Abstract
// only this story; concrete sinks land later.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <string>

#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::ports {

// Severity of an operator alert. Stable, log-friendly ordering low->high.
enum class AlertLevel { Info, Warning, Error, Critical };

// TYPED PROVENANCE for an alert (IMP-16, FR-27/FR-29).
//
// THE DEFECT THIS EXISTS FOR: a sink scrubs the whole FREE-FORM `message`
// (MultiChannelAlertSink runs domain::scrub over it), and a minted client_ref is
// one long token-shaped run — so a caller that interpolated `ref=<client_ref>`
// into the body shipped `ref=***REDACTED***` to the operator. The most urgent
// alert in the system NAMED NO ORDER. The IMP-15 provenance exemption could not
// be reused directly: it is a WHOLE-TYPED-COLUMN allowlist, and a substring
// exemption inside a free-form body would disable scrub()'s bare high-entropy
// rule for every alert ever sent.
//
// So the ids travel BESIDE the body, in typed columns, and the sink renders them
// through domain::scrub_provenance_column AFTER scrubbing the body — the body's
// redaction is untouched, and a value that is not id-shaped is still redacted.
// PASS IDS HERE, NEVER IN `message`.
struct AlertContext {
  std::string client_ref;       // our idempotency key (the store/intent-log join key)
  std::string broker_order_id;  // the broker-minted order id
  std::string strategy;         // the owning strategy name

  // THE INSTRUMENT. Same defect as the ids, different shape, and it was NOT
  // covered by the first cut of IMP-16: callers interpolated `symbol` into the
  // free-form body, where scrub()'s bare high-entropy rule redacts any >=20-char
  // run mixing letters and digits. NIFTY24JUN24000CE (17) survived by luck of
  // length; FINNIFTY24JUN23000CE (20), BANKNIFTY24JUN52000CE (21) and
  // MIDCPNIFTY24JUN12000CE (22) were DESTROYED — i.e. the alert saying a position
  // is unprotected named no instrument for exactly the contracts this library
  // trades. It rides here instead and is rendered through
  // domain::is_instrument_symbol_shape, which is a TIGHTER allowlist than the id
  // rule (uppercase alnum only), never a looser one. PASS SYMBOLS HERE, NEVER IN
  // `message`.
  std::string symbol;
};

// Abstract alert delivery channel. Header-only, pure-virtual.
class AlertSink {
 public:
  virtual ~AlertSink() = default;

  // Deliver an alert. `message` MUST be redaction-safe (no secrets/tokens),
  // consistent with the Error logging policy.
  [[nodiscard]] virtual Result<Ok> send(AlertLevel level, const std::string& message) = 0;

  // Deliver an alert carrying TYPED PROVENANCE (IMP-16). `message` is the same
  // free-form, redaction-safe body as send(); `provenance` carries the ids, which
  // the sink renders through the whole-column allowlist so they survive to the
  // operator.
  //
  // ADDITIVE BY DESIGN — the default implementation IGNORES `provenance` and
  // delegates to send(), so every pre-existing implementation (one production
  // sink, ~40 test stubs) keeps compiling and behaving EXACTLY as it did. This is
  // deliberately not a RENDERING default: rendering needs
  // domain::scrub_provenance_column, and `ports` is a pure abstract seam that must
  // not carry redaction POLICY — the policy lives in the sink that owns the
  // outbound bytes. A sink that does not override this therefore DROPS the
  // provenance rather than leaking it, which is the fail-closed direction.
  //
  // WHY A NEW NAME RATHER THAN AN OVERLOAD OF `send` — this is load-bearing, do
  // not "tidy" it into `send(level, message, provenance)`. The project builds
  // with `-Woverloaded-virtual -Werror` on gcc/clang (cmake/CompilerWarnings.cmake).
  // A second VIRTUAL named `send` would be reported as "hidden" in every derived
  // class that declares only the 2-argument override — i.e. in all ~40 existing
  // stubs — turning an additive change into a repo-wide build failure on Linux and
  // macOS. A distinct name hides nothing, needs no `using AlertSink::send;` in any
  // subclass, and leaves both entry points callable on a concrete type.
  [[nodiscard]] virtual Result<Ok> send_with_context(AlertLevel level, const std::string& message,
                                                     const AlertContext& provenance) {
    static_cast<void>(provenance);
    return send(level, message);
  }

  // Send a fixed self-test alert to prove the channel is wired and reachable.
  // Used by the startup health check and the operator "test alert" command.
  [[nodiscard]] virtual Result<Ok> send_test_alert() = 0;
};

}  // namespace broker_exec::ports
