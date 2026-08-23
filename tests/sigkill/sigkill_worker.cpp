// SIGKILL durability worker (Story 1.12, NFR-3/NFR-10). A PLAIN int main — NO
// Catch2 — spawned by sigkill_test.cpp. It exists to be killed at the single most
// dangerous instant in the whole system: AFTER the order intent is fsync'd, BEFORE
// the broker send. The recover subcommand then proves that replay + reconcile +
// RE-SUBMIT produces ZERO duplicate orders despite that kill.
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
// RECOVERY MODEL — WHY THE RECOVERY BROKER IS SEEDED, NOT FRESH:
//   The kill lands in the pre-send barrier, so we CANNOT KNOW whether the send
//   left the box. This subcommand used to model that as "it did not": it built a
//   brand-new FakeBroker — which has no persistence, so its book is empty BY
//   CONSTRUCTION — and counted duplicates in it. That count was 0 for every
//   possible build of this library. Deleting IdempotencyIndex::rebuild_from_log
//   outright left the harness green, because the only branch in which a restart
//   can create a duplicate is the one where the order IS ALREADY AT THE BROKER,
//   and that branch was never modelled at all.
//   So recovery now assumes the DANGEROUS branch: the recovery broker is SEEDED
//   with the order carrying the client_ref recovered from the durable PlaceOrder
//   record, and recovery then RE-SUBMITS the same signal through a real
//   Dispatcher — which is what a strategy that was mid-signal when the box died
//   actually does on restart. DUPLICATES=0 now means what it says: restart-dedup
//   recognised the signal from the rebuilt index and sent nothing. Fail closed on
//   doubt: model the fault that can hurt, not the one that cannot.
//
// SUBCOMMANDS:
//   worker  <datadir> kill : open the log + store, build the dispatcher, install
//                            the std::_Exit(42) barrier, then place() — the process
//                            dies in the barrier and never returns. Exit 42 ==
//                            "killed as expected" (the fsync is already durable).
//   recover <datadir> [--no-dedup]
//                          : replay the log, rebuild the idempotency index, seed a
//                            FakeBroker with the killed order (RECOVERY MODEL
//                            above), run the reconcile pass, then re-submit the
//                            same signal through a real Dispatcher and count
//                            duplicate broker orders. Writes its numbers to stdout
//                            AND to <datadir>/recover_report.txt. Exit 0 iff the
//                            re-submit cost zero broker sends and left zero
//                            duplicates; 1 on a duplicate; 2 on an infrastructure
//                            failure (including "no PlaceOrder record to recover",
//                            so a datadir where no kill ever happened is REFUSED
//                            rather than reported clean). --no-dedup is the
//                            negative control — see RecoverOptions.
//
// DETERMINISM: SystemClock is fine here (no fault depends on the timeline), and
// SeededUuidGenerator(1) makes the minted client_ref reproducible across the
// worker and recover phases so they reason about the SAME order.
//
// CROSS-PLATFORM: std::filesystem for paths, std::_Exit for the hard kill, C++20
// stdlib only. No OS APIs, no `#ifdef`, no floating point.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
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

// Where recovery writes its numbers. platform::run_command hands back ONLY an exit
// code, so anything merely printed cannot be asserted — which is exactly how
// ORDERS came to be computed, printed and ignored. This file is the channel the
// Catch2 test reads.
constexpr const char* kRecoverReportFile = "recover_report.txt";

// A no-op alert sink for the recovery path's UnknownResolver (the worker has no
// operator channel; an escalation is simply counted-as-handled here).
class NullAlertSink final : public bx::ports::AlertSink {
 public:
  bx::Result<bx::ports::Ok> send(bx::ports::AlertLevel, const std::string&) override {
    return bx::ports::ok();
  }
  bx::Result<bx::ports::Ok> send_test_alert() override { return bx::ports::ok(); }
};

// Linear membership test. The harness handles a handful of orders, so a plain
// loop keeps this file free of anything the assertion has to reason around.
bool contains(const std::vector<std::string>& haystack, const std::string& needle) {
  for (const std::string& s : haystack) {
    if (s == needle) {
      return true;
    }
  }
  return false;
}

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

// How the recovery phase is wired. The DEFAULTS are the honest configuration; the
// other values exist solely so the harness can run a NEGATIVE CONTROL that proves
// this exe is capable of reporting a duplicate at all. A durability harness never
// observed going red is not evidence of anything.
struct RecoverOptions {
  // Rebuild the idempotency index from the replayed log. This is layer 2 of the
  // three-layer dedup and, after a PRE-SEND kill, the ONLY layer that can fire:
  // Dispatcher::place() calls insert_order() only AFTER the send, so the
  // projection is empty and the UNIQUE(client_ref) backstop has nothing to match.
  // Turning it off models a build whose rebuild_from_log is broken.
  bool rebuild_index = true;

  // Seed for the client-ref UUID source. Seed 1 reproduces the ref the kill run
  // minted; a DIFFERENT seed models a build whose reserve() mints a fresh ref on
  // every call, so the re-fired order carries a client_ref nobody has seen. That
  // is the shape a ref-keyed duplicate count cannot see — see DUPLICATE_REFS.
  std::uint64_t uuid_seed = 1;
};

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

// ── recover <datadir> [--no-dedup] ──────────────────────────────────────────
// Replay the log, rebuild the index, seed broker truth with the killed order,
// reconcile, then RE-SUBMIT the same signal through a real Dispatcher. The
// re-submit is the only step that can create the duplicate this harness exists to
// catch; everything before it is setup. See RECOVERY MODEL at the top of the file.
int run_recover(const fs::path& datadir, const RecoverOptions& opts) {
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

  // ORDERS = how many orders we ENUMERATE from the durable log (PlaceOrder
  // records), and the FIRST such record's client_ref is the identity of the order
  // whose fate the kill made ambiguous — the ref broker truth is seeded with below.
  //
  // REFUSE AN EMPTY LOG. With no PlaceOrder record there is nothing to recover, so
  // every count below would be a vacuous zero and this exe would exit 0 on a
  // datadir where no kill ever happened — indistinguishable from surviving one.
  // That indistinguishability is the defect this subcommand was rewritten for;
  // fail closed instead of reporting clean.
  std::string killed_ref;
  int enumerated = 0;
  for (const bx::intentlog::IntentRecord& r : records) {
    if (r.op != bx::intentlog::IntentOp::PlaceOrder) {
      continue;
    }
    ++enumerated;
    if (killed_ref.empty()) {
      killed_ref = r.client_ref;
    }
  }
  if (enumerated == 0 || killed_ref.empty()) {
    std::cerr << "recover: no durable PlaceOrder record — nothing to recover\n";
    return 2;
  }

  // open_or_rebuild: a corrupt/half-migrated projection is rebuilt from the log
  // rather than refused. The store IS empty here — the kill preceded any upsert —
  // which is deliberate: it leaves the rebuilt index as the ONLY dedup layer that
  // can fire, so a break in it cannot be masked by the UNIQUE(client_ref) backstop.
  auto store_outcome = bx::store::Store::open_or_rebuild((datadir / "store.db").string());
  if (!store_outcome) {
    std::cerr << "recover: failed to open/rebuild store\n";
    return 2;
  }
  bx::store::Store store = std::move(store_outcome.value().store);

  // Rebuild the idempotency index from the replayed log so the killed signal is
  // recognized (a re-submit of the same signal dedups instead of re-firing).
  bx::idempotency::IdempotencyIndex index;
  std::size_t index_entries = 0;
  if (opts.rebuild_index) {
    index_entries = index.rebuild_from_log(records);
  }

  // BROKER TRUTH AS IT MUST BE ASSUMED. We do not know whether the send left the
  // box, so we assume IT DID — the only branch in which a restart can duplicate.
  // The seeded order carries the client_ref recovered from the durable record, so
  // the broker holds exactly the order the killed process was about to send.
  bx::adapters::fake::FakeBroker broker(clock);
  bx::domain::OrderIntent killed = sample_intent();
  killed.client_ref = killed_ref;
  if (auto seeded = broker.place(killed); !seeded) {
    std::cerr << "recover: failed to seed broker truth with the killed order\n";
    return 2;
  }

  // Reconcile any locally-UNKNOWN order against broker truth. STATE PLAINLY WHAT
  // THIS PROVES HERE: nothing. The resolver iterates the STORE, and a pre-send kill
  // leaves the projection empty, so resolve_all() finds no UNKNOWN row and is a
  // no-op. It stays in the sequence because a real recovery runs it before acting,
  // and being read-only (fetch_orders only) it cannot perturb the count below.
  // Projecting an un-resulted PlaceOrder back into the store on boot is a
  // PRODUCTION gap, not a harness one — Store::open_or_rebuild rebuilds the SQLite
  // projection, it does not replay the intent log — so the duplicate proof rests on
  // the re-submit below, never on this call.
  bx::lifecycle::LifecycleEngine fsm;
  NullAlertSink alerts;
  bx::runtime::UnknownResolver resolver(broker, store, fsm, alerts, clock);
  auto resolved = resolver.resolve_all();
  if (!resolved) {
    std::cerr << "recover: resolve_all failed at the infrastructure level\n";
    return 2;
  }

  // THE RE-SUBMIT — the capstone step. A strategy that was mid-signal when the box
  // died re-issues the SAME signal on restart. A correct build recognises it from
  // the rebuilt index and sends NOTHING; a broken one fires a second order at a
  // broker that already holds the first. RESENDS counts broker requests
  // attributable to this call alone, so it is measured across place() and nothing
  // else (the fetch_orders below would otherwise inflate it).
  bx::idempotency::SeededUuidGenerator uuids(opts.uuid_seed);
  bx::runtime::Dispatcher dispatcher(broker, log, store, index, uuids, fsm, clock);
  const bx::domain::OrderIntent intent = sample_intent();
  const std::size_t requests_before = broker.request_count();
  auto resubmitted = dispatcher.place(intent.strategy, intent);
  if (!resubmitted) {
    std::cerr << "recover: re-submit failed at the infrastructure level\n";
    return 2;
  }
  // Narrowed to int deliberately: every reported value is a small count, and one
  // signed type across the report keeps the comparisons below free of the
  // signed/unsigned mismatch /W4 + /WX rejects.
  const int resends = static_cast<int>(broker.request_count() - requests_before);

  auto broker_orders = broker.fetch_orders();
  if (!broker_orders) {
    std::cerr << "recover: broker fetch_orders failed\n";
    return 2;
  }

  // A duplicate is TWO BROKER ORDERS FOR THE SAME SIGNAL, so it is counted on the
  // signal SIGNATURE — which deliberately excludes the client_ref — and NOT on the
  // ref itself. Keying on the ref is what a build with a broken reserve() defeats
  // for free: its second order carries a brand-new ref, so a ref-keyed counter
  // reports zero while the account holds two lots. DUPLICATE_REFS is reported
  // alongside only so the negative control can pin that distinction; DUPLICATES is
  // the invariant.
  int duplicates = 0;
  int duplicate_refs = 0;
  std::vector<std::string> seen_signatures;
  std::vector<std::string> seen_refs;
  for (const bx::domain::Order& o : broker_orders.value()) {
    const std::string signature = bx::idempotency::signal_signature(o.intent);
    if (contains(seen_signatures, signature)) {
      ++duplicates;
    } else {
      seen_signatures.push_back(signature);
    }
    if (contains(seen_refs, o.intent.client_ref)) {
      ++duplicate_refs;
    } else {
      seen_refs.push_back(o.intent.client_ref);
    }
  }

  std::ostringstream report;
  report << "DUPLICATES=" << duplicates << "\n"
         << "DUPLICATE_REFS=" << duplicate_refs << "\n"
         << "ORDERS=" << enumerated << "\n"
         << "BROKER_ORDERS=" << static_cast<int>(broker_orders.value().size()) << "\n"
         << "RESENDS=" << resends << "\n"
         << "INDEX_ENTRIES=" << static_cast<int>(index_entries) << "\n";
  std::cout << report.str();

  std::ofstream out(datadir / kRecoverReportFile);
  out << report.str();
  out.close();
  if (!out) {
    std::cerr << "recover: failed to write the recovery report\n";
    return 2;
  }

  // FAIL CLOSED on every observable a duplicate moves: a second order for the
  // signal, a second order under the same ref, or ANY broker mutation issued by the
  // re-submit. An already-recorded signal must cost exactly zero sends (FR-10).
  const bool clean = duplicates == 0 && duplicate_refs == 0 && resends == 0;
  return clean ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  // Usage:
  //   worker  <datadir> kill
  //   recover <datadir> [--no-dedup]
  std::vector<std::string_view> args(argv, argv + argc);
  if (args.size() < 3) {
    std::cerr << "usage: " << (argc > 0 ? args[0] : "sigkill_worker")
              << " worker <datadir> kill | recover <datadir> [--no-dedup]\n";
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
    // An UNRECOGNISED argument is rejected, never ignored. The pre-fix build
    // silently swallowed extra argv, so a test could ask for a mode that did not
    // exist and still be handed exit 0 — a green light for a run that never
    // happened.
    if (args.size() > 4) {
      std::cerr << "recover: too many arguments\n";
      return 64;
    }
    RecoverOptions opts;
    if (args.size() == 4) {
      if (args[3] != "--no-dedup") {
        std::cerr << "recover: unknown option: " << args[3] << "\n";
        return 64;
      }
      // NEGATIVE CONTROL: model a build whose restart-dedup is broken. Skip the
      // index rebuild AND mint from a different UUID seed, so the re-submit fires a
      // second order under a NEW client_ref — the exact fault this harness claims
      // to catch. It MUST report DUPLICATES=1 and exit non-zero; a run that stays
      // green under this flag proves the harness is measuring nothing.
      opts.rebuild_index = false;
      opts.uuid_seed = 2;
    }
    return run_recover(datadir, opts);
  }

  std::cerr << "unknown command: " << command << "\n";
  return 64;
}
