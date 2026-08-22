#pragma once

// broker_exec::composition — THE COMPOSITION ROOT (Story 6.3, SM-3, FR-1/FR-2,
// IA-1/IA-2, CAP-2).
//
// WHAT THIS IS: the one module that is allowed to know which brokers exist. It
// turns a CONFIG VALUE (`broker.name = "kite" | "kotak"`) into an assembled
// `ports::BrokerPort` plus that broker's `capabilities::CapabilitySet`, and it
// refuses to hand back anything at all unless the strategy's declared
// capabilities are satisfied FIRST.
//
// WHY IT MAY LINK ADAPTERS. The hexagonal rule enforced at configure time
// (`enforce_no_dependency` in cmake/HexagonalBoundary.cmake) is that `domain` and
// `ports` must never link `adapters`. A composition root is the deliberate
// exception in the other direction: it is the OUTERMOST layer, it links every
// adapter, and it exists precisely so nothing else has to. A strategy depends on
// `ports::BrokerPort` and on this module's *result* — never on an adapter type.
//
// ── THE PORTABILITY CLAIM THIS MODULE MAKES (SM-3) ──────────────────────────
// An unchanged strategy written against `ports::BrokerPort` runs against a
// different broker by flipping ONE string in configuration. There is no
// per-broker branch in strategy code, no broker-specific type in a strategy
// signature, and no broker identifier in a strategy translation unit. Proven by
// tests/portability/ — the same strategy function, the same outcome value, two
// brokers.
//
// ── LOAD-TIME REJECTION IS THE POINT (AC-2, CAP-2 reject-only) ──────────────
// `make_broker()` runs `CapabilitySet::require_all()` over the strategy's
// declared capabilities BEFORE it constructs anything that can reach a broker.
// A missing capability therefore fails at WIRING time with a typed
// `NotSupported`/`DoNotRetry` error that names the capability — never halfway
// through a trade with a live position on. Remember the tri-state's fail-closed
// default: `Support::Unknown` reads as UNSUPPORTED at the gate, so an
// uncertified broker (today: all of Kotak Neo, pending the tier-2 live smoke)
// is rejected here rather than optimistically admitted.
//
// ── FAIL-CLOSED PARSING ─────────────────────────────────────────────────────
// The broker name is matched EXACTLY and CASE-SENSITIVELY against the two
// spellings below. "Kite", "KOTAK", " kite", "zerodha" and "" are all typed
// `Validation`/`DoNotRetry` errors. The offending value IS named in the message
// (an operator debugging a config typo needs to see it) but only after
// `domain::scrub()` and a length cap, because a config field is exactly the sort
// of place an operator pastes a token by mistake.
//
// ── CONFIG: EXTENDED, NOT FORKED ────────────────────────────────────────────
// This reads the existing `config::Config` from Story 2.1 (`broker.name`). The
// string -> enum mapping lives HERE, not in `config`, so the config module never
// has to know the adapter roster and adding a third broker touches exactly one
// module.
//
// NO-THROW: every fallible entry point returns `Result<T>`. No float, no OS API,
// no `#ifdef`. C++20 standard library only.

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "broker_exec/adapters/kite/http_client.hpp"
#include "broker_exec/adapters/kotak/kotak_rest_client.hpp"
#include "broker_exec/adapters/kotak/kotak_transport.hpp"
#include "broker_exec/capabilities/capabilities.hpp"
#include "broker_exec/config/config.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/ports/secret_provider.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::composition {

// The brokers this build can compose. Deliberately a CLOSED enum: an unknown
// config value is a typed rejection, never a "default broker".
enum class BrokerChoice { Kite, Kotak };

// The canonical config spelling of a choice ("kite" / "kotak"). This string is
// part of the configuration + observability contract: renaming it is a breaking
// change to every deployed TOML/env override.
[[nodiscard]] std::string_view to_string(BrokerChoice choice) noexcept;

// Parse `broker.name`. EXACT, case-sensitive match against `to_string()`.
// Anything else — including a case variant, surrounding whitespace, or an empty
// value — is a `Validation`/`DoNotRetry` error naming the (scrubbed, length-capped)
// value. Fail-closed: there is no fallback broker.
[[nodiscard]] Result<BrokerChoice> parse_broker_choice(std::string_view name);

// ── Injected dependencies ───────────────────────────────────────────────────
//
// Everything transport-shaped the adapters need, in ONE struct that BOTH brokers
// can be built from. That is what keeps the factory transport-free and testable:
// production wires a cpr/libcurl `HttpClient` and a real `SecretProvider`; a test
// wires an in-memory recorded server and a synthetic session bundle, and NOTHING
// in this module changes.
//
// LIFETIME: every pointer/reference here is BORROWED. The adapters hold the
// transport by reference, so each non-null dependency MUST outlive the
// `BrokerAssembly` built from it. (The session provider is a `std::function`
// captured BY VALUE into the REST client, so it is the one member that may own
// its state.)
//
// Only the fields for the CHOSEN broker are validated; the other broker's fields
// may be left empty. Populating both is normal and is exactly what makes the
// config flip a one-string change.
struct BrokerDeps {
  // ── Kite ──
  // The HTTP transport seam (`adapters::kite::HttpClient`) pointed at the Kite
  // base URL, and the secret store holding the api_key + daily access_token.
  const adapters::kite::HttpClient* kite_http = nullptr;
  const ports::SecretProvider* kite_secrets = nullptr;
  // Logical secret NAMES (not values). The defaults are the Story 2.3 contract.
  std::string kite_api_key_secret = "kite.api_key";
  std::string kite_access_token_secret = "kite.access_token";

  // ── Kotak Neo ──
  // A separate transport instance: the same seam type, a different base URL. Kept
  // as its own field so a caller cannot accidentally point one broker's adapter
  // at the other broker's endpoint.
  const adapters::kotak::HttpClient* kotak_http = nullptr;
  // Yields the CURRENT session bundle, invoked per request (normally
  // `[&est]{ return est.load(); }` straight off the encrypted TokenStore). The
  // factory never sees a credential — only this callable.
  adapters::kotak::BundleProvider kotak_session;
};

// ═════════════════════════════════════════════════════════════════════════════
// ██ FIXTURE_CERTIFICATION — TEST-ONLY. NEVER CONSTRUCT THIS IN PRODUCTION. ██
// ═════════════════════════════════════════════════════════════════════════════
//
// A capability profile ASSERTED BY A TEST FIXTURE rather than certified by a
// broker. It replaces the real profile for the lifetime of one assembly.
//
// It exists for ONE reason: the portability proof (AC-1) has to run the same
// strategy against a broker whose every capability is still `Unknown` by design
// (Kotak Neo stays uncertified until the operator-run live min-qty smoke in
// docs/kotak-min-qty-smoke.md — architecture TO-6), and `Unknown` is fail-closed
// at the gate.
//
// WHY IT IS A NAMED TYPE AND NOT A BARE `CapabilitySet`: a bare set is
// indistinguishable from a legitimate one at a call site and invisible to review.
// This spelling is GREPPABLE — `FixtureCertification` appearing anywhere outside
// a test is, by itself, the bug report. The constructor is `explicit` so it can
// never be conjured by an implicit conversion.
//
// WHAT KEEPS IT HONEST, BEYOND THE NAME:
//   * `BrokerAssembly::capability_override_in_effect()` reports it;
//   * `assert_production_posture()` turns that into a startup failure;
//   * `require_capabilities()` REFUSES to answer from it at all;
//   * the AC-2 tests prove the default path still rejects the same capability on
//     the same broker.
// It promotes nothing in any adapter's real profile. Only the live smoke may.
struct FixtureCertification {
  explicit FixtureCertification(capabilities::CapabilitySet profile) noexcept : asserted(profile) {}

  capabilities::CapabilitySet asserted;
};

// ── Composition options ─────────────────────────────────────────────────────
struct BrokerOptions {
  // The capabilities the component declares AS DATA. Checked with
  // `CapabilitySet::require_all()` BEFORE any adapter is constructed, so an
  // unsupported (or merely uncertified) capability is rejected at load — the
  // whole point of AC-2.
  //
  // LEAVING THIS EMPTY IS AN ERROR BY DEFAULT. `require_all({})` is vacuously
  // `ok()`, so an empty list would sail through the gate and hand back a port
  // that can place live orders on a broker where every capability is `Unknown`.
  // Silence is not a declaration. To genuinely need nothing, say so explicitly
  // with `declares_no_capabilities` below.
  std::vector<capabilities::Capability> required_capabilities;

  // The escape hatch for a component that TRULY performs no broker mutation: a
  // read-only operator tool, a reconciliation report, a health probe. It buys
  // exactly one thing — permission to declare nothing — and it is deliberately
  // wordy so that setting it on an order-placing component looks as wrong as it
  // is. It does NOT relax any capability check; it only distinguishes "I need
  // nothing" from "I forgot to say".
  //
  // NOTE WHAT IT DOES NOT DO: it does not RESTRICT the returned port either. The
  // assembly still carries the broker's real profile, so on a broker that
  // certifies PlaceOrder the port can still place. This field is a declaration,
  // not a sandbox — enforcing least privilege per component is a separate
  // concern and would need the port itself to be narrowed.
  bool declares_no_capabilities = false;

  // TEST-ONLY. See FixtureCertification above.
  std::optional<FixtureCertification> capability_override;
};

class BrokerAssembly;

// Compose the chosen broker. In order:
//   0. reject an EMPTY, unflagged capability declaration (silence is not a
//      declaration — see BrokerOptions::required_capabilities);
//   1. resolve the capability profile (real, or the fixture certification);
//   2. run `require_all(options.required_capabilities)` — REJECT HERE (AC-2);
//   3. validate the injected dependencies for THIS choice;
//   4. only then construct the transport + adapter stack, handing each adapter
//      the ADMITTED profile so every mutation is re-gated per call.
// Steps 0-3 happen before ANYTHING capable of reaching a broker exists, which is
// what makes "rejected at load, not mid-trade" a structural property rather than
// a convention.
//
// `options` IS MANDATORY — there is no defaulted argument. A defaulted
// `BrokerOptions{}` is precisely the ungated composition step 0 exists to
// prevent, and a default argument would make it the path of least resistance.
//
// Errors: `Validation`/`DoNotRetry` for an undeclared capability list (step 0)
// or a missing dependency (step 3); `NotSupported`/`DoNotRetry` naming the
// capability (step 2).
[[nodiscard]] Result<BrokerAssembly> make_broker(BrokerChoice choice, const BrokerDeps& deps,
                                                 const BrokerOptions& options);

// A composed, ready-to-use broker: the adapter stack behind a single
// `ports::BrokerPort`, plus the capability profile it was admitted under.
//
// OWNERSHIP: the assembly owns the WHOLE stack (REST client + adapter), not just
// the outermost object — the adapter holds its REST client by reference and the
// REST client holds the transport by reference, so the intermediate layer has to
// be owned somewhere or it dangles. Move-only, and it can only be produced by
// `make_broker()` (private constructor): there is no way to obtain one that
// skipped the capability gate.
class BrokerAssembly {
 public:
  BrokerAssembly(BrokerAssembly&&) noexcept = default;
  BrokerAssembly& operator=(BrokerAssembly&&) noexcept = default;
  BrokerAssembly(const BrokerAssembly&) = delete;
  BrokerAssembly& operator=(const BrokerAssembly&) = delete;

  // The broker-neutral seam a strategy is written against. Stable for the
  // lifetime of this assembly.
  [[nodiscard]] ports::BrokerPort& broker() const noexcept { return *broker_; }

  // Which broker was composed (diagnostics/logging; a strategy must not branch
  // on it — that is exactly the coupling this module exists to prevent).
  [[nodiscard]] BrokerChoice choice() const noexcept { return choice_; }

  // The profile this assembly was admitted under. A later gate (validation,
  // safe-start) consults the SAME set the load-time check used.
  //
  // NAMED `capability_set()`, NOT `capabilities()`, ON PURPOSE: a member named
  // `capabilities` would sit in the same class scope that has to resolve
  // `capabilities::CapabilitySet` as a nested-name-specifier. The standard says
  // qualified lookup ignores the function, but this is exactly the corner where
  // toolchains diverge, and a portability library should not bet a build on it.
  [[nodiscard]] const capabilities::CapabilitySet& capability_set() const noexcept {
    return capabilities_;
  }

  // True when `BrokerOptions::capability_override` supplied the profile above,
  // i.e. the capabilities are a TEST FIXTURE POSTURE and not the broker's real,
  // certified profile. A production composition root should treat `true` here as
  // a hard startup failure.
  [[nodiscard]] bool capability_override_in_effect() const noexcept { return override_in_effect_; }

 private:
  // Parameter names deliberately avoid the member/accessor spellings so no
  // compiler's shadow diagnostic (-Wshadow / MSVC C4458) has anything to say.
  BrokerAssembly(BrokerChoice chosen, std::unique_ptr<ports::BrokerPort> port,
                 capabilities::CapabilitySet profile, bool overridden) noexcept
      : choice_(chosen),
        broker_(std::move(port)),
        capabilities_(profile),
        override_in_effect_(overridden) {}

  friend Result<BrokerAssembly> make_broker(BrokerChoice, const BrokerDeps&, const BrokerOptions&);

  BrokerChoice choice_ = BrokerChoice::Kite;
  std::unique_ptr<ports::BrokerPort> broker_;
  capabilities::CapabilitySet capabilities_;
  bool override_in_effect_ = false;
};

// The config-driven entry point — THE config flip itself. Reads
// `config.broker.name`, parses it fail-closed, and delegates to the overload
// above. This is the only line that has to change to move a strategy between
// brokers, and it is not in strategy code. `options` is mandatory here for the
// same reason it is above.
[[nodiscard]] Result<BrokerAssembly> make_broker(const config::Config& config,
                                                 const BrokerDeps& deps,
                                                 const BrokerOptions& options);

// Re-check a capability list against an already-composed assembly.
//
// `make_broker()` already gates on `BrokerOptions::required_capabilities`, so
// this is for the SECOND consumer of the same rule: a component loaded AFTER the
// broker (a protective-stop supervisor, a basket-margin pre-check) declaring its
// own requirements against the shared assembly. Same reject-only posture, same
// typed `NotSupported` error naming the first missing capability. Call it before
// any trading use — never mid-trade.
//
// IT REFUSES TO ANSWER FROM A FIXTURE CERTIFICATION. If the assembly was
// composed with `BrokerOptions::capability_override`, this returns a typed
// `Validation` error instead of a capability verdict. The reason is that the
// later component never agreed to that fixture posture: it is asking "may I
// rely on this broker for X", and the only truthful answers are the certified
// profile or "I cannot tell you". Quietly echoing a test's assertion back to a
// production component is how a fixture escapes its test.
[[nodiscard]] Result<ports::Ok> require_capabilities(
    const BrokerAssembly& assembly, std::span<const capabilities::Capability> required);

// A startup guard for a PRODUCTION composition root: ok() only if this assembly
// carries a broker's real, certified capability profile. Fails with a typed
// `Validation`/`DoNotRetry` error when a `FixtureCertification` is in effect.
//
// Call it immediately after `make_broker()` in any binary that can reach a live
// broker. It is the mechanical backstop behind the naming convention: even if a
// `FixtureCertification` survives review, the process refuses to start.
[[nodiscard]] Result<ports::Ok> assert_production_posture(const BrokerAssembly& assembly);

}  // namespace broker_exec::composition
