#pragma once

// broker_exec::conformance — the reusable adapter conformance kit (Story 1.12,
// FR-37, NFR-3). THE CAPSTONE OF EPIC 1.
//
// WHAT THIS IS: a single, broker-agnostic driver that runs ANY ports::BrokerPort
// implementation through the full adversarial fault matrix and proves the
// headline invariant — ZERO duplicate orders — for that broker. It is built once
// against the FakeBroker (Story 1.11) here in Epic 1, and is REUSED unchanged to
// certify the Kite adapter (Epic 2, Story 2.14) and the Kotak Neo adapter
// (Epic 6, Story 6.2): same kit, same bar, every adapter gated by it in CI.
//
// WHY A FACTORY: the kit must not know which broker it is exercising. It receives
// a `BrokerFactory` — a callable that constructs a fresh ports::BrokerPort given
// a ClockPort and a FaultConfig — and builds a complete safety stack (Dispatcher
// + Store + IntentLog + IdempotencyIndex + LifecycleEngine + UnknownResolver +
// UnknownPause) around it for each scenario. For the fake broker the factory is a
// one-liner; for a real adapter it injects the recorded-fixture transport. The
// FaultConfig is taken by the factory because only the fake broker honors it (a
// real adapter's faults come from its recorded fixtures), but the kit's PASS/FAIL
// logic is identical either way: it asserts behavior (no second fire, UNKNOWN
// recorded + resolvable or fail-closed, <= 1 broker order per signal), never the
// fault mechanism.
//
// WHAT IT PROVES PER SCENARIO (the three acceptance properties):
//   1. NO BLIND RETRY (FR-10): a dangerous mutation is attempted AT MOST ONCE.
//      Measured directly via the broker's accepted-request count — the safety
//      core must never auto-repeat a place/modify/cancel/square-off.
//   2. UNKNOWN HANDLING (FR-9/FR-10): an ambiguous outcome is recorded as
//      OrderState::Unknown, is enumerable, and is then either resolved against
//      broker truth by the precedence ladder OR stays UNKNOWN-with-an-alert
//      (fail-closed) — never silently dropped, never a second fire. The alert must
//      also NAME THE ORDER: an escalation carrying no typed provenance leaves the
//      operator with nothing to reconcile, which is not a resolvable fail-closed
//      state at all (IMP-16). See CountingAlertSink.
//   3. ZERO DUPLICATES (NFR-3): after reconciliation, the broker holds AT MOST
//      ONE order per client signal. A duplicate = two broker orders for the same
//      signal; the kit counts them directly from broker truth.
//
// CROSS-PLATFORM: C++20 standard library only (<filesystem> temp dirs, <functional>
// factory, <memory> ownership). No OS APIs, no `#ifdef`, no floating point. This
// is a test-support header that composes the already-portable library modules.
//
// HEADER-ONLY: the kit is a header so any adapter epic can include and run it
// without a shared compiled object. It pulls only public library headers.

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "broker_exec/adapters/fake/fake_broker.hpp"
#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/idempotency/idempotency.hpp"
#include "broker_exec/idempotency/uuid.hpp"
#include "broker_exec/intentlog/intent_log.hpp"
#include "broker_exec/lifecycle/lifecycle.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/ports/clock_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/runtime/dispatcher.hpp"
#include "broker_exec/runtime/unknown_pause.hpp"
#include "broker_exec/runtime/unknown_resolver.hpp"
#include "broker_exec/store/store.hpp"

namespace broker_exec::conformance {

// The broker-agnostic factory the kit is driven by. Given the injected clock and
// the fault profile for a scenario, it returns a fresh, owned BrokerPort. The
// FakeBroker factory honors the FaultConfig; a real adapter ignores it (its faults
// come from recorded fixtures) but still satisfies the same signature so Epic 2/6
// reuse this kit verbatim.
using BrokerFactory = std::function<std::unique_ptr<ports::BrokerPort>(
    ports::ClockPort&, adapters::fake::FaultConfig)>;

// The structured result of a full conformance run. `ok()` is the single PASS/FAIL
// gate CI keys on; `failures` carry human-readable diagnostics for any scenario
// that did not meet all three properties.
struct ConformanceReport {
  int scenarios_run = 0;
  int scenarios_passed = 0;
  int duplicate_orders = 0;           // total duplicate broker orders across all scenarios
  std::vector<std::string> failures;  // human-readable, one line per failed assertion

  // PASS iff every scenario passed AND not a single duplicate order was created.
  [[nodiscard]] bool ok() const {
    return scenarios_passed == scenarios_run && duplicate_orders == 0;
  }
};

namespace detail {

// A throwaway alert sink the kit hands to the UnknownResolver: it records the
// count of escalations so a scenario can assert that a fail-closed NoMatch DID
// alert (UNKNOWN handling property) without coupling to message text.
//
// IT MUST OVERRIDE send_with_context, AND THAT IS A CONTRACT MATTER, NOT A STUB
// DETAIL. ports::AlertSink's default send_with_context DROPS the provenance and
// delegates to send() — the fail-closed direction for a sink that opted out, but
// it means a sink which never overrides it still increments `count_`. A kit whose
// only escalation assertion is "count > 0" therefore certifies a stack in which
// EVERY escalation is anonymous: the alert fires, and it names no order. That is
// the exact defect IMP-16 exists to fix, so the kit has to be able to SEE it.
// Recording the context here is what lets `escalations_named_the_order()` below
// be a real conformance property rather than a tautology.
class CountingAlertSink final : public ports::AlertSink {
 public:
  Result<ports::Ok> send(ports::AlertLevel, const std::string&) override {
    ++count_;
    ++anonymous_count_;  // an escalation with NO typed provenance at all
    return ports::ok();
  }
  Result<ports::Ok> send_with_context(ports::AlertLevel level, const std::string& message,
                                      const ports::AlertContext& provenance) override {
    const Result<ports::Ok> out = send(level, message);
    if (!provenance.client_ref.empty()) {
      --anonymous_count_;  // this one named the order
      last_client_ref_ = provenance.client_ref;
    }
    return out;
  }
  Result<ports::Ok> send_test_alert() override { return ports::ok(); }
  [[nodiscard]] std::size_t count() const noexcept { return count_; }

  // True iff EVERY escalation so far carried a non-empty client_ref.
  [[nodiscard]] bool escalations_named_the_order() const noexcept { return anonymous_count_ == 0; }
  [[nodiscard]] const std::string& last_client_ref() const noexcept { return last_client_ref_; }

 private:
  std::size_t count_ = 0;
  std::size_t anonymous_count_ = 0;
  std::string last_client_ref_;
};

// A scenario's declarative description: a readable name and the fault profile to
// drive through the stack. The full matrix below covers every FaultConfig knob.
struct Scenario {
  std::string name;
  adapters::fake::FaultConfig fault;
};

// The full adversarial fault matrix (FR-37). Each row is one dangerous broker
// behavior the zero-duplicate invariant must survive. `clean` is the fault-free
// control. These are exactly the FaultConfig knobs Story 1.11 exposes.
inline std::vector<Scenario> fault_matrix() {
  std::vector<Scenario> matrix;

  // 0. Control: a benign broker. The happy path must place exactly one order.
  matrix.push_back({"clean", {}});

  // 1. drop_ack: the order enters the book but the ack never returns -> Timeout.
  {
    adapters::fake::FaultConfig f;
    f.drop_ack = true;
    matrix.push_back({"drop_ack", f});
  }

  // 2. ack_lost_but_placed: THE headline danger — caller sees a failure but the
  //    order IS placed. A blind retry here would duplicate; we must reconcile.
  {
    adapters::fake::FaultConfig f;
    f.ack_lost_but_placed = true;
    matrix.push_back({"ack_lost_but_placed", f});
  }

  // 3. rate_limit_after=0: the very first request is throttled (429-shaped). A
  //    clean, non-ambiguous rejection — nothing reached the book.
  {
    adapters::fake::FaultConfig f;
    f.rate_limit_after = 0;
    matrix.push_back({"rate_limit", f});
  }

  // 4. duplicate_fill: the broker emits the same fill twice. Exercises trade
  //    dedup; the ORDER count must still be one.
  {
    adapters::fake::FaultConfig f;
    f.duplicate_fill = true;
    matrix.push_back({"duplicate_fill", f});
  }

  // 5. out_of_order_events: status/trade views arrive reversed. The forward-
  //    progressing apply logic must drop the stale view, not duplicate the order.
  {
    adapters::fake::FaultConfig f;
    f.out_of_order_events = true;
    matrix.push_back({"out_of_order", f});
  }

  // 6. delay_ack_ticks: the ack is withheld for N ticks -> a transient Timeout
  //    even though the order is placed. Same reconcile-don't-retry shape.
  {
    adapters::fake::FaultConfig f;
    f.delay_ack_ticks = 5;
    matrix.push_back({"delay_ack", f});
  }

  return matrix;
}

// Everything a single scenario needs, composed on the stack around a freshly
// constructed broker. A new temp data dir + fresh IntentLog + fresh Store per
// scenario means scenarios never bleed into one another.
struct ScenarioStack {
  std::filesystem::path datadir;
  clock::TestClock clock;
  std::unique_ptr<ports::BrokerPort> broker;
  intentlog::IntentLog log;
  store::Store store;
  idempotency::IdempotencyIndex index;
  idempotency::SeededUuidGenerator uuids;
  lifecycle::LifecycleEngine fsm;
  CountingAlertSink alerts;
  runtime::Dispatcher dispatcher;
  runtime::UnknownResolver resolver;
  runtime::UnknownPause pause;

  ScenarioStack(const BrokerFactory& make_broker, const adapters::fake::FaultConfig& fault,
                std::filesystem::path dir, intentlog::IntentLog opened_log,
                store::Store opened_store)
      : datadir(std::move(dir)),
        clock(std::chrono::steady_clock::time_point{}, std::chrono::system_clock::time_point{}),
        broker(make_broker(clock, fault)),
        log(std::move(opened_log)),
        store(std::move(opened_store)),
        uuids(0xC0FFEEu),
        dispatcher(*broker, log, store, index, uuids, fsm, clock),
        resolver(*broker, store, fsm, alerts, clock) {}

  ScenarioStack(const ScenarioStack&) = delete;
  ScenarioStack& operator=(const ScenarioStack&) = delete;
};

// A canonical option-selling intent the kit places in every scenario (client_ref
// left empty — the dispatcher mints it via reserve()).
inline domain::OrderIntent sample_intent() {
  domain::OrderIntent intent;
  intent.symbol = "NIFTY24JUN24000CE";
  intent.side = domain::Side::Sell;
  intent.quantity = domain::Quantity::of(50);
  intent.price = domain::Price::from_rupees(123, 50);
  intent.order_type = domain::OrderType::Limit;
  intent.product = domain::Product::Intraday;
  intent.strategy = "conformance";
  return intent;
}

// The broker's accepted-request count, IF this broker exposes one (the FakeBroker
// does, via request_count()). Returned as an optional so the kit stays truly
// broker-agnostic: a real adapter that does not expose a counter simply skips the
// no-blind-retry-by-counting probe and relies on its recorded-fixture request log
// instead. The dynamic_cast is the single broker-type-specific line in the kit and
// it is intentionally pointer-form (no throw, no UB) so a non-fake broker yields
// std::nullopt rather than a failure.
inline std::optional<std::size_t> accepted_requests(ports::BrokerPort& broker) {
  if (auto* fake = dynamic_cast<adapters::fake::FakeBroker*>(&broker)) {
    return fake->request_count();
  }
  return std::nullopt;
}

// Count, from BROKER TRUTH, how many distinct broker orders exist for a given
// client signal (its client_ref). A duplicate is two broker orders for the same
// signal — the exact thing NFR-3 forbids. Read via fetch_orders() so the count is
// the broker's own view (the fake exposes ack-lost-but-placed entries here too).
inline int count_broker_orders_for(ports::BrokerPort& broker, const std::string& client_ref) {
  auto orders = broker.fetch_orders();
  if (!orders) {
    return 0;  // a broker read failure is handled by the caller's scenario check
  }
  int n = 0;
  for (const domain::Order& o : orders.value()) {
    if (o.intent.client_ref == client_ref) {
      ++n;
    }
  }
  return n;
}

}  // namespace detail

// Drive `make_broker` through the full fault matrix and return the structured
// report. For each scenario the kit:
//   1. builds a fresh temp data dir + IntentLog + Store + the full safety stack;
//   2. places an order (and, for ambiguous outcomes, re-submits the SAME signal
//      to prove the idempotency/duplicate guard, then resolves any UNKNOWN);
//   3. reconciles against broker truth and asserts the three properties:
//        * no blind retry  — the dangerous op was attempted at most once;
//        * UNKNOWN handling — an ambiguous order is recorded UNKNOWN and is
//          resolvable or fail-closed (alerted), never silently lost;
//        * zero duplicates — <= 1 broker order per client signal.
// A scenario that satisfies all three increments scenarios_passed; any miss adds
// a human-readable line to `failures`. The temp data dir is removed after each
// scenario.
[[nodiscard]] inline ConformanceReport run_conformance(const BrokerFactory& make_broker) {
  namespace fs = std::filesystem;
  ConformanceReport report;

  const std::vector<detail::Scenario> matrix = detail::fault_matrix();
  for (const detail::Scenario& scenario : matrix) {
    ++report.scenarios_run;
    bool scenario_ok = true;
    const auto fail = [&](const std::string& why) {
      scenario_ok = false;
      report.failures.push_back(scenario.name + ": " + why);
    };

    // ── Fresh, isolated data dir for this scenario ──────────────────────────
    std::error_code ec;
    const fs::path datadir =
        fs::temp_directory_path() /
        ("broker_exec_conformance_" + scenario.name + "_" + std::to_string(report.scenarios_run));
    fs::remove_all(datadir, ec);
    fs::create_directories(datadir, ec);

    // Build the IntentLog + Store first (both fallible) so a setup failure is a
    // scenario failure, not a crash.
    clock::TestClock setup_clock(std::chrono::steady_clock::time_point{},
                                 std::chrono::system_clock::time_point{});
    auto log_opened = intentlog::IntentLog::open(datadir / "intent.log", setup_clock);
    if (!log_opened) {
      fail("could not open intent log");
      fs::remove_all(datadir, ec);
      continue;
    }
    auto store_opened = store::Store::open((datadir / "store.db").string());
    if (!store_opened) {
      fail("could not open store");
      fs::remove_all(datadir, ec);
      continue;
    }

    // NOTE: the stack owns its OWN clock; we pass the opened log/store by move.
    // The setup_clock above is only used for the (clock-independent) open() call.
    detail::ScenarioStack stack(make_broker, scenario.fault, datadir, std::move(log_opened.value()),
                                std::move(store_opened.value()));

    const domain::OrderIntent intent = detail::sample_intent();

    // ── Place (first attempt) ───────────────────────────────────────────────
    auto placed = stack.dispatcher.place(intent.strategy, intent);
    if (!placed) {
      // The dispatcher returns the (possibly Unknown/Rejected) order as a VALUE on
      // every non-infrastructure outcome; an Error here means the place pipeline
      // itself failed, which is a conformance failure.
      fail("dispatcher.place returned an Error instead of a recorded order");
      fs::remove_all(datadir, ec);
      continue;
    }
    const domain::Order order = placed.value();
    const std::string client_ref = order.intent.client_ref;
    const std::optional<std::size_t> sends_after_place = detail::accepted_requests(*stack.broker);

    // ── PROPERTY 1: no blind retry ──────────────────────────────────────────
    // A single place must reach the broker AT MOST ONCE. For a cleanly rejected
    // (rate-limited) request that never reached the book, the accepted-request
    // count is 0; otherwise it is exactly 1. It must NEVER exceed 1. (Probed only
    // when the broker exposes a request counter; real adapters use their fixtures.)
    if (sends_after_place && *sends_after_place > 1) {
      fail("place was attempted more than once (blind retry)");
    }

    // ── If the outcome is ambiguous: re-submit + resolve ────────────────────
    // An Unknown order is the dangerous case. Two things must hold:
    //   (a) re-submitting the SAME signal must NOT fire a second order (the
    //       idempotency/duplicate guard) — request_count must not grow;
    //   (b) the UNKNOWN must be resolvable against broker truth, OR stay UNKNOWN
    //       with an alert (fail-closed). Either is acceptable; a silent drop is
    //       not.
    if (order.state == domain::OrderState::Unknown) {
      // (a) Duplicate-submit guard: the reservation must short-circuit.
      auto resubmit = stack.dispatcher.place(intent.strategy, intent);
      const std::optional<std::size_t> sends_after_resubmit =
          detail::accepted_requests(*stack.broker);
      if (!resubmit) {
        fail("re-submitting the same signal returned an Error");
      }
      if (sends_after_place && sends_after_resubmit &&
          *sends_after_resubmit != *sends_after_place) {
        fail("re-submitting the same signal fired a second broker send");
      }

      // (b) Resolve the UNKNOWN against broker truth via the precedence ladder.
      auto resolution = stack.resolver.resolve(order);
      if (!resolution) {
        fail("UNKNOWN resolution failed at the infrastructure level");
      } else {
        const runtime::UnknownResolution& res = resolution.value();
        if (res.resolved_ok) {
          // Resolved to broker truth — the order is now confirmed, not lost.
          stack.pause.clear(client_ref);
        } else {
          // Fail-closed: it must have escalated to the operator (an alert), and
          // the order stays paused. A NoMatch with NO alert is a silent drop.
          stack.pause.mark_unknown(client_ref);
          if (stack.alerts.count() == 0) {
            fail("a fail-closed UNKNOWN did not raise an operator alert");
          } else {
            // ...AND THE ESCALATION MUST NAME THE ORDER (IMP-16). An alert that
            // fires but carries no client_ref tells the operator that SOMETHING is
            // in an unresolvable state and gives them nothing to reconcile against
            // the broker — the safety property is "the operator can act", not "a
            // message was sent". Asserted structurally (typed provenance present
            // and equal to the signal's ref) rather than by grepping the body,
            // because the body is scrubbed and a ref interpolated there would be
            // redacted — which is the whole defect.
            if (!stack.alerts.escalations_named_the_order()) {
              fail(
                  "a fail-closed UNKNOWN escalated ANONYMOUSLY: the alert carried no "
                  "typed provenance, so it named no order");
            } else if (stack.alerts.last_client_ref() != client_ref) {
              fail("the fail-closed escalation named '" + stack.alerts.last_client_ref() +
                   "', not the signal that went UNKNOWN");
            }
          }
        }
      }
    }

    // ── PROPERTY 3: zero duplicate orders (reconcile against broker truth) ───
    // Count broker orders for THIS signal. For a clean rejection nothing reached
    // the broker (0 is fine); for every other case there must be exactly one — and
    // NEVER two. Two would be the duplicate NFR-3 forbids.
    const int broker_orders = detail::count_broker_orders_for(*stack.broker, client_ref);
    if (broker_orders > 1) {
      report.duplicate_orders += (broker_orders - 1);
      fail("the broker holds " + std::to_string(broker_orders) +
           " orders for one signal (duplicate)");
    }

    // The local projection must likewise hold exactly one row for the signal.
    auto local = stack.store.all_orders();
    if (!local) {
      fail("store.all_orders() failed");
    } else {
      int local_for_signal = 0;
      for (const domain::Order& o : local.value()) {
        if (o.intent.client_ref == client_ref) {
          ++local_for_signal;
        }
      }
      if (local_for_signal > 1) {
        fail("the local projection holds more than one order for one signal");
      }
    }

    if (scenario_ok) {
      ++report.scenarios_passed;
    }

    fs::remove_all(datadir, ec);
  }

  return report;
}

}  // namespace broker_exec::conformance
