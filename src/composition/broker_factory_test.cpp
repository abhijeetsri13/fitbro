// broker_exec::composition unit tests (Story 6.3). These cover the FACTORY's own
// contract — name parsing, the mandatory capability declaration, the load-time
// gate, the per-call mutation gate, dependency validation, and the fixture
// certification escape hatch — with stub transports.
//
// The end-to-end portability PROOF (the same strategy run against both brokers'
// recorded servers, plus the broker-identifier source scan) lives in
// tests/portability/, because it needs the recorded-fixture servers that the
// conformance suites own.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`, no float.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "broker_exec/adapters/kite/http_client.hpp"
#include "broker_exec/adapters/kotak/kotak_rest_client.hpp"
#include "broker_exec/adapters/kotak/kotak_session.hpp"
#include "broker_exec/capabilities/capabilities.hpp"
#include "broker_exec/composition/broker_factory.hpp"
#include "broker_exec/config/config.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/secret_provider.hpp"
#include "broker_exec/result.hpp"

namespace comp = broker_exec::composition;
namespace caps = broker_exec::capabilities;

namespace {

using broker_exec::Result;
using broker_exec::adapters::kite::HttpClient;
using broker_exec::adapters::kite::HttpRequest;
using broker_exec::adapters::kite::HttpResponse;
using broker_exec::adapters::kotak::KotakSessionBundle;
using broker_exec::ports::SecretProvider;

// A transport that counts what reaches it. Composition must not issue a single
// request; a gate refusal must not either. If one of these counters moves when a
// test says it should not, something started talking to a broker.
class CountingHttpClient final : public HttpClient {
 public:
  mutable int calls = 0;

  [[nodiscard]] Result<HttpResponse> send(const HttpRequest&) const override {
    ++calls;
    HttpResponse response;
    response.status_code = 503;
    response.body = R"({"status":"error","message":"stub transport"})";
    return response;
  }
};

class UnusedSecretProvider final : public SecretProvider {
 public:
  mutable int calls = 0;

  [[nodiscard]] Result<std::string> get(std::string_view) const override {
    ++calls;
    return broker_exec::fail(broker_exec::errors::make_error(
        broker_exec::errors::ErrorCategory::Auth, "stub secret provider", "TEST"));
  }
};

// A synthetic bundle provider — token-SHAPED strings only, no live credentials.
[[nodiscard]] broker_exec::adapters::kotak::BundleProvider stub_session() {
  return []() -> Result<KotakSessionBundle> {
    KotakSessionBundle bundle;
    bundle.access_token = "atSTUB0000AAAA1111BBBB2222";
    bundle.token = "ftSTUB9999GGGG0000HHHH1111";
    bundle.sid = "fsSTUB2222IIII3333JJJJ4444";
    bundle.hs_server_id = "server1";
    return bundle;
  };
}

struct Fixture {
  CountingHttpClient kite_http;
  CountingHttpClient kotak_http;
  UnusedSecretProvider secrets;

  [[nodiscard]] comp::BrokerDeps deps() const {
    comp::BrokerDeps d;
    d.kite_http = &kite_http;
    d.kite_secrets = &secrets;
    d.kotak_http = &kotak_http;
    d.kotak_session = stub_session();
    return d;
  }
};

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

// A declaration good enough to get past step 0 in tests that are about something
// else. Deliberately a helper rather than a default argument: every call site
// still has to name what it needs.
[[nodiscard]] comp::BrokerOptions declaring(std::vector<caps::Capability> required) {
  comp::BrokerOptions options;
  options.required_capabilities = std::move(required);
  return options;
}

[[nodiscard]] comp::BrokerOptions declaring_place() {
  return declaring({caps::Capability::PlaceOrder});
}

[[nodiscard]] broker_exec::domain::OrderIntent some_intent() {
  broker_exec::domain::OrderIntent intent;
  intent.client_ref = "unit-1a2b3c4d-0000111122223333444455556666";
  intent.symbol = "NIFTY24JUN24000CE";
  intent.side = broker_exec::domain::Side::Sell;
  intent.quantity = broker_exec::domain::Quantity::of(50);
  intent.price = broker_exec::domain::Price::from_rupees(123, 50);
  intent.order_type = broker_exec::domain::OrderType::Limit;
  intent.product = broker_exec::domain::Product::Intraday;
  intent.strategy = "unit";
  return intent;
}

}  // namespace

// ── Name parsing: exact, case-sensitive, fail-closed ────────────────────────

TEST_CASE("composition: broker.name parses exactly and fails closed otherwise",
          "[composition][config]") {
  SECTION("the two supported spellings round-trip") {
    auto kite = comp::parse_broker_choice("kite");
    REQUIRE(kite.has_value());
    CHECK(kite.value() == comp::BrokerChoice::Kite);
    CHECK(comp::to_string(kite.value()) == "kite");

    auto kotak = comp::parse_broker_choice("kotak");
    REQUIRE(kotak.has_value());
    CHECK(kotak.value() == comp::BrokerChoice::Kotak);
    CHECK(comp::to_string(kotak.value()) == "kotak");
  }

  SECTION("case variants, whitespace and near-misses are REJECTED, not guessed") {
    // A config value we do not recognize means the operator believes they are
    // trading somewhere we are not. Guessing is how a strategy ends up pointed
    // at the wrong account, so there is no trimming and no case folding.
    for (const char* bad : {"Kite", "KITE", "Kotak", "KOTAK", " kite", "kite ", "kite\n",
                            "zerodha", "kotak-neo", "fake", ""}) {
      UNSCOPED_INFO("broker.name = '" << bad << "'");
      auto parsed = comp::parse_broker_choice(bad);
      REQUIRE_FALSE(parsed.has_value());
      CHECK(parsed.error().category == broker_exec::errors::ErrorCategory::Validation);
      CHECK(parsed.error().action == broker_exec::errors::SuggestedAction::DoNotRetry);
      // The discriminator lives in the MESSAGE. `broker_code` is reserved for a
      // code a BROKER sent us, and nothing here has spoken to one.
      CHECK(parsed.error().broker_code.empty());
      CHECK(contains(parsed.error().message, "broker.name"));
    }
  }

  SECTION("the rejection NAMES a short alphabetic value so a typo is debuggable") {
    auto parsed = comp::parse_broker_choice("zerodha");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(contains(parsed.error().message, "zerodha"));
    // ...and names both accepted spellings, so the fix is in the message.
    CHECK(contains(parsed.error().message, "kite"));
    CHECK(contains(parsed.error().message, "kotak"));
  }

  SECTION("an empty value says so instead of echoing nothing") {
    auto parsed = comp::parse_broker_choice("");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(contains(parsed.error().message, "<empty>"));
  }

  SECTION("anything that is not a short alphabetic word is DESCRIBED, never echoed") {
    // A config field is exactly where an operator pastes a credential by mistake.
    // The echo is therefore restricted to a class of value that cannot be a
    // secret (<=16 chars, letters only); everything else is reported by length.
    // This is stronger than scrub-then-truncate, which can slice the redaction
    // marker in half and only catches shapes it already knows.
    const std::string secret_shaped = "accesstoken0000ZZZZ9999YYYY8888";
    auto parsed = comp::parse_broker_choice(secret_shaped);
    REQUIRE_FALSE(parsed.has_value());
    CHECK_FALSE(contains(parsed.error().message, secret_shaped));
    CHECK(contains(parsed.error().message, "<invalid broker name, 31 chars>"));
  }

  SECTION("a long ALPHABETIC value is not echoed either — length is the only cue") {
    const std::string long_word(64, 'a');
    auto parsed = comp::parse_broker_choice(long_word);
    REQUIRE_FALSE(parsed.has_value());
    CHECK_FALSE(contains(parsed.error().message, long_word));
    CHECK(contains(parsed.error().message, "64 chars"));
  }

  SECTION("an absurdly long value cannot inflate the message") {
    const std::string huge(65536, 'x');
    auto parsed = comp::parse_broker_choice(huge);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().message.size() < 256);
  }
}

// ── Step 0: silence is not a declaration ────────────────────────────────────

TEST_CASE("composition: an EMPTY capability declaration is refused, not waved through",
          "[composition][capabilities][declaration]") {
  // THE BUG THIS PINS: require_all({}) is vacuously ok(), so an empty list used
  // to sail through the gate and return a fully UNGATED port — on a broker whose
  // every capability is Unknown. An omitted declaration must not be the quiet
  // path to more privilege than a stated one.
  Fixture fixture;
  const comp::BrokerDeps deps = fixture.deps();

  SECTION("empty and unflagged is a Validation error on BOTH brokers") {
    for (const comp::BrokerChoice choice : {comp::BrokerChoice::Kite, comp::BrokerChoice::Kotak}) {
      UNSCOPED_INFO("broker: " << comp::to_string(choice));
      auto made = comp::make_broker(choice, deps, comp::BrokerOptions{});
      REQUIRE_FALSE(made.has_value());
      CHECK(made.error().category == broker_exec::errors::ErrorCategory::Validation);
      CHECK(made.error().action == broker_exec::errors::SuggestedAction::DoNotRetry);
      // The message says what to do about it.
      CHECK(contains(made.error().message, "required_capabilities"));
      CHECK(contains(made.error().message, "declares_no_capabilities"));
    }
    // And nothing was constructed or sent on the way to that refusal.
    CHECK(fixture.kite_http.calls == 0);
    CHECK(fixture.kotak_http.calls == 0);
  }

  SECTION("declaring nothing EXPLICITLY is allowed — the read-only-tool escape hatch") {
    // A reconciliation report or health probe genuinely needs no mutation. It
    // says so out loud; that is the whole difference from the section above.
    comp::BrokerOptions read_only;
    read_only.declares_no_capabilities = true;
    auto made = comp::make_broker(comp::BrokerChoice::Kotak, deps, read_only);
    REQUIRE(made.has_value());
    // The flag buys permission to declare nothing. It does not certify anything:
    // the real, all-Unknown profile is still what got attached.
    CHECK_FALSE(made.value().capability_set().supports(caps::Capability::PlaceOrder));
    CHECK(made.value().capability_set().support_of(caps::Capability::PlaceOrder) ==
          caps::Support::Unknown);
  }

  SECTION("the flag does not excuse an unmet declaration") {
    // Setting both is not a loophole: a non-empty list is still checked.
    comp::BrokerOptions confused = declaring_place();
    confused.declares_no_capabilities = true;
    auto made = comp::make_broker(comp::BrokerChoice::Kotak, deps, confused);
    REQUIRE_FALSE(made.has_value());
    CHECK(made.error().category == broker_exec::errors::ErrorCategory::NotSupported);
  }

  SECTION("step 0 runs before the capability gate and before dependency validation") {
    comp::BrokerDeps broken;  // no transport, no secrets, no session provider
    auto made = comp::make_broker(comp::BrokerChoice::Kotak, broken, comp::BrokerOptions{});
    REQUIRE_FALSE(made.has_value());
    CHECK(contains(made.error().message, "required_capabilities"));
  }
}

// ── AC-2: load-time capability rejection ────────────────────────────────────

TEST_CASE("composition: a required capability is gated at LOAD, before any adapter exists",
          "[composition][capabilities]") {
  Fixture fixture;
  const comp::BrokerDeps deps = fixture.deps();

  SECTION("a certified-Supported capability composes") {
    auto made = comp::make_broker(comp::BrokerChoice::Kite, deps,
                                  declaring({caps::Capability::PlaceOrder,
                                             caps::Capability::CancelOrder}));
    REQUIRE(made.has_value());
    CHECK(made.value().choice() == comp::BrokerChoice::Kite);
    CHECK(made.value().capability_set().supports(caps::Capability::PlaceOrder));
    CHECK_FALSE(made.value().capability_override_in_effect());
  }

  SECTION("an UNKNOWN capability is rejected — unknown reads as unsupported") {
    // Every entry in kotak_capabilities() is Unknown until the operator-run live
    // min-qty smoke promotes it (architecture TO-6). The gate must therefore
    // refuse to compose a Kotak assembly for a strategy that needs to place.
    auto made = comp::make_broker(comp::BrokerChoice::Kotak, deps, declaring_place());
    REQUIRE_FALSE(made.has_value());
    CHECK(made.error().category == broker_exec::errors::ErrorCategory::NotSupported);
    CHECK(made.error().action == broker_exec::errors::SuggestedAction::DoNotRetry);
    // The error NAMES the capability that failed.
    CHECK(contains(made.error().message, caps::to_string(caps::Capability::PlaceOrder)));
  }

  SECTION("a certified-Unsupported capability is rejected too") {
    auto made = comp::make_broker(comp::BrokerChoice::Kite, deps,
                                  declaring({caps::Capability::HeadlessSessionRefresh}));
    REQUIRE_FALSE(made.has_value());
    CHECK(made.error().category == broker_exec::errors::ErrorCategory::NotSupported);
    CHECK(contains(made.error().message,
                   caps::to_string(caps::Capability::HeadlessSessionRefresh)));
  }

  SECTION("REJECTED AT LOAD MEANS NOTHING WAS BUILT AND NOTHING WAS SENT") {
    // This is the whole AC-2 claim: the refusal happens at wiring time, so no
    // request ever crosses the transport and no adapter is left half-alive.
    auto made = comp::make_broker(comp::BrokerChoice::Kotak, deps, declaring_place());
    REQUIRE_FALSE(made.has_value());
    CHECK(fixture.kotak_http.calls == 0);
    CHECK(fixture.kite_http.calls == 0);
    CHECK(fixture.secrets.calls == 0);
  }

  SECTION("the gate runs BEFORE dependency validation") {
    // Both are broken; the caller hears "this broker cannot do that", which is
    // the more actionable of the two.
    comp::BrokerDeps broken;  // no transport, no secrets, no session provider
    auto made = comp::make_broker(comp::BrokerChoice::Kotak, broken, declaring_place());
    REQUIRE_FALSE(made.has_value());
    CHECK(made.error().category == broker_exec::errors::ErrorCategory::NotSupported);
  }
}

// ── Defence in depth: the per-call mutation gate ────────────────────────────

TEST_CASE("composition: every MUTATION is re-gated per call, reads are not",
          "[composition][capabilities][percall]") {
  // The load gate already refused any assembly whose DECLARED capabilities were
  // unmet — so this can only fire when that guarantee has eroded: a component
  // that under-declares what it actually calls, or a port reached without going
  // through make_broker(). The scenario below is exactly the first case: a
  // component declares PlaceOrder, is admitted for PlaceOrder only, and then
  // calls cancel/modify/square_off anyway.
  Fixture fixture;

  comp::BrokerOptions options = declaring_place();
  options.capability_override = comp::FixtureCertification(
      caps::CapabilitySet::builder()
          .set(caps::Capability::PlaceOrder, caps::Support::Supported)
          .build());

  auto made = comp::make_broker(comp::BrokerChoice::Kotak, fixture.deps(), options);
  REQUIRE(made.has_value());
  broker_exec::ports::BrokerPort& broker = made.value().broker();

  SECTION("the declared mutation is let through to the adapter") {
    auto placed = broker.place(some_intent());
    // The stub transport answers 503, so this fails — but it fails at the WIRE,
    // not at the gate, which is the point.
    REQUIRE_FALSE(placed.has_value());
    CHECK(placed.error().category != broker_exec::errors::ErrorCategory::NotSupported);
    CHECK(fixture.kotak_http.calls == 1);
  }

  SECTION("an UNDECLARED mutation is refused, and reaches no transport") {
    auto cancelled = broker.cancel("SOME-ORDER-ID");
    REQUIRE_FALSE(cancelled.has_value());
    CHECK(cancelled.error().category == broker_exec::errors::ErrorCategory::NotSupported);
    CHECK(cancelled.error().action == broker_exec::errors::SuggestedAction::DoNotRetry);
    CHECK(contains(cancelled.error().message, caps::to_string(caps::Capability::CancelOrder)));
    // THE POINT: refusing is a local decision. Nothing crossed the wire.
    CHECK(fixture.kotak_http.calls == 0);
  }

  SECTION("modify and square_off are gated on their OWN capabilities") {
    auto modified = broker.modify("SOME-ORDER-ID", some_intent());
    REQUIRE_FALSE(modified.has_value());
    CHECK(modified.error().category == broker_exec::errors::ErrorCategory::NotSupported);
    CHECK(contains(modified.error().message, caps::to_string(caps::Capability::ModifyOrder)));

    auto squared = broker.square_off("SOME-ORDER-ID");
    REQUIRE_FALSE(squared.has_value());
    CHECK(squared.error().category == broker_exec::errors::ErrorCategory::NotSupported);
    CHECK(contains(squared.error().message, caps::to_string(caps::Capability::SquareOff)));

    CHECK(fixture.kotak_http.calls == 0);
  }

  SECTION("READS are never gated — broker truth stays reachable") {
    // An operator recovering from an ambiguous state needs to SEE the book. A
    // capability refusal on an idempotent read could strand them.
    auto orders = broker.fetch_orders();
    REQUIRE_FALSE(orders.has_value());  // the stub transport 503s
    CHECK(orders.error().category != broker_exec::errors::ErrorCategory::NotSupported);
    CHECK(fixture.kotak_http.calls == 1);

    auto positions = broker.fetch_positions();
    CHECK_FALSE(positions.has_value());
    CHECK(positions.error().category != broker_exec::errors::ErrorCategory::NotSupported);
  }
}

TEST_CASE("composition: the per-call gate is silent when the load gate did its job",
          "[composition][capabilities][percall]") {
  // The normal case: everything the component calls was declared and certified,
  // so the extra check never changes an outcome.
  Fixture fixture;
  auto made = comp::make_broker(
      comp::BrokerChoice::Kite, fixture.deps(),
      declaring({caps::Capability::PlaceOrder, caps::Capability::CancelOrder,
                 caps::Capability::SquareOff, caps::Capability::ModifyOrder}));
  REQUIRE(made.has_value());

  // Kite certifies all four, so none of these is refused by the gate; they fail
  // downstream at the stub secret provider / transport instead.
  auto cancelled = made.value().broker().cancel("SOME-ORDER-ID");
  REQUIRE_FALSE(cancelled.has_value());
  CHECK(cancelled.error().category != broker_exec::errors::ErrorCategory::NotSupported);
}

// ── require_capabilities() + the production-posture guard ───────────────────

TEST_CASE("composition: require_capabilities() re-gates an already-composed assembly",
          "[composition][capabilities]") {
  Fixture fixture;
  auto made = comp::make_broker(comp::BrokerChoice::Kite, fixture.deps(), declaring_place());
  REQUIRE(made.has_value());

  const std::vector<caps::Capability> supported = {caps::Capability::PlaceOrder,
                                                   caps::Capability::SquareOff};
  CHECK(comp::require_capabilities(made.value(), supported).has_value());

  const std::vector<caps::Capability> unsupported = {caps::Capability::PlaceOrder,
                                                     caps::Capability::BasketMargin};
  auto gated = comp::require_capabilities(made.value(), unsupported);
  REQUIRE_FALSE(gated.has_value());
  CHECK(gated.error().category == broker_exec::errors::ErrorCategory::NotSupported);
  // It names the FIRST missing capability, not the first in the list.
  CHECK(contains(gated.error().message, caps::to_string(caps::Capability::BasketMargin)));
}

TEST_CASE("composition: require_capabilities() REFUSES to answer from a fixture certification",
          "[composition][capabilities][testonly]") {
  // THE HOLE THIS CLOSES: a later component asking "may I rely on this broker
  // for X" never agreed to a test's fixture posture. Answering "yes" from the
  // override would launder a test assertion into a broker fact — and the caller
  // has no way to tell the difference. The only truthful answer is a refusal.
  Fixture fixture;
  comp::BrokerOptions options = declaring_place();
  options.capability_override = comp::FixtureCertification(
      caps::CapabilitySet::builder()
          .set(caps::Capability::PlaceOrder, caps::Support::Supported)
          .build());

  auto made = comp::make_broker(comp::BrokerChoice::Kotak, fixture.deps(), options);
  REQUIRE(made.has_value());

  const std::vector<caps::Capability> declared = {caps::Capability::PlaceOrder};
  auto gated = comp::require_capabilities(made.value(), declared);
  REQUIRE_FALSE(gated.has_value());
  CHECK(gated.error().category == broker_exec::errors::ErrorCategory::Validation);
  CHECK(gated.error().action == broker_exec::errors::SuggestedAction::DoNotRetry);
  CHECK(contains(gated.error().message, "FixtureCertification"));
}

TEST_CASE("composition: assert_production_posture() fails a fixture-composed assembly",
          "[composition][testonly]") {
  Fixture fixture;

  SECTION("a real profile passes") {
    auto made = comp::make_broker(comp::BrokerChoice::Kite, fixture.deps(), declaring_place());
    REQUIRE(made.has_value());
    CHECK(comp::assert_production_posture(made.value()).has_value());
  }

  SECTION("a fixture certification is a hard startup failure") {
    comp::BrokerOptions options = declaring_place();
    options.capability_override = comp::FixtureCertification(
        caps::CapabilitySet::builder()
            .set(caps::Capability::PlaceOrder, caps::Support::Supported)
            .build());
    auto made = comp::make_broker(comp::BrokerChoice::Kotak, fixture.deps(), options);
    REQUIRE(made.has_value());

    auto posture = comp::assert_production_posture(made.value());
    REQUIRE_FALSE(posture.has_value());
    CHECK(posture.error().category == broker_exec::errors::ErrorCategory::Validation);
    CHECK(contains(posture.error().message, "FixtureCertification"));
  }
}

// ── The test-only fixture certification ─────────────────────────────────────

TEST_CASE("composition: the fixture certification is honored AND flagged",
          "[composition][testonly]") {
  Fixture fixture;
  comp::BrokerOptions options = declaring_place();
  options.capability_override = comp::FixtureCertification(
      caps::CapabilitySet::builder()
          .set(caps::Capability::PlaceOrder, caps::Support::Supported)
          .build());

  auto made = comp::make_broker(comp::BrokerChoice::Kotak, fixture.deps(), options);
  REQUIRE(made.has_value());

  // It must be VISIBLE that these capabilities are a fixture posture and not the
  // broker's certified profile.
  CHECK(made.value().capability_override_in_effect());
  CHECK(made.value().capability_set().supports(caps::Capability::PlaceOrder));

  // The override is scoped to the assembly: it does NOT promote anything in the
  // adapter's real profile, which stays entirely Unknown until the live smoke.
  auto strict = comp::make_broker(comp::BrokerChoice::Kotak, fixture.deps(), declaring_place());
  CHECK_FALSE(strict.has_value());

  // The override is fail-closed for anything it does not list.
  CHECK_FALSE(made.value().capability_set().supports(caps::Capability::CancelOrder));
}

// ── Dependency validation ───────────────────────────────────────────────────

TEST_CASE("composition: a missing dependency is a named Validation error",
          "[composition][deps]") {
  Fixture fixture;

  SECTION("kite without a transport") {
    comp::BrokerDeps deps = fixture.deps();
    deps.kite_http = nullptr;
    auto made = comp::make_broker(comp::BrokerChoice::Kite, deps, declaring_place());
    REQUIRE_FALSE(made.has_value());
    CHECK(made.error().category == broker_exec::errors::ErrorCategory::Validation);
    CHECK(made.error().action == broker_exec::errors::SuggestedAction::DoNotRetry);
    CHECK(made.error().broker_code.empty());
    CHECK(contains(made.error().message, "kite_http"));
  }

  SECTION("kite without a secret provider") {
    comp::BrokerDeps deps = fixture.deps();
    deps.kite_secrets = nullptr;
    auto made = comp::make_broker(comp::BrokerChoice::Kite, deps, declaring_place());
    REQUIRE_FALSE(made.has_value());
    CHECK(contains(made.error().message, "kite_secrets"));
  }

  SECTION("kite with an empty secret NAME") {
    comp::BrokerDeps deps = fixture.deps();
    deps.kite_access_token_secret.clear();
    auto made = comp::make_broker(comp::BrokerChoice::Kite, deps, declaring_place());
    REQUIRE_FALSE(made.has_value());
    CHECK(made.error().category == broker_exec::errors::ErrorCategory::Validation);
  }

  SECTION("kotak without a transport") {
    comp::BrokerDeps deps = fixture.deps();
    deps.kotak_http = nullptr;
    comp::BrokerOptions options;
    options.declares_no_capabilities = true;  // isolate the WIRING failure
    auto made = comp::make_broker(comp::BrokerChoice::Kotak, deps, options);
    REQUIRE_FALSE(made.has_value());
    CHECK(contains(made.error().message, "kotak_http"));
  }

  SECTION("kotak without a session bundle provider") {
    comp::BrokerDeps deps = fixture.deps();
    deps.kotak_session = nullptr;
    comp::BrokerOptions options;
    options.declares_no_capabilities = true;
    auto made = comp::make_broker(comp::BrokerChoice::Kotak, deps, options);
    REQUIRE_FALSE(made.has_value());
    CHECK(contains(made.error().message, "kotak_session"));
  }

  SECTION("the OTHER broker's dependencies may be absent") {
    // One deps struct serves both brokers; only the chosen one is validated.
    // That is what makes the config flip a one-string change.
    comp::BrokerDeps deps;
    deps.kite_http = &fixture.kite_http;
    deps.kite_secrets = &fixture.secrets;
    auto made = comp::make_broker(comp::BrokerChoice::Kite, deps, declaring_place());
    CHECK(made.has_value());
  }
}

// ── The config-driven overload (the flip itself) ────────────────────────────

TEST_CASE("composition: make_broker(config) is the whole broker switch",
          "[composition][config]") {
  Fixture fixture;
  const comp::BrokerDeps deps = fixture.deps();

  broker_exec::config::Config config;  // defaults; only broker.name matters here

  // Both brokers must be composable for the flip to be observable, so this uses
  // the fixture posture. AC-2 covers the default posture separately.
  comp::BrokerOptions options = declaring_place();
  options.capability_override = comp::FixtureCertification(
      caps::CapabilitySet::builder()
          .set(caps::Capability::PlaceOrder, caps::Support::Supported)
          .build());

  SECTION("flipping the one string flips the composed broker") {
    config.broker.name = "kite";
    auto kite = comp::make_broker(config, deps, options);
    REQUIRE(kite.has_value());
    CHECK(kite.value().choice() == comp::BrokerChoice::Kite);

    config.broker.name = "kotak";
    auto kotak = comp::make_broker(config, deps, options);
    REQUIRE(kotak.has_value());
    CHECK(kotak.value().choice() == comp::BrokerChoice::Kotak);
  }

  SECTION("an unknown broker in config is a Validation error, not a default") {
    config.broker.name = "kotakneo";
    auto made = comp::make_broker(config, deps, options);
    REQUIRE_FALSE(made.has_value());
    CHECK(made.error().category == broker_exec::errors::ErrorCategory::Validation);
    CHECK(made.error().broker_code.empty());
    CHECK(contains(made.error().message, "broker.name"));
    CHECK(fixture.kite_http.calls == 0);
    CHECK(fixture.kotak_http.calls == 0);
  }
}

TEST_CASE("composition: the assembly owns the whole stack behind one BrokerPort",
          "[composition][ownership]") {
  // The adapter holds its REST client by reference and the REST client holds the
  // transport by reference; if the intermediate layer were not owned by the
  // assembly, this call would read freed memory. Driving one method through the
  // port after the factory has returned is what proves the ownership chain.
  // The Kite REST client fetches its secrets BEFORE touching the transport, so
  // this test needs a provider that answers (with a token-shaped stub) — the
  // Fixture's failing provider would short-circuit the call before the wire.
  class AnsweringSecretProvider final : public SecretProvider {
   public:
    [[nodiscard]] Result<std::string> get(std::string_view) const override {
      return std::string{"stubSECRET0000AAAA1111BBBB"};
    }
  };

  Fixture fixture;
  AnsweringSecretProvider answering_secrets;
  comp::BrokerDeps deps = fixture.deps();
  deps.kite_secrets = &answering_secrets;
  auto made = comp::make_broker(comp::BrokerChoice::Kite, deps, declaring_place());
  REQUIRE(made.has_value());

  auto orders = made.value().broker().fetch_orders();
  // The stub transport answers 503, so this is an Error rather than a crash —
  // and, critically, the transport WAS reached through the owned stack.
  CHECK_FALSE(orders.has_value());
  CHECK(fixture.kite_http.calls == 1);

  // AND IT SURVIVES A MOVE. The assembly is move-only and callers WILL move it
  // (out of a Result, into a member, through a factory chain). The stack is held
  // behind a unique_ptr precisely so the sibling references inside it never
  // relocate — but that is a claim, and this is the check.
  comp::BrokerAssembly relocated = std::move(made.value());
  auto after_move = relocated.broker().fetch_orders();
  CHECK_FALSE(after_move.has_value());
  CHECK(fixture.kite_http.calls == 2);
  CHECK(relocated.choice() == comp::BrokerChoice::Kite);
  CHECK(relocated.capability_set().supports(caps::Capability::PlaceOrder));
}
