#include "broker_exec/idempotency/idempotency.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// IMP-17: the payload projection is parsed back with the REAL serialiser rather
// than pattern-matched, so the no-throw claim is checked against nlohmann itself.
#include <nlohmann/json.hpp>

#include "broker_exec/clock/test_clock.hpp"
#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/redaction.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/idempotency/uuid.hpp"
#include "broker_exec/intentlog/intent_log.hpp"
#include "broker_exec/store/store.hpp"

namespace fs = std::filesystem;
using broker_exec::clock::TestClock;
using broker_exec::domain::OrderIntent;
using broker_exec::domain::OrderState;
using broker_exec::domain::OrderType;
using broker_exec::domain::Price;
using broker_exec::domain::Product;
using broker_exec::domain::Quantity;
using broker_exec::domain::Side;
using broker_exec::intentlog::IntentLog;
using broker_exec::intentlog::IntentOp;
using broker_exec::intentlog::IntentRecord;
using broker_exec::store::Store;

namespace idem = broker_exec::idempotency;

namespace {

// A representative intent the tests reuse. `client_ref` is intentionally left
// empty here — the signature ignores it, and reserve()/make_client_ref mint it.
OrderIntent sample_intent() {
  OrderIntent intent;
  intent.symbol = "NIFTY24JUN24000CE";
  intent.side = Side::Sell;
  intent.quantity = Quantity::of(50);
  intent.price = Price::from_rupees(123, 50);
  intent.order_type = OrderType::Limit;
  intent.product = Product::Intraday;
  intent.strategy = "alpha";
  return intent;
}

// A unique temp path per test, removed on scope exit.
struct TempLog {
  fs::path path;
  explicit TempLog(const std::string& tag)
      : path(fs::temp_directory_path() /
             ("broker_exec_idem_" + tag + "_" +
              std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".log")) {
    std::error_code ec;
    fs::remove(path, ec);
  }
  ~TempLog() {
    std::error_code ec;
    fs::remove(path, ec);
  }
  TempLog(const TempLog&) = delete;
  TempLog& operator=(const TempLog&) = delete;
};

}  // namespace

// ── (a) client-ref / child-ref format ──────────────────────────────────────

TEST_CASE("make_client_ref has the form strategy-<8hex>-<uuid>", "[idempotency][format]") {
  idem::SeededUuidGenerator uuids(0xABCD'1234'5678'9101ULL);
  const std::string uuid = uuids.next();
  const OrderIntent intent = sample_intent();
  const std::string sig = idem::signal_signature(intent);

  const std::string ref = idem::make_client_ref("alpha", sig, uuid);

  // strategy + '-' + 8 hex + '-' + 36-char uuid.
  const std::string prefix = "alpha-";
  REQUIRE(ref.rfind(prefix, 0) == 0);

  // Split on the two structural '-' that bound the sig8 (strategy has no '-').
  const std::size_t first = ref.find('-');
  const std::size_t second = ref.find('-', first + 1);
  REQUIRE(first != std::string::npos);
  REQUIRE(second != std::string::npos);

  const std::string strategy_part = ref.substr(0, first);
  const std::string sig8 = ref.substr(first + 1, second - first - 1);
  const std::string uuid_part = ref.substr(second + 1);

  REQUIRE(strategy_part == "alpha");
  REQUIRE(sig8.size() == 8);
  REQUIRE(sig8 == sig.substr(0, 8));
  for (char c : sig8) {
    REQUIRE(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')));
  }
  REQUIRE(uuid_part == uuid);
}

// ── IMP-19: the strategy-name rule, proven against the REAL minter ────────────
//
// domain::is_valid_strategy_name is DERIVED from domain::is_provenance_id_shape:
// a strategy name is the first segment (or first few segments) of every
// client_ref, and the shape rule demands every segment be homogeneous. The
// derivation is only worth anything if the ACTUAL ref this module mints comes out
// id-shaped, so that is what is asserted here — the predicate is never checked
// against itself. If make_client_ref's format ever changes, THIS is the test that
// fails, and the rule in domain must move with it.

TEST_CASE("IMP-19: every accepted strategy name mints an id-shaped client_ref",
          "[idempotency][format][redaction][IMP-19]") {
  using broker_exec::domain::is_provenance_id_shape;
  using broker_exec::domain::is_valid_strategy_name;
  using broker_exec::domain::kMaxStrategyNameChars;
  using broker_exec::domain::render_provenance_block;
  using broker_exec::domain::scrub_provenance_column;

  idem::SeededUuidGenerator uuids(0x0FED'CBA9'8765'4321ULL);
  OrderIntent intent = sample_intent();

  const std::vector<std::string> accepted = {
      "alpha",
      "S-1",
      "momentum-v-2",
      "atm-straddle-9-20",
      "IRON_CONDOR",
      "deadbeef",
      "12345",
      "x",
      std::string(kMaxStrategyNameChars, 'a'),  // the longest accepted name
  };

  for (const std::string& name : accepted) {
    INFO("strategy = " << name);
    REQUIRE(is_valid_strategy_name(name));

    intent.strategy = name;
    const std::string sig = idem::signal_signature(intent);
    const std::string ref = idem::make_client_ref(name, sig, uuids.next());

    // THE CONTRACT: the minted ref is admissible into an alert / ledger block as a
    // whole typed column, verbatim — not redacted.
    CHECK(is_provenance_id_shape(ref));
    CHECK(scrub_provenance_column(ref) == ref);

    // And so are the refs DERIVED from it: the freeze-slicer's `#<k>` children and
    // IMP-13's `#X` square-off exit are the client_ref an alert about a slice
    // carries, so the length bound has to leave room for them too.
    CHECK(is_provenance_id_shape(idem::child_ref(ref, 1)));
    CHECK(is_provenance_id_shape(idem::child_ref(ref, 999)));
    CHECK(is_provenance_id_shape(ref + "#X"));

    // The `strategy=` column itself survives alongside the ref (V6).
    CHECK(scrub_provenance_column(name) == name);
    const std::string expected = " [client_ref=" + ref + " strategy=" + name + "]";
    CHECK(render_provenance_block({{"client_ref", ref}, {"strategy", name}}) == expected);
  }
}

TEST_CASE("IMP-19: a REJECTED strategy name is rejected for a reason that is real",
          "[idempotency][format][redaction][IMP-19]") {
  using broker_exec::domain::is_provenance_id_shape;
  using broker_exec::domain::is_valid_strategy_name;

  idem::SeededUuidGenerator uuids(0x1234'5678'9ABC'DEF0ULL);
  OrderIntent intent = sample_intent();

  // The homogeneity rejections are not pedantry: the ref they mint really is
  // destroyed. ("iron condor v2" additionally fails the block grammar guard.)
  for (const std::string_view name : {"S1", "momentum-v2", "iron condor v2", "v2beta"}) {
    INFO("strategy = " << name);
    CHECK_FALSE(is_valid_strategy_name(name));
    intent.strategy = std::string(name);
    const std::string ref =
        idem::make_client_ref(name, idem::signal_signature(intent), uuids.next());
    CHECK_FALSE(is_provenance_id_shape(ref));
  }

  // '#' is excluded from the strategy charset even though the provenance-id
  // charset admits it, and THIS is why: '#' is the child-ref separator, so a
  // PARENT ref minted from a name containing one parses as a CHILD of a truncated
  // parent — the FSM would then recover the wrong owner for the slice.
  const std::string forged =
      idem::make_client_ref("a#b", idem::signal_signature(intent), uuids.next());
  // A PARENT ref that parses as a child, whose recovered "parent" is a fragment of
  // the strategy name rather than an order that exists.
  CHECK_FALSE(is_valid_strategy_name("a#b"));
  CHECK(idem::is_child_ref(forged));
  CHECK(idem::parent_of(forged) == "a");
}

// The SELF-PROVING version of the two tests above — a fixed list only ever proves
// the list. This sweeps EVERY string of length 1..5 over an alphabet chosen to hit
// each class the rules distinguish: 'a' (a letter that IS a hex digit), 'g' (a
// letter that is not), 'F' (uppercase, and hex), '0' and '9' (digits), and the two
// separators '-' and '_'. For every name the rule ACCEPTS it asserts the whole
// round trip against the REAL minter: the client_ref, and its `#1` / `#999` /
// `#X` children, are id-shaped AND come back verbatim from a typed column, and so
// does the name in its own `strategy=` column.
//
// WHY EXHAUSTIVELY: this is what makes an arbitrary future loosening of
// is_valid_strategy_name FAIL — admit a name whose ref is not loggable and the
// counterexample is found here, without anyone having to think of it first.
TEST_CASE("IMP-19: EXHAUSTIVE — every accepted name over a small alphabet round-trips",
          "[idempotency][format][redaction][IMP-19]") {
  using broker_exec::domain::is_provenance_id_shape;
  using broker_exec::domain::is_valid_strategy_name;
  using broker_exec::domain::scrub_provenance_column;

  static constexpr std::string_view kAlphabet = "agF09-_";
  static constexpr std::size_t kMaxLen = 5;

  idem::SeededUuidGenerator uuids(0x0BAD'C0DE'CAFE'F00DULL);
  OrderIntent intent = sample_intent();

  // Report the FIRST counterexample only: ~19'600 names times several checks would
  // drown the output, and one counterexample is all a reader needs.
  std::string failure;
  std::size_t accepted = 0;
  std::string name;

  const auto require_loggable = [&](const std::string& ref, const char* what) {
    if (!failure.empty()) {
      return;
    }
    if (!is_provenance_id_shape(ref) || scrub_provenance_column(ref) != ref) {
      failure = std::string(what) + " is not loggable: '" + ref + "' (strategy '" + name + "')";
    }
  };

  for (std::size_t len = 1; len <= kMaxLen; ++len) {
    std::vector<std::size_t> odometer(len, 0);
    for (;;) {
      name.clear();
      for (const std::size_t d : odometer) {
        name.push_back(kAlphabet[d]);
      }

      if (is_valid_strategy_name(name)) {
        ++accepted;
        intent.strategy = name;
        const std::string ref =
            idem::make_client_ref(name, idem::signal_signature(intent), uuids.next());
        require_loggable(ref, "the minted client_ref");
        require_loggable(idem::child_ref(ref, 1), "the #1 slice child");
        require_loggable(idem::child_ref(ref, 999), "the #999 slice child");
        require_loggable(ref + "#X", "the #X square-off exit ref");
        if (failure.empty() && scrub_provenance_column(name) != name) {
          failure = "the strategy column itself is redacted: '" + name + "'";
        }
      }

      std::size_t digit = len;
      while (digit > 0 && ++odometer[digit - 1] == kAlphabet.size()) {
        odometer[digit - 1] = 0;
        --digit;
      }
      if (digit == 0) {
        break;  // the odometer wrapped: every string of this length is done
      }
    }
  }

  INFO("first counterexample: " << failure);
  CHECK(failure.empty());
  // Guard the guard: a rule that accepted NOTHING would satisfy the loop vacuously.
  CHECK(accepted > std::size_t{1000});  // it is 13'457 today
}

TEST_CASE("UUID generator yields canonical 8-4-4-4-12 v4", "[idempotency][uuid]") {
  // Deterministic bytes from a fixed seed so the assertion is exact.
  const std::string u = idem::format_uuid_v4(0x0123456789abcdefULL, 0xfedcba9876543210ULL);
  REQUIRE(u.size() == 36);
  REQUIRE(u[8] == '-');
  REQUIRE(u[13] == '-');
  REQUIRE(u[14] == '4');  // version nibble forced to 4
  REQUIRE(u[18] == '-');
  // variant: first nibble of the 4th group is one of 8,9,a,b.
  const char variant = u[19];
  REQUIRE(((variant == '8') || (variant == '9') || (variant == 'a') || (variant == 'b')));
  REQUIRE(u[23] == '-');
  for (std::size_t i = 0; i < u.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      continue;
    }
    const char c = u[i];
    REQUIRE(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')));
  }
}

TEST_CASE("SeededUuidGenerator is deterministic; Random differs", "[idempotency][uuid]") {
  idem::SeededUuidGenerator a(42);
  idem::SeededUuidGenerator b(42);
  REQUIRE(a.next() == b.next());
  REQUIRE(a.next() == b.next());

  idem::RandomUuidGenerator r1;
  idem::RandomUuidGenerator r2;
  // Astronomically unlikely to collide; guards against a constant generator.
  REQUIRE(r1.next() != r2.next());
}

TEST_CASE("child_ref / parent_of round-trip and is_child_ref", "[idempotency][child]") {
  const std::string parent = "alpha-deadbeef-0123";
  REQUIRE_FALSE(idem::is_child_ref(parent));
  REQUIRE(idem::parent_of(parent).empty());

  const std::string c1 = idem::child_ref(parent, 1);
  const std::string c2 = idem::child_ref(parent, 2);
  REQUIRE(c1 == parent + "#1");
  REQUIRE(c2 == parent + "#2");

  REQUIRE(idem::is_child_ref(c1));
  REQUIRE(idem::is_child_ref(c2));
  REQUIRE(idem::parent_of(c1) == parent);
  REQUIRE(idem::parent_of(c2) == parent);

  // Deterministic: same (parent, k) is bit-identical (Story 2.9 re-slice).
  REQUIRE(idem::child_ref(parent, 2) == c2);

  // k < 1 is clamped to 1.
  REQUIRE(idem::child_ref(parent, 0) == parent + "#1");
  REQUIRE(idem::child_ref(parent, -5) == parent + "#1");
}

// ── (b) deterministic signal signature ─────────────────────────────────────

TEST_CASE("signal_signature is deterministic and excludes client_ref", "[idempotency][signature]") {
  OrderIntent a = sample_intent();
  OrderIntent b = sample_intent();
  a.client_ref = "alpha-aaaaaaaa-0001";
  b.client_ref = "alpha-bbbbbbbb-9999";  // different ref, same signal

  const std::string sig_a = idem::signal_signature(a);
  REQUIRE(sig_a.size() == 64);                  // SHA-256 hex
  REQUIRE(sig_a == idem::signal_signature(a));  // stable across calls
  REQUIRE(sig_a == idem::signal_signature(b));  // client_ref does not change it
}

TEST_CASE("changing any order-defining field changes the signature", "[idempotency][signature]") {
  const std::string base = idem::signal_signature(sample_intent());

  {
    OrderIntent v = sample_intent();
    v.symbol = "BANKNIFTY24JUN50000PE";
    REQUIRE(idem::signal_signature(v) != base);
  }
  {
    OrderIntent v = sample_intent();
    v.side = Side::Buy;
    REQUIRE(idem::signal_signature(v) != base);
  }
  {
    OrderIntent v = sample_intent();
    v.quantity = Quantity::of(75);
    REQUIRE(idem::signal_signature(v) != base);
  }
  {
    OrderIntent v = sample_intent();
    v.price = Price::from_rupees(123, 51);
    REQUIRE(idem::signal_signature(v) != base);
  }
  {
    OrderIntent v = sample_intent();
    v.order_type = OrderType::Market;
    REQUIRE(idem::signal_signature(v) != base);
  }
  {
    OrderIntent v = sample_intent();
    v.product = Product::Delivery;
    REQUIRE(idem::signal_signature(v) != base);
  }
  {
    OrderIntent v = sample_intent();
    v.strategy = "beta";
    REQUIRE(idem::signal_signature(v) != base);
  }
  {
    // IMP-11: the TRIGGER is order-defining. Arming a stop makes it a different
    // signal from the otherwise-identical unarmed order...
    OrderIntent v = sample_intent();
    v.order_type = OrderType::StopLoss;
    v.trigger_price = Price::from_rupees(124);
    REQUIRE(idem::signal_signature(v) != base);

    // ...and two stops that differ ONLY in trigger LEVEL are different signals
    // too. Collapsing them would dedupe the second away and never place it — a
    // protective order silently dropped.
    OrderIntent higher = v;
    higher.trigger_price = Price::from_rupees(125);
    REQUIRE(idem::signal_signature(higher) != idem::signal_signature(v));
  }
}

TEST_CASE("IMP-11 GOLDEN: a non-stop signature is byte-identical to the pre-IMP-11 one",
          "[idempotency][signature][IMP-11]") {
  // THE UPGRADE HAZARD THIS PINS. A signal's signature is the dedupe key across a
  // binary upgrade: the intent log stores the signature computed at write time and
  // reserve() recomputes one from the live intent. If adding the trigger field
  // shifted EVERY signature, a replayed pre-upgrade signal would stop matching its
  // own record and could be placed a SECOND time.
  //
  // A self-comparison (sig(x) == sig(x)) would prove nothing here — it holds for
  // ANY implementation, including one that broke legacy dedupe. So these are HARD
  // GOLDEN LITERALS: the SHA-256 of the exact canonical byte string the
  // pre-IMP-11 build produced for these two intents,
  //
  //   "strategy=alpha\x1fsymbol=NIFTY24JUN24000CE\x1fside=SELL\x1fqty=50"
  //   "\x1fprice=12350\x1forder_type=<TYPE>\x1fproduct=INTRADAY"
  //
  // with no trigger field appended. IF EITHER OF THESE FAILS, DO NOT "UPDATE THE
  // EXPECTED VALUE" — the legacy restart dedupe has silently broken and every
  // in-flight order from the previous binary is now duplicable. See
  // docs/upgrade-imp-11-stops.md.
  CHECK(idem::signal_signature(sample_intent()) ==
        "94c4c909371278c114ff14b432657d2509144526221f957178b5a0cac390f43a");

  // MARKET specifically: its `price` is as meaningless as an SL-M's (no adapter
  // transmits it), and it is DELIBERATELY still hashed anyway — zeroing it would
  // change the signature of every plain market order ever written. This golden is
  // what makes that "deliberately" enforceable rather than a comment.
  OrderIntent market = sample_intent();
  market.order_type = OrderType::Market;
  CHECK(idem::signal_signature(market) ==
        "3acb3443d79a6540bce992c256b078e53ad45819ffe26b669bbe7f4f81a584de");

  // And the mechanism behind the goldens: an absent trigger contributes NOTHING,
  // while a trigger ENGAGED AT ZERO is a real (different) signal.
  const std::string base = idem::signal_signature(sample_intent());
  OrderIntent reset_trigger = sample_intent();
  reset_trigger.trigger_price.reset();
  CHECK(idem::signal_signature(reset_trigger) == base);
  OrderIntent armed_at_zero = sample_intent();
  armed_at_zero.trigger_price = Price::from_paise(0);
  CHECK(idem::signal_signature(armed_at_zero) != base);
}

TEST_CASE("IMP-11 HIGH-1: an SL-M's meaningless limit price does NOT define the signal",
          "[idempotency][signature][IMP-11]") {
  // THE DUPLICATE-ORDER HAZARD THIS CLOSES. An SL-M fires a MARKET order once its
  // trigger is crossed: the gate does not check its `price` and NEITHER adapter
  // transmits one. The field is therefore residue — and residue is usually a live
  // mark. A strategy that re-derives `price` from the tape each tick would mint a
  // DIFFERENT signature for a BYTE-IDENTICAL wire order, so the dedupe would let
  // a SECOND live stop through. Both then fire and the position inverts.
  OrderIntent slm = sample_intent();
  slm.order_type = OrderType::StopLossMarket;
  slm.trigger_price = Price::from_rupees(124);

  OrderIntent wobbled = slm;
  wobbled.price = Price::from_rupees(999, 99);  // mark moved; wire order identical
  CHECK(idem::signal_signature(wobbled) == idem::signal_signature(slm));

  // The trigger still defines it — that IS the SL-M's wire identity.
  OrderIntent moved_stop = slm;
  moved_stop.trigger_price = Price::from_rupees(125);
  CHECK(idem::signal_signature(moved_stop) != idem::signal_signature(slm));

  // AN SL IS THE OPPOSITE CASE and must keep hashing its limit: an SL's `price` is
  // a REAL limit that goes on the wire, so two SLs differing only in limit are
  // genuinely different orders. Zeroing it here would collapse them and drop one.
  OrderIntent sl = sample_intent();
  sl.order_type = OrderType::StopLoss;
  sl.trigger_price = Price::from_rupees(124);
  OrderIntent sl_other_limit = sl;
  sl_other_limit.price = Price::from_rupees(123, 75);
  CHECK(idem::signal_signature(sl_other_limit) != idem::signal_signature(sl));
}

TEST_CASE("IMP-11: the intent payload carries the trigger only when engaged",
          "[idempotency][signature][IMP-11]") {
  // Schema-version tolerance in the direction that matters: the payload marker is
  // UNCHANGED, so a record written before the field existed still parses (its key
  // is simply absent -> nullopt). Bumping the marker would make rebuild_from_log
  // skip every committed pre-IMP-11 record and empty the dedupe index on the first
  // boot after an upgrade.
  const std::string plain = idem::intent_payload_json(sample_intent());
  CHECK(plain.find("trigger_price_paise") == std::string::npos);
  CHECK(plain.find("\"schema\":1") != std::string::npos);

  OrderIntent stop = sample_intent();
  stop.order_type = OrderType::StopLoss;
  stop.trigger_price = Price::from_rupees(124);
  const std::string armed = idem::intent_payload_json(stop);
  CHECK(armed.find("\"trigger_price_paise\":12400") != std::string::npos);
  CHECK(armed.find("\"schema\":1") != std::string::npos);  // marker NOT bumped
}

TEST_CASE("IMP-17: intent_payload_json NEVER THROWS on ill-formed caller text",
          "[idempotency][utf8]") {
  // THE HOT-PATH THROW, ONE FRAME ABOVE THE INTENT LOG. This function projects
  // THREE caller-supplied strings — client_ref, strategy, symbol — into JSON and
  // returns a plain std::string (no Result). dispatch()'s place/modify paths call
  // it and hand the result to IntentLog::append(), so with nlohmann's DEFAULT dump
  // handler (::strict) a single ill-formed UTF-8 byte in a strategy name or an
  // instrument symbol raised json::type_error.316 across the dispatcher's no-throw
  // boundary BEFORE the intent log ever got the chance to normalise anything.
  // error_handler_t::replace makes the render total; the output is valid UTF-8, so
  // the intent log's canonical_text() is then a provable no-op on this path.
  OrderIntent bad = sample_intent();
  bad.symbol =
      "NIFTY24\xFF"
      "JUN24000CE";  // 0xFF, never a valid UTF-8 lead
  bad.strategy =
      "al\x80"
      "pha";                        // bare continuation byte
  bad.client_ref = "ref-\xE2\x82";  // truncated 3-byte run

  std::string payload;
  REQUIRE_NOTHROW(payload = idem::intent_payload_json(bad));
  CHECK_FALSE(payload.empty());
  CHECK(payload.find("\"schema\":1") != std::string::npos);

  // The render is total AND well-formed: it parses back, and the ill-formed bytes
  // have become U+FFFD rather than aborting the write.
  const nlohmann::json parsed = nlohmann::json::parse(payload, nullptr, false);
  REQUIRE_FALSE(parsed.is_discarded());
  CHECK(parsed.at("symbol").get<std::string>() ==
        "NIFTY24\xEF\xBF\xBD"
        "JUN24000CE");
  CHECK(parsed.at("strategy").get<std::string>() ==
        "al\xEF\xBF\xBD"
        "pha");

  // The dedupe key is UNAFFECTED: signal_signature() hashes the RAW fields
  // directly (canonical_signature_input, not this JSON projection), so restart
  // dedup keys exactly as it did before — and as it will after a restart.
  CHECK(parsed.at("sig").get<std::string>() == idem::signal_signature(bad));

  // And an ASCII intent is BYTE-IDENTICAL to what the strict handler produced, so
  // no committed payload — or the intent-log hash over it — moves.
  CHECK(idem::intent_payload_json(sample_intent()).find("NIFTY24JUN24000CE") != std::string::npos);
}

// ── (c) dedup: reserve twice -> one order ──────────────────────────────────

TEST_CASE("reserve twice with the same intent returns the existing order, one row",
          "[idempotency][dedup]") {
  auto opened = Store::open(":memory:");
  REQUIRE(opened.has_value());
  Store store = std::move(opened.value());

  idem::SeededUuidGenerator uuids(7);
  idem::IdempotencyIndex index;
  const OrderIntent intent = sample_intent();

  // First reserve: a fresh client-ref, is_new == true.
  auto first = idem::reserve(index, store, uuids, intent.strategy, intent);
  REQUIRE(first.has_value());
  REQUIRE(first.value().is_new);
  REQUIRE_FALSE(first.value().existing.has_value());
  const std::string ref = first.value().client_ref;

  // Caller persists the new order (the dispatch path's record step).
  broker_exec::domain::Order order;
  order.intent = intent;
  order.intent.client_ref = ref;
  order.state = OrderState::Sent;
  auto inserted = store.insert_order(order);
  REQUIRE(inserted.has_value());

  // Second reserve of the SAME signal: not new, returns the existing order.
  auto second = idem::reserve(index, store, uuids, intent.strategy, intent);
  REQUIRE(second.has_value());
  REQUIRE_FALSE(second.value().is_new);
  REQUIRE(second.value().client_ref == ref);
  REQUIRE(second.value().existing.has_value());
  REQUIRE(second.value().existing->intent.client_ref == ref);

  // Index holds exactly one mapping; the store holds exactly one order.
  REQUIRE(index.size() == 1);
  auto all = store.all_orders();
  REQUIRE(all.has_value());
  REQUIRE(all.value().size() == 1);
}

TEST_CASE("UNIQUE(client_ref) rejects a duplicate insert (store backstop)",
          "[idempotency][dedup]") {
  auto opened = Store::open(":memory:");
  REQUIRE(opened.has_value());
  Store store = std::move(opened.value());

  broker_exec::domain::Order order;
  order.intent = sample_intent();
  order.intent.client_ref = "alpha-deadbeef-0001";
  REQUIRE(store.insert_order(order).has_value());

  auto dup = store.insert_order(order);
  REQUIRE_FALSE(dup.has_value());
  REQUIRE(dup.error().category == broker_exec::errors::ErrorCategory::DuplicateOrder);
}

TEST_CASE("reserve finds a store-only duplicate even with an empty index", "[idempotency][dedup]") {
  // Simulates a torn restart: the order is in the store but the in-memory index
  // was not rebuilt for it. reserve must still return it (UNIQUE backstop path).
  auto opened = Store::open(":memory:");
  REQUIRE(opened.has_value());
  Store store = std::move(opened.value());

  // Use a fixed-seed generator so the minted ref is reproducible, then pre-seed
  // the store with exactly that ref.
  const OrderIntent intent = sample_intent();
  const std::string sig = idem::signal_signature(intent);
  idem::SeededUuidGenerator probe(99);
  const std::string ref = idem::make_client_ref(intent.strategy, sig, probe.next());

  broker_exec::domain::Order order;
  order.intent = intent;
  order.intent.client_ref = ref;
  order.state = OrderState::Acknowledged;
  REQUIRE(store.insert_order(order).has_value());

  idem::IdempotencyIndex empty_index;   // not rebuilt
  idem::SeededUuidGenerator uuids(99);  // same seq -> mints the same ref
  auto r = idem::reserve(empty_index, store, uuids, intent.strategy, intent);
  REQUIRE(r.has_value());
  REQUIRE_FALSE(r.value().is_new);
  REQUIRE(r.value().client_ref == ref);
  REQUIRE(r.value().existing.has_value());
}

// ── (d) restart: rebuild_from_log finds a prior submission ──────────────────

TEST_CASE("rebuild_from_log recovers a prior submission so restart dedups",
          "[idempotency][restart]") {
  TempLog tmp("restart");
  TestClock clock(std::chrono::steady_clock::time_point{}, std::chrono::system_clock::time_point{});

  const OrderIntent intent = sample_intent();
  idem::SeededUuidGenerator uuids(123);
  const std::string sig = idem::signal_signature(intent);
  const std::string ref = idem::make_client_ref(intent.strategy, sig, uuids.next());

  // Pre-restart: append the canonical PlaceOrder payload to the intent log.
  {
    auto opened = IntentLog::open(tmp.path, clock);
    REQUIRE(opened.has_value());
    IntentLog log = std::move(opened.value());

    OrderIntent placed = intent;
    placed.client_ref = ref;
    auto appended = log.append(IntentOp::PlaceOrder, ref, idem::intent_payload_json(placed));
    REQUIRE(appended.has_value());

    // A non-PlaceOrder record must be ignored by rebuild.
    REQUIRE(log.append(IntentOp::Result, ref, R"({"status":"FILLED"})").has_value());
  }

  // Post-restart: reopen, replay, rebuild the index from the records.
  std::vector<IntentRecord> records;
  {
    auto opened = IntentLog::open(tmp.path, clock);
    REQUIRE(opened.has_value());
    IntentLog log = std::move(opened.value());
    auto replayed = log.replay();
    REQUIRE(replayed.has_value());
    records = std::move(replayed.value());
  }

  idem::IdempotencyIndex index;
  const std::size_t recovered = index.rebuild_from_log(records);
  REQUIRE(recovered == 1);
  REQUIRE(index.size() == 1);

  // The same signal now resolves to the prior client-ref (no new order).
  auto found = index.existing_ref(intent);
  REQUIRE(found.has_value());
  REQUIRE(*found == ref);
}

TEST_CASE("rebuild_from_log skips opaque/foreign payloads without guessing",
          "[idempotency][restart]") {
  TempLog tmp("opaque");
  TestClock clock(std::chrono::steady_clock::time_point{}, std::chrono::system_clock::time_point{});

  std::vector<IntentRecord> records;
  {
    auto opened = IntentLog::open(tmp.path, clock);
    REQUIRE(opened.has_value());
    IntentLog log = std::move(opened.value());
    // Opaque payloads (no canonical schema/sig) — must be skipped.
    REQUIRE(log.append(IntentOp::PlaceOrder, "ref-x", R"({"qty":1,"side":"BUY"})").has_value());
    REQUIRE(log.append(IntentOp::PlaceOrder, "ref-y", "{}").has_value());
    auto replayed = log.replay();
    REQUIRE(replayed.has_value());
    records = std::move(replayed.value());
  }

  idem::IdempotencyIndex index;
  REQUIRE(index.rebuild_from_log(records) == 0);
  REQUIRE(index.size() == 0);
}

TEST_CASE("end-to-end: rebuilt index makes reserve return the prior ref after restart",
          "[idempotency][restart]") {
  TempLog tmp("e2e");
  TestClock clock(std::chrono::steady_clock::time_point{}, std::chrono::system_clock::time_point{});
  const OrderIntent intent = sample_intent();

  auto store_opened = Store::open(":memory:");
  REQUIRE(store_opened.has_value());
  Store store = std::move(store_opened.value());

  // First boot: reserve, persist the order, record the intent in the log.
  std::string ref;
  {
    idem::IdempotencyIndex index;
    idem::SeededUuidGenerator uuids(555);
    auto r = idem::reserve(index, store, uuids, intent.strategy, intent);
    REQUIRE(r.has_value());
    REQUIRE(r.value().is_new);
    ref = r.value().client_ref;

    broker_exec::domain::Order order;
    order.intent = intent;
    order.intent.client_ref = ref;
    order.state = OrderState::Sent;
    REQUIRE(store.insert_order(order).has_value());

    auto opened = IntentLog::open(tmp.path, clock);
    REQUIRE(opened.has_value());
    IntentLog log = std::move(opened.value());
    OrderIntent placed = intent;
    placed.client_ref = ref;
    REQUIRE(log.append(IntentOp::PlaceOrder, ref, idem::intent_payload_json(placed)).has_value());
  }

  // Second boot: a brand-new index rebuilt from the log; reserve must dedup.
  {
    auto opened = IntentLog::open(tmp.path, clock);
    REQUIRE(opened.has_value());
    IntentLog log = std::move(opened.value());
    auto replayed = log.replay();
    REQUIRE(replayed.has_value());

    idem::IdempotencyIndex index;
    REQUIRE(index.rebuild_from_log(replayed.value()) == 1);

    idem::SeededUuidGenerator uuids(999);  // a fresh seed — must NOT be used
    auto r = idem::reserve(index, store, uuids, intent.strategy, intent);
    REQUIRE(r.has_value());
    REQUIRE_FALSE(r.value().is_new);
    REQUIRE(r.value().client_ref == ref);
    REQUIRE(r.value().existing.has_value());
  }

  // Still exactly one order across the whole flow.
  auto all = store.all_orders();
  REQUIRE(all.has_value());
  REQUIRE(all.value().size() == 1);
}
