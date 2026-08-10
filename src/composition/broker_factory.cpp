#include "broker_exec/composition/broker_factory.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "broker_exec/adapters/kite/kite_broker_adapter.hpp"
#include "broker_exec/adapters/kite/kite_rest_client.hpp"
#include "broker_exec/adapters/kotak/kotak_broker_adapter.hpp"
#include "broker_exec/adapters/kotak/kotak_capabilities.hpp"
#include "broker_exec/adapters/kotak/kotak_rest_client.hpp"
#include "broker_exec/capabilities/capabilities.hpp"
#include "broker_exec/errors/error.hpp"

namespace broker_exec::composition {

namespace {

// The exact, case-sensitive config spellings. Kept as constants so
// `to_string()` and `parse_broker_choice()` can never drift apart.
constexpr std::string_view kKiteName = "kite";
constexpr std::string_view kKotakName = "kotak";

// ── Echoing a bad config value back, safely ─────────────────────────────────
//
// An operator debugging a typo needs to SEE the value; a config field is also
// exactly where a credential gets pasted by mistake. The resolution is NOT
// "scrub then truncate" — truncating a scrubbed string can slice the redaction
// marker in half and turn a safety marker into noise, and scrub() only catches
// shapes it knows.
//
// Instead the echo is restricted to a class of value that CANNOT be a secret: at
// most 16 characters, ASCII letters only. No digits, no punctuation, no
// separators, nothing long. Every real broker name fits; every token, path, URL,
// JSON blob and pasted file does not, and is DESCRIBED rather than reproduced.
constexpr std::size_t kMaxEchoedValueChars = 16;

[[nodiscard]] bool is_ascii_alpha(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

[[nodiscard]] std::string safe_echo(std::string_view raw) {
  if (raw.empty()) {
    return "<empty>";
  }
  const bool echoable =
      raw.size() <= kMaxEchoedValueChars &&
      std::all_of(raw.begin(), raw.end(), [](char c) { return is_ascii_alpha(c); });
  if (!echoable) {
    // Say what it was, never what it said. The length is the one diagnostic that
    // is always safe and is usually enough ("oh — that is my token").
    return "<invalid broker name, " + std::to_string(raw.size()) + " chars>";
  }
  return std::string(raw);
}

// NOTE ON `broker_code`: it stays EMPTY on every error this module raises. Per
// errors/error.hpp it carries a raw code from a BROKER or transport, and nothing
// here has spoken to one — these are local wiring verdicts. Stuffing synthetic
// tokens ("BROKER_NAME", "MISSING_DEP") into it would corrupt the field's meaning
// for anything that aggregates broker codes. The discriminator lives in the
// message, which is where a local error's detail belongs.
[[nodiscard]] errors::Error validation_error(std::string message) {
  errors::Error error =
      errors::make_error(errors::ErrorCategory::Validation, std::move(message));
  // Validation already defaults to DoNotRetry; set it explicitly so the contract
  // survives a change to default_action_for().
  error.action = errors::SuggestedAction::DoNotRetry;
  return error;
}

// The broker's REAL, certified capability profile. Note what is NOT here: any
// notion of "close enough". Kotak's profile is entirely `Unknown` by design and
// stays that way until the operator-run live min-qty smoke promotes an entry
// (docs/kotak-min-qty-smoke.md, architecture TO-6) — the gate reads Unknown as
// unsupported, so composing a Kotak assembly that declares PlaceOrder is
// REJECTED here today. That rejection is the feature.
[[nodiscard]] capabilities::CapabilitySet real_capabilities(BrokerChoice choice) {
  switch (choice) {
    case BrokerChoice::Kite:
      return capabilities::kite_capabilities();
    case BrokerChoice::Kotak:
      return adapters::kotak::kotak_capabilities();
  }
  // Unreachable for a valid enumerator. Fail CLOSED rather than picking a
  // broker's profile: a default-constructed set is all-Unknown, so a future
  // enumerator that forgets its case is rejected at the gate instead of silently
  // inheriting someone else's certification.
  return capabilities::CapabilitySet{};
}

// ── Owning adapter stacks ───────────────────────────────────────────────────
//
// Each broker's stack is REST client + adapter, where the adapter holds the REST
// client BY REFERENCE. Something has to own that intermediate object for the
// lifetime of the port, and `unique_ptr<BrokerPort>` alone cannot. These wrappers
// are that owner: one object, owned by the assembly, forwarding the whole
// BrokerPort surface.
//
// MEMBER DECLARATION ORDER IS LOAD-BEARING (it is the construction order): the
// REST client must be constructed before the adapter that references it. The
// wrappers are non-copyable and non-movable for the same reason — a member
// holding a reference to a sibling member cannot survive a relocation.
//
// ── THEY ALSO RE-GATE EVERY MUTATION, PER CALL ──────────────────────────────
// Each wrapper carries the ADMITTED CapabilitySet and calls `require()` on the
// matching capability before place/modify/cancel/square_off reaches the adapter.
// Reads are not gated: they are idempotent, they are how a caller recovers from
// an ambiguous state, and refusing one could strand an operator who needs to see
// broker truth.
//
// IN A CORRECT PROGRAM THIS NEVER FIRES — the load-time gate already refused any
// assembly whose declared capabilities were unmet. It is DEFENCE IN DEPTH for the
// three ways that guarantee erodes: a component that under-declares what it
// actually calls, a future caller that reaches a port without going through
// `make_broker()`, and a capability list that drifts away from the code beside
// it. In every one of those the load gate is silent and this is the last thing
// standing between an uncertified broker and a live order. Cost is one array
// lookup per mutation.
//
// The refusal is the SAME typed `NotSupported`/`DoNotRetry` error the load gate
// raises, so a caller needs no new branch — and, crucially, it is raised BEFORE
// the adapter is touched, so nothing crosses the wire.

class OwnedKiteBroker final : public ports::BrokerPort {
 public:
  OwnedKiteBroker(const adapters::kite::HttpClient& http, const ports::SecretProvider& secrets,
                  std::string api_key_secret, std::string access_token_secret,
                  capabilities::CapabilitySet admitted)
      : rest_(http, secrets, std::move(api_key_secret), std::move(access_token_secret)),
        adapter_(rest_),
        admitted_(admitted) {}

  OwnedKiteBroker(const OwnedKiteBroker&) = delete;
  OwnedKiteBroker& operator=(const OwnedKiteBroker&) = delete;

  [[nodiscard]] Result<ports::BrokerAck> place(const domain::OrderIntent& intent) override {
    if (auto gate = admitted_.require(capabilities::Capability::PlaceOrder); !gate) {
      return fail(gate.error());
    }
    return adapter_.place(intent);
  }
  [[nodiscard]] Result<ports::BrokerAck> modify(const std::string& broker_order_id,
                                                const domain::OrderIntent& intent) override {
    if (auto gate = admitted_.require(capabilities::Capability::ModifyOrder); !gate) {
      return fail(gate.error());
    }
    return adapter_.modify(broker_order_id, intent);
  }
  [[nodiscard]] Result<ports::Ok> cancel(const std::string& broker_order_id) override {
    if (auto gate = admitted_.require(capabilities::Capability::CancelOrder); !gate) {
      return fail(gate.error());
    }
    return adapter_.cancel(broker_order_id);
  }
  [[nodiscard]] Result<ports::Ok> square_off(const std::string& broker_order_id) override {
    if (auto gate = admitted_.require(capabilities::Capability::SquareOff); !gate) {
      return fail(gate.error());
    }
    return adapter_.square_off(broker_order_id);
  }

  // ── Reads: never gated (see the header comment above) ──
  [[nodiscard]] Result<std::vector<domain::Order>> fetch_orders() override {
    return adapter_.fetch_orders();
  }
  [[nodiscard]] Result<std::vector<domain::Trade>> fetch_trades() override {
    return adapter_.fetch_trades();
  }
  [[nodiscard]] Result<std::vector<domain::Position>> fetch_positions() override {
    return adapter_.fetch_positions();
  }
  [[nodiscard]] Result<ports::FundsSnapshot> fetch_funds() override {
    return adapter_.fetch_funds();
  }

 private:
  adapters::kite::KiteRestClient rest_;      // must outlive adapter_
  adapters::kite::KiteBrokerAdapter adapter_;
  capabilities::CapabilitySet admitted_;
};

class OwnedKotakBroker final : public ports::BrokerPort {
 public:
  OwnedKotakBroker(const adapters::kotak::HttpClient& http,
                   adapters::kotak::BundleProvider session, capabilities::CapabilitySet admitted)
      : rest_(http, std::move(session)), adapter_(rest_), admitted_(admitted) {}

  OwnedKotakBroker(const OwnedKotakBroker&) = delete;
  OwnedKotakBroker& operator=(const OwnedKotakBroker&) = delete;

  [[nodiscard]] Result<ports::BrokerAck> place(const domain::OrderIntent& intent) override {
    if (auto gate = admitted_.require(capabilities::Capability::PlaceOrder); !gate) {
      return fail(gate.error());
    }
    return adapter_.place(intent);
  }
  [[nodiscard]] Result<ports::BrokerAck> modify(const std::string& broker_order_id,
                                                const domain::OrderIntent& intent) override {
    if (auto gate = admitted_.require(capabilities::Capability::ModifyOrder); !gate) {
      return fail(gate.error());
    }
    return adapter_.modify(broker_order_id, intent);
  }
  [[nodiscard]] Result<ports::Ok> cancel(const std::string& broker_order_id) override {
    if (auto gate = admitted_.require(capabilities::Capability::CancelOrder); !gate) {
      return fail(gate.error());
    }
    return adapter_.cancel(broker_order_id);
  }
  [[nodiscard]] Result<ports::Ok> square_off(const std::string& broker_order_id) override {
    if (auto gate = admitted_.require(capabilities::Capability::SquareOff); !gate) {
      return fail(gate.error());
    }
    return adapter_.square_off(broker_order_id);
  }

  // ── Reads: never gated (see the header comment above) ──
  [[nodiscard]] Result<std::vector<domain::Order>> fetch_orders() override {
    return adapter_.fetch_orders();
  }
  [[nodiscard]] Result<std::vector<domain::Trade>> fetch_trades() override {
    return adapter_.fetch_trades();
  }
  [[nodiscard]] Result<std::vector<domain::Position>> fetch_positions() override {
    return adapter_.fetch_positions();
  }
  [[nodiscard]] Result<ports::FundsSnapshot> fetch_funds() override {
    return adapter_.fetch_funds();
  }

 private:
  adapters::kotak::KotakRestClient rest_;      // must outlive adapter_
  adapters::kotak::KotakBrokerAdapter adapter_;
  capabilities::CapabilitySet admitted_;
};

// Dependency validation, per choice. Deliberately checked AFTER the capability
// gate and BEFORE construction: a caller that is missing both a capability and a
// transport hears about the capability first, because that is the one that says
// "this broker cannot do what you are asking", not "you wired it wrong".
[[nodiscard]] Result<ports::Ok> validate_deps(BrokerChoice choice, const BrokerDeps& deps) {
  switch (choice) {
    case BrokerChoice::Kite:
      if (deps.kite_http == nullptr) {
        return fail(validation_error(
            "composition: broker \"kite\" requires BrokerDeps::kite_http (HTTP transport)"));
      }
      if (deps.kite_secrets == nullptr) {
        return fail(validation_error(
            "composition: broker \"kite\" requires BrokerDeps::kite_secrets (SecretProvider)"));
      }
      if (deps.kite_api_key_secret.empty() || deps.kite_access_token_secret.empty()) {
        return fail(validation_error(
            "composition: broker \"kite\" requires non-empty api_key/access_token secret NAMES"));
      }
      return ports::ok();

    case BrokerChoice::Kotak:
      if (deps.kotak_http == nullptr) {
        return fail(validation_error(
            "composition: broker \"kotak\" requires BrokerDeps::kotak_http (HTTP transport)"));
      }
      if (!deps.kotak_session) {
        return fail(validation_error(
            "composition: broker \"kotak\" requires BrokerDeps::kotak_session (bundle provider)"));
      }
      return ports::ok();
  }
  return fail(validation_error("composition: unhandled broker choice"));
}

// STEP 0 — SILENCE IS NOT A DECLARATION.
//
// `CapabilitySet::require_all({})` is vacuously ok(), so an empty declaration
// would pass the gate and hand back a port that can place live orders on a
// broker where every capability is `Unknown`. The one construct in this module
// that could quietly produce a fully ungated broker is therefore an empty list,
// and it is rejected unless the caller says out loud that it needs nothing.
[[nodiscard]] Result<ports::Ok> validate_declaration(const BrokerOptions& options) {
  if (!options.required_capabilities.empty()) {
    return ports::ok();
  }
  if (options.declares_no_capabilities) {
    return ports::ok();
  }
  return fail(validation_error(
      "composition: BrokerOptions::required_capabilities is empty — declare what this component "
      "needs from the broker (an empty list would pass the capability gate vacuously and hand "
      "back an UNGATED port). If it genuinely performs no mutation, set "
      "BrokerOptions::declares_no_capabilities = true."));
}

}  // namespace

std::string_view to_string(BrokerChoice choice) noexcept {
  switch (choice) {
    case BrokerChoice::Kite:
      return kKiteName;
    case BrokerChoice::Kotak:
      return kKotakName;
  }
  return "unknown_broker";
}

Result<BrokerChoice> parse_broker_choice(std::string_view name) {
  if (name == kKiteName) {
    return BrokerChoice::Kite;
  }
  if (name == kKotakName) {
    return BrokerChoice::Kotak;
  }

  // EXACT match only. No trimming, no case folding, no "did you mean". A config
  // value we do not recognize means the operator believes they are trading
  // somewhere we are not; guessing is how a strategy ends up pointed at the
  // wrong account.
  std::string message = "composition: config broker.name is not a supported broker: '";
  message += safe_echo(name);
  message += "' (expected exactly \"";
  message += kKiteName;
  message += "\" or \"";
  message += kKotakName;
  message += "\", case-sensitive)";
  return fail(validation_error(std::move(message)));
}

Result<BrokerAssembly> make_broker(BrokerChoice choice, const BrokerDeps& deps,
                                   const BrokerOptions& options) {
  // 0. An empty capability declaration is an error unless it is deliberate.
  //    This runs FIRST because it is the one input that could otherwise produce
  //    a completely ungated broker port.
  if (auto declared = validate_declaration(options); !declared) {
    return fail(declared.error());
  }

  // 1. Resolve the capability profile. The override, when present, is a TEST
  //    fixture posture (see FixtureCertification) and is recorded on the assembly.
  const bool override_in_effect = options.capability_override.has_value();
  const capabilities::CapabilitySet profile = override_in_effect
                                                  ? options.capability_override.value().asserted
                                                  : real_capabilities(choice);

  // 2. THE LOAD-TIME GATE (AC-2). Reject-only, fail-closed on Unknown, and it
  //    runs while nothing capable of reaching a broker exists yet.
  if (auto gate = profile.require_all(options.required_capabilities); !gate) {
    return fail(gate.error());
  }

  // 3. Only now is it worth checking the wiring.
  if (auto wiring = validate_deps(choice, deps); !wiring) {
    return fail(wiring.error());
  }

  // 4. Construct — handing each stack the ADMITTED profile so every mutation is
  //    re-gated per call (defence in depth; see the wrapper comments).
  std::unique_ptr<ports::BrokerPort> port;
  switch (choice) {
    case BrokerChoice::Kite:
      port = std::make_unique<OwnedKiteBroker>(*deps.kite_http, *deps.kite_secrets,
                                               deps.kite_api_key_secret,
                                               deps.kite_access_token_secret, profile);
      break;
    case BrokerChoice::Kotak:
      port = std::make_unique<OwnedKotakBroker>(*deps.kotak_http, deps.kotak_session, profile);
      break;
  }
  if (!port) {
    return fail(validation_error("composition: unhandled broker choice"));
  }

  return BrokerAssembly(choice, std::move(port), profile, override_in_effect);
}

Result<BrokerAssembly> make_broker(const config::Config& config, const BrokerDeps& deps,
                                   const BrokerOptions& options) {
  auto choice = parse_broker_choice(config.broker.name);
  if (!choice) {
    return fail(choice.error());
  }
  return make_broker(choice.value(), deps, options);
}

Result<ports::Ok> assert_production_posture(const BrokerAssembly& assembly) {
  if (assembly.capability_override_in_effect()) {
    return fail(validation_error(
        "composition: this BrokerAssembly was composed with a FixtureCertification — its "
        "capabilities are a TEST FIXTURE assertion, not a broker's certified profile. A process "
        "that can reach a live broker must not start on one."));
  }
  return ports::ok();
}

Result<ports::Ok> require_capabilities(const BrokerAssembly& assembly,
                                       std::span<const capabilities::Capability> required) {
  // REFUSE TO ANSWER FROM A FIXTURE. A component asking this question after
  // composition never agreed to the fixture posture; echoing a test's assertion
  // back to it as though it were a broker fact is how a fixture escapes its test.
  // "I cannot tell you" is the only truthful answer available here.
  if (auto posture = assert_production_posture(assembly); !posture) {
    return fail(posture.error());
  }
  return assembly.capability_set().require_all(required);
}

}  // namespace broker_exec::composition
