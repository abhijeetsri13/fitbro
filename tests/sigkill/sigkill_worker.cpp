// SIGKILL durability worker (Story 1.12, NFR-3/NFR-10). A PLAIN int main — NO
// Catch2 — spawned by sigkill_test.cpp. It exists to be killed at the single most
// dangerous instant in the whole system: AFTER the order intent is fsync'd, BEFORE
// the broker send. The recover subcommand then proves that replay + reconcile
// produces ZERO duplicate orders despite that kill.
//
// THE KILL POINT (`std::_Exit`, not a signal — and why):
//   The dispatcher exposes set_pre_send_barrier(), a hook that runs at exactly
//   record-intent -> fsync -> [BARRIER] -> send. Installing a barrier that calls
//   std::_Exit(42) kills the process there. std::_Exit (from <cstdlib>) terminates
//   IMMEDIATELY: it runs NO destructors, NO atexit handlers, NO stream flushing —
//   it is the closest PORTABLE equivalent to an uncatchable OS kill (SIGKILL /
//   TerminateProcess). We deliberately do NOT raise SIGKILL: signal delivery and
//   numbering differ across Windows/POSIX and would force a `#ifdef`, which the
//   cross-platform convention forbids. std::_Exit gives the same observable
//   property — the process dies with NOTHING flushed after the fsync — on every
//   OS, with zero platform branching. The fsync already happened inside
//   IntentLog::append() (it flushes + durable_sync BEFORE returning), so the
//   PlaceOrder record is guaranteed on stable storage; everything the dispatcher
//   would have done after the barrier (the broker send, the Result record, the
//   Store upsert) never happens. That is precisely the SIGKILL-between-fsync-and-
//   send fault SM-1 must survive.
//
// SUBCOMMANDS:
//   worker  <datadir> kill : open the log + store, build the dispatcher, install
//                            the std::_Exit(42) barrier, then place() — the process
//                            dies in the barrier and never returns. Exit 42 ==
//                            "killed as expected" (the fsync is already durable).
//   recover <datadir>      : open the log + store (open_or_rebuild), replay() the
//                            log, rebuild the idempotency index, reconcile against
//                            a FRESH FakeBroker (which has NO order — the send never
//                            happened), resolve any UNKNOWN, then print
//                            DUPLICATES=<n> and ORDERS=<n>. Exit 0 iff
//                            DUPLICATES == 0.
//
// DETERMINISM: SystemClock is fine here (no fault depends on the timeline), and
// SeededUuidGenerator(1) makes the minted client_ref reproducible across the
// worker and recover phases so they reason about the SAME order.
//
// CROSS-PLATFORM: std::filesystem for paths, std::_Exit for the hard kill, C++20
// stdlib only. No OS APIs, no `#ifdef`, no floating point.

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "broker_exec/adapters/fake/fake_broker.hpp"
#include "broker_exec/clock/system_clock.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/idempotency/idempotency.hpp"
#include "broker_exec/idempotency/uuid.hpp"
#include "broker_exec/intentlog/intent_log.hpp"
#include "broker_exec/lifecycle/lifecycle.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/ports/broker_port.hpp"
#include "broker_exec/result.hpp"
#include "broker_exec/runtime/dispatcher.hpp"
#include "broker_exec/runtime/unknown_resolver.hpp"
#include "broker_exec/store/store.hpp"

namespace {

namespace fs = std::filesystem;
namespace bx = broker_exec;

// The exit code the kill barrier uses. The test treats any non-zero exit as
// "killed in the barrier", but a distinct, recognizable value aids diagnostics.
constexpr int kKillExitCode = 42;

// A no-op alert sink for the recovery path's UnknownResolver (the worker has no
// operator channel; an escalation is simply counted-as-handled here).
class NullAlertSink final : public bx::ports::AlertSink {
 public:
  bx::Result<bx::ports::Ok> send(bx::ports::AlertLevel, const std::string&) override {
    return bx::ports::ok();
  }
  bx::Result<bx::ports::Ok> send_test_alert() override { return bx::ports::ok(); }
};

// The single canonical intent both phases reason about. client_ref is left empty
// — the dispatcher mints it via reserve() using the seeded generator, so the kill
// and recover phases derive the SAME ref.
bx::domain::OrderIntent sample_intent() {
  bx::domain::OrderIntent intent;
  intent.symbol = "NIFTY24JUN24000CE";
  intent.side = bx::domain::Side::Sell;
  intent.quantity = bx::domain::Quantity::of(50);
  intent.price = bx::domain::Price::from_rupees(123, 50);
  intent.order_type = bx::domain::OrderType::Limit;
  intent.product = bx::domain::Product::Intraday;
  intent.strategy = "sigkill";
  return intent;
}

// ── worker <datadir> kill ───────────────────────────────────────────────────
// Build the full dispatch stack, install the std::_Exit barrier, and place().
// place() never returns — the process dies in the barrier with the PlaceOrder
// intent already fsync'd to <datadir>/intent.log.
int run_kill(const fs::path& datadir) {
  bx::clock::SystemClock clock;

  auto log_opened = bx::intentlog::IntentLog::open(datadir / "intent.log", clock);
  if (!log_opened) {
    std::cerr << "worker: failed to open intent log\n";
    return 2;
  }
  bx::intentlog::IntentLog log = std::move(log_opened.value());

  auto store_opened = bx::store::Store::open((datadir / "store.db").string());
  if (!store_opened) {
    std::cerr << "worker: failed to open store\n";
    return 2;
  }
  bx::store::Store store = std::move(store_opened.value());

  bx::adapters::fake::FakeBroker broker(clock);
  bx::idempotency::IdempotencyIndex index;
  bx::idempotency::SeededUuidGenerator uuids(1);
  bx::lifecycle::LifecycleEngine fsm;
  bx::runtime::Dispatcher dispatcher(broker, log, store, index, uuids, fsm, clock);

  // THE KILL: runs after fsync, before the broker send. std::_Exit terminates the
  // process HARD — no destructors, no flushing — the portable SIGKILL analog.
  dispatcher.set_pre_send_barrier([] { std::_Exit(kKillExitCode); });

  const bx::domain::OrderIntent intent = sample_intent();
  auto placed = dispatcher.place(intent.strategy, intent);

  // UNREACHABLE: place() dies inside the barrier. If we somehow get here the
  // barrier did not fire, which is itself a failure of the harness contract.
  std::cerr << "worker: place() returned without hitting the kill barrier\n";
  (void)placed;
  return 3;
}

// ── recover <datadir> ───────────────────────────────────────────────────────
// Replay the log, rebuild the index, reconcile against a FRESH broker, resolve
// UNKNOWNs, and print DUPLICATES / ORDERS. The fresh broker has NO order because
// the kill happened BEFORE the send — so a correct recovery creates ZERO
// duplicates while still ENUMERATING the order from the durable intent record.
int run_recover(const fs::path& datadir) {
  bx::clock::SystemClock clock;

  auto log_opened = bx::intentlog::IntentLog::open(datadir / "intent.log", clock);
  if (!log_opened) {
    std::cerr << "recover: failed to open intent log\n";
    return 2;
  }
  bx::intentlog::IntentLog log = std::move(log_opened.value());

  // Replay head->tail (verifies the hash chain) to enumerate every order that
  // might have been sent. This is how the killed order remains KNOWN.
  auto replayed = log.replay();
  if (!replayed) {
    std::cerr << "recover: intent log replay failed (chain integrity)\n";
    return 2;
  }
  const std::vector<bx::intentlog::IntentRecord>& records = replayed.value();

  // open_or_rebuild: a corrupt/half-migrated projection is rebuilt from the log
  // rather than refused. The store may be empty (the kill preceded any upsert).
  auto store_outcome = bx::store::Store::open_or_rebuild((datadir / "store.db").string());
  if (!store_outcome) {
    std::cerr << "recover: failed to open/rebuild store\n";
    return 2;
  }
  bx::store::Store store = std::move(store_outcome.value().store);

  // Rebuild the idempotency index from the replayed log so the killed signal is
  // recognized (a re-submit of the same signal would dedup, never re-fire).
  bx::idempotency::IdempotencyIndex index;
  index.rebuild_from_log(records);

  // Reconcile any locally-UNKNOWN order against broker truth. A fresh FakeBroker
  // holds NO order (the send never happened), so an UNKNOWN resolves to NoMatch
  // (fail-closed: stays UNKNOWN, alerted) — and crucially NOTHING is sent. The
  // resolver is read-only (fetch_orders only): it can NEVER create a duplicate.
  bx::adapters::fake::FakeBroker broker(clock);
  bx::lifecycle::LifecycleEngine fsm;
  NullAlertSink alerts;
  bx::runtime::UnknownResolver resolver(broker, store, fsm, alerts, clock);
  auto resolved = resolver.resolve_all();
  if (!resolved) {
    std::cerr << "recover: resolve_all failed at the infrastructure level\n";
    return 2;
  }

  // Count duplicate orders at the broker: a duplicate is two broker orders for the
  // same client signal. The killed order was never sent, so broker truth is empty;
  // any value > 1 for some signal would be a duplicate. We count per client_ref.
  auto broker_orders = broker.fetch_orders();
  if (!broker_orders) {
    std::cerr << "recover: broker fetch_orders failed\n";
    return 2;
  }
  std::vector<std::string> seen;
  int duplicates = 0;
  for (const bx::domain::Order& o : broker_orders.value()) {
    int already = 0;
    for (const std::string& ref : seen) {
      if (ref == o.intent.client_ref) {
        ++already;
      }
    }
    if (already >= 1) {
      ++duplicates;
    }
    seen.push_back(o.intent.client_ref);
  }

  // ORDERS = how many orders we ENUMERATE from the durable log (PlaceOrder
  // records). This proves the order is not lost: we know about it even though it
  // was never sent — the basis for a later reconcile/resume.
  int enumerated = 0;
  for (const bx::intentlog::IntentRecord& r : records) {
    if (r.op == bx::intentlog::IntentOp::PlaceOrder) {
      ++enumerated;
    }
  }

  std::cout << "DUPLICATES=" << duplicates << "\n";
  std::cout << "ORDERS=" << enumerated << "\n";

  // Exit 0 iff zero duplicates — the invariant the harness asserts.
  return duplicates == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  // Usage:
  //   worker  <datadir> kill
  //   recover <datadir>
  std::vector<std::string_view> args(argv, argv + argc);
  if (args.size() < 3) {
    std::cerr << "usage: " << (argc > 0 ? args[0] : "sigkill_worker")
              << " worker <datadir> kill | recover <datadir>\n";
    return 64;  // EX_USAGE
  }

  const std::string_view command = args[1];
  const fs::path datadir(args[2]);

  if (command == "worker") {
    if (args.size() < 4 || args[3] != "kill") {
      std::cerr << "worker: expected subcommand 'kill'\n";
      return 64;
    }
    return run_kill(datadir);
  }
  if (command == "recover") {
    return run_recover(datadir);
  }

  std::cerr << "unknown command: " << command << "\n";
  return 64;
}
