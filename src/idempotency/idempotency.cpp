#include "broker_exec/idempotency/idempotency.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <utility>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/money.hpp"
#include "sha256.hpp"  // broker_exec::intentlog::sha256_hex (intentlog target)

namespace broker_exec::idempotency {
namespace {

using json = nlohmann::json;

// The canonical-payload schema marker. Bumped if the payload shape changes; on
// rebuild we only interpret payloads carrying the current marker (so a foreign
// or future payload is skipped, not misread).
constexpr int kPayloadSchema = 1;

// The unit separator (ASCII 0x1F) between canonical fields. A control byte that
// cannot appear in a symbol/strategy id, so field boundaries are unambiguous.
constexpr char kSep = '\x1f';

// The limit price AS THE WIRE SEES IT — the only version of it that can define
// an order.
//
// A StopLossMarket fires a MARKET order once its trigger is crossed: the gate
// does not check its `price`, and NEITHER adapter transmits one (Kite omits the
// field, Kotak sends pr="0"). The number is therefore pure residue on an SL-M —
// and residue is usually a live mark. A strategy that re-derives `price` from the
// tape each tick would mint a DIFFERENT signature for a byte-identical wire
// order, defeating the dedupe: the second call looks like a fresh signal, gets a
// fresh client_ref, and a SECOND live stop goes on. Both then fire and the
// position ends up inverted — naked in the opposite direction.
//
// MARKET IS DELIBERATELY LEFT ALONE. Its `price` is equally meaningless, but
// zeroing it here would change the signature of every plain market order ever
// written, breaking the legacy-stability property this function otherwise holds
// (see signal_signature's contract). SL-M signatures are ALREADY moving in this
// release — the stop level relocated from `price` to `trigger_price` — so folding
// this correction into the same break costs nothing extra.
[[nodiscard]] std::int64_t defining_price_paise(const domain::OrderIntent& intent) {
  return intent.order_type == domain::OrderType::StopLossMarket ? 0 : intent.price.paise();
}

// Build the canonical signature input over the order-defining fields ONLY, in a
// fixed order with explicit field tags and a stable separator. Tagging each
// field (not just concatenating values) avoids ambiguity (e.g. strategy "a-b"
// vs symbol boundaries). All numeric fields use exact integer paise/values —
// never any float (binding money convention). This byte string is what gets
// SHA-256'd; its stability IS the determinism guarantee.
[[nodiscard]] std::string canonical_signature_input(const domain::OrderIntent& intent) {
  std::string s;
  s.reserve(128);
  s += "strategy=";
  s += intent.strategy;
  s += kSep;
  s += "symbol=";
  s += intent.symbol;
  s += kSep;
  s += "side=";
  s += domain::to_string(intent.side);
  s += kSep;
  s += "qty=";
  s += std::to_string(intent.quantity.value());
  s += kSep;
  s += "price=";
  s += std::to_string(defining_price_paise(intent));  // 0 for SL-M; see above
  s += kSep;
  s += "order_type=";
  s += domain::to_string(intent.order_type);
  s += kSep;
  s += "product=";
  s += domain::to_string(intent.product);
  // The TRIGGER is order-defining (IMP-11): two stops on the same symbol/qty/limit
  // that arm at DIFFERENT levels are different signals, and folding them onto one
  // signature would make the second look like a duplicate of the first and never
  // be sent — a protective order silently dropped.
  //
  // APPENDED ONLY WHEN PRESENT, AND THAT IS LOAD-BEARING, NOT TIDINESS. A signal's
  // signature is the dedupe key across a BINARY UPGRADE: the intent log stores the
  // signature that was computed at write time, and reserve() recomputes one from
  // the live intent. If this field always appeared, every signature would change
  // on the upgrade, so a replayed pre-upgrade signal would no longer match its own
  // record and could be placed a SECOND time. No pre-IMP-11 intent could carry a
  // trigger (the field did not exist), so leaving it off when absent makes every
  // legacy signature byte-identical to what the old build produced — the dedupe
  // survives the upgrade, and only genuinely-new stop shapes get new keys.
  if (intent.trigger_price.has_value()) {
    s += kSep;
    s += "trigger=";
    s += std::to_string(intent.trigger_price->paise());
  }
  return s;
}

}  // namespace

std::string signal_signature(const domain::OrderIntent& intent) {
  return intentlog::sha256_hex(canonical_signature_input(intent));
}

std::string sig8_of(std::string_view signature_hex) {
  // First 8 hex chars; if a caller passes a shorter string, take what is there
  // (callers always pass a full 64-char SHA-256 hex, so this is defensive).
  const std::size_t n = signature_hex.size() < 8 ? signature_hex.size() : std::size_t{8};
  return std::string(signature_hex.substr(0, n));
}

std::string intent_payload_json(const domain::OrderIntent& intent) {
  // Carry both the precomputed signature (so rebuild needs no domain types) AND
  // the order-defining fields (so a future reader could recompute it). Compact,
  // single-line (the intent log requires a single JSON line, no embedded '\n').
  json j;
  j["schema"] = kPayloadSchema;
  j["sig"] = signal_signature(intent);
  j["client_ref"] = intent.client_ref;
  j["strategy"] = intent.strategy;
  j["symbol"] = intent.symbol;
  j["side"] = std::string(domain::to_string(intent.side));
  j["qty"] = intent.quantity.value();
  j["price_paise"] = intent.price.paise();
  j["order_type"] = std::string(domain::to_string(intent.order_type));
  j["product"] = std::string(domain::to_string(intent.product));
  // The trigger is written ONLY when the intent has one, so the key's ABSENCE on
  // replay is the record's way of saying nullopt. This is deliberately an ADDITIVE
  // field at the SAME kPayloadSchema: rebuild_from_log() only reads `schema` and
  // `sig`, and it SKIPS any record whose schema marker it does not recognize —
  // bumping the marker would therefore make every committed pre-IMP-11 record
  // unreadable and silently empty the dedupe index on the first boot after an
  // upgrade. Old records replay to nullopt; new records carry the value.
  if (intent.trigger_price.has_value()) {
    j["trigger_price_paise"] = intent.trigger_price->paise();
  }
  // error_handler_t::replace, NOT the default (IMP-17). The default handler is
  // ::strict and THROWS json::type_error.316 on the first ill-formed UTF-8 byte,
  // and THREE of the fields above are CALLER-SUPPLIED TEXT — client_ref, strategy
  // and symbol. This function returns a plain std::string (no Result), and it is
  // called from dispatch()'s place/modify paths ONE FRAME ABOVE
  // IntentLog::append(), so a throw here escapes the dispatcher's no-throw
  // boundary before the intent log ever gets a chance to normalise anything.
  //
  // BYTE-IDENTICAL for every payload written so far: the two handlers differ only
  // on input the strict one would have rejected outright, so no existing record's
  // payload — or the intent-log hash over it — moves. The output is valid UTF-8,
  // which makes the intent log's own canonical_text() a provable no-op on this
  // path; the signature in `sig` is computed over the RAW fields by
  // signal_signature(), independently of this projection, so restart-dedup is
  // unaffected either way.
  return j.dump(-1, ' ', /*ensure_ascii=*/false, json::error_handler_t::replace);
}

std::string make_client_ref(std::string_view strategy, std::string_view signature_hex,
                            std::string_view uuid) {
  std::string ref;
  ref.reserve(strategy.size() + 1 + 8 + 1 + uuid.size());
  ref += strategy;
  ref += '-';
  ref += sig8_of(signature_hex);
  ref += '-';
  ref += uuid;
  return ref;
}

std::string child_ref(std::string_view parent_client_ref, int k) {
  const int kk = k < 1 ? 1 : k;
  std::string ref(parent_client_ref);
  ref += '#';
  ref += std::to_string(kk);
  return ref;
}

bool is_child_ref(std::string_view client_ref) noexcept {
  const std::size_t pos = client_ref.rfind('#');
  // Must have a non-empty parent before '#' and a non-empty suffix after it.
  return pos != std::string_view::npos && pos > 0 && pos + 1 < client_ref.size();
}

std::string parent_of(std::string_view child_client_ref) {
  if (!is_child_ref(child_client_ref)) {
    return std::string{};
  }
  const std::size_t pos = child_client_ref.rfind('#');
  return std::string(child_client_ref.substr(0, pos));
}

// ── IdempotencyIndex ───────────────────────────────────────────────────────

std::size_t IdempotencyIndex::rebuild_from_log(
    const std::vector<intentlog::IntentRecord>& records) {
  by_signature_.clear();
  for (const intentlog::IntentRecord& rec : records) {
    if (rec.op != intentlog::IntentOp::PlaceOrder) {
      continue;  // only PlaceOrder records carry a signal to dedup.
    }
    // Parse the canonical payload. We rely on the PlaceOrder payload being the
    // canonical intent_payload_json(): it must be a JSON object carrying our
    // schema marker and a "sig". Anything else (an opaque/foreign payload, a
    // parse error) is skipped — we never guess a signature from arbitrary bytes.
    json j = json::parse(rec.payload_json, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
      continue;
    }
    auto schema_it = j.find("schema");
    auto sig_it = j.find("sig");
    if (schema_it == j.end() || !schema_it->is_number_integer() ||
        schema_it->get<int>() != kPayloadSchema) {
      continue;
    }
    if (sig_it == j.end() || !sig_it->is_string()) {
      continue;
    }
    std::string signature = sig_it->get<std::string>();
    if (signature.empty() || rec.client_ref.empty()) {
      continue;
    }
    // First writer wins: the earliest PlaceOrder for a signal owns the mapping
    // (matches register_ref semantics), so a later re-record cannot rebind it.
    by_signature_.emplace(std::move(signature), rec.client_ref);
  }
  return by_signature_.size();
}

std::optional<std::string> IdempotencyIndex::existing_ref(
    const domain::OrderIntent& intent) const {
  return existing_ref_for_signature(signal_signature(intent));
}

std::optional<std::string> IdempotencyIndex::existing_ref_for_signature(
    std::string_view signature_hex) const {
  const auto it = by_signature_.find(std::string(signature_hex));
  if (it == by_signature_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void IdempotencyIndex::register_ref(const domain::OrderIntent& intent, std::string client_ref) {
  register_signature(signal_signature(intent), std::move(client_ref));
}

void IdempotencyIndex::register_signature(std::string signature_hex, std::string client_ref) {
  // First writer wins: emplace is a no-op if the signature is already mapped.
  by_signature_.emplace(std::move(signature_hex), std::move(client_ref));
}

// ── reserve ────────────────────────────────────────────────────────────────

Result<Reservation> reserve(IdempotencyIndex& index, store::Store& store, UuidGenerator& uuids,
                            std::string_view strategy, const domain::OrderIntent& intent) {
  const std::string signature = signal_signature(intent);

  // (2) Already-seen signal: return the prior ref, attaching the stored Order.
  if (auto prior = index.existing_ref_for_signature(signature); prior.has_value()) {
    Result<std::optional<domain::Order>> found = store.find_order(*prior);
    if (!found.has_value()) {
      return fail(found.error());
    }
    Reservation r;
    r.client_ref = *prior;
    r.is_new = false;
    r.existing = std::move(found.value());
    return r;
  }

  // (3) Mint a fresh client-ref. Guard against an existing client_ref in the
  // store (the UNIQUE(client_ref) backstop / a torn restart where the index was
  // not fully rebuilt): if it already exists, treat it as the existing order.
  const std::string client_ref = make_client_ref(strategy, signature, uuids.next());
  Result<std::optional<domain::Order>> existing = store.find_order(client_ref);
  if (!existing.has_value()) {
    return fail(existing.error());
  }
  if (existing.value().has_value()) {
    index.register_signature(signature, client_ref);
    Reservation r;
    r.client_ref = client_ref;
    r.is_new = false;
    r.existing = std::move(existing.value());
    return r;
  }

  // (4) Fresh signal + fresh ref: register and report new.
  index.register_signature(signature, client_ref);
  Reservation r;
  r.client_ref = client_ref;
  r.is_new = true;
  r.existing = std::nullopt;
  return r;
}

}  // namespace broker_exec::idempotency
