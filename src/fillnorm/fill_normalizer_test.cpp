#include "broker_exec/fillnorm/fill_normalizer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>

#include "broker_exec/domain/enums.hpp"
#include "broker_exec/domain/redaction.hpp"

using broker_exec::domain::OrderState;
using broker_exec::domain::to_string;
using broker_exec::fillnorm::exit_qty_for;
using broker_exec::fillnorm::FillSnapshot;
using broker_exec::fillnorm::is_authoritative;
using broker_exec::fillnorm::normalize_fill;

TEST_CASE("THE CORE FIX: a partial fill arriving as 'UPDATE' is PartiallyFilled, not not-filled",
          "[fillnorm]") {
  // Kite delivers a partial as a websocket order-update of type UPDATE (NOT
  // COMPLETE). Driving off the EVENT TYPE would read this as "not filled" and
  // re-enter / mismanage. Driving off the FILLED QUANTITY gives the truth.
  const FillSnapshot snap = normalize_fill("UPDATE", /*filled=*/30, /*total=*/100,
                                           /*from_reconcile=*/true);
  CHECK(snap.canonical_state == OrderState::PartiallyFilled);
  CHECK(snap.filled_qty == 30);
  CHECK(snap.pending_qty == 70);
}

TEST_CASE("COMPLETE / fully filled maps to Filled with no pending", "[fillnorm]") {
  const FillSnapshot snap = normalize_fill("COMPLETE", 100, 100, true);
  CHECK(snap.canonical_state == OrderState::Filled);
  CHECK(snap.filled_qty == 100);
  CHECK(snap.pending_qty == 0);
}

TEST_CASE("a working order with no fill is Acknowledged (OPEN / TRIGGER PENDING)", "[fillnorm]") {
  const FillSnapshot open = normalize_fill("OPEN", 0, 100, true);
  CHECK(open.canonical_state == OrderState::Acknowledged);
  CHECK(open.filled_qty == 0);
  CHECK(open.pending_qty == 100);

  // A stop that is armed but not triggered: still a live working order, no fill.
  const FillSnapshot trig = normalize_fill("TRIGGER PENDING", 0, 50, true);
  CHECK(trig.canonical_state == OrderState::Acknowledged);
  CHECK(trig.pending_qty == 50);
}

TEST_CASE("a partial that is then cancelled stays Cancelled but PRESERVES the filled qty",
          "[fillnorm]") {
  // A cancel can leave a partial fill behind; the filled_qty is what matters
  // downstream, so it must be carried, not zeroed.
  const FillSnapshot snap = normalize_fill("CANCELLED", 40, 100, true);
  CHECK(snap.canonical_state == OrderState::Cancelled);
  CHECK(snap.filled_qty == 40);
  CHECK(snap.pending_qty == 60);
}

TEST_CASE("a rejected order maps to Rejected", "[fillnorm]") {
  const FillSnapshot snap = normalize_fill("REJECTED", 0, 100, true);
  CHECK(snap.canonical_state == OrderState::Rejected);
  CHECK(snap.filled_qty == 0);
}

TEST_CASE("an unrecognized status with no quantity signal is Unknown (force reconcile)",
          "[fillnorm]") {
  const FillSnapshot snap = normalize_fill("WEIRD_NEW_STATE", 0, 0, true);
  CHECK(snap.canonical_state == OrderState::Unknown);
  CHECK(snap.filled_qty == 0);
  CHECK(snap.pending_qty == 0);
}

TEST_CASE("authoritative reconcile may size an exit; an identical push may NOT", "[fillnorm]") {
  // A reconcile read with a real fill is trustworthy: exit may be sized off it.
  const FillSnapshot reconciled = normalize_fill("COMPLETE", 50, 50, /*from_reconcile=*/true);
  CHECK(is_authoritative(reconciled));
  CHECK(reconciled.exit_qty_trustworthy);
  CHECK(exit_qty_for(reconciled) == 50);

  // The SAME fill arriving as a push (websocket/postback) is NOT authoritative:
  // pushes fire out of order and miss events, so they can never directly drive an
  // exit. exit_qty_for must be 0 — reconcile first.
  const FillSnapshot push = normalize_fill("COMPLETE", 50, 50, /*from_reconcile=*/false);
  CHECK_FALSE(is_authoritative(push));
  CHECK_FALSE(push.exit_qty_trustworthy);
  CHECK(exit_qty_for(push) == 0);

  // Both agree on the fill FACTS — only the trust differs.
  CHECK(push.canonical_state == OrderState::Filled);
  CHECK(push.filled_qty == 50);
}

TEST_CASE("an authoritative reconcile that observed no fill yields no exit qty", "[fillnorm]") {
  const FillSnapshot snap = normalize_fill("OPEN", 0, 100, /*from_reconcile=*/true);
  CHECK(is_authoritative(snap));
  CHECK_FALSE(snap.exit_qty_trustworthy);  // authoritative but filled == 0
  CHECK(exit_qty_for(snap) == 0);
}

TEST_CASE("quantities are clamped: negatives become zero, filled>total never goes negative",
          "[fillnorm]") {
  const FillSnapshot neg = normalize_fill("UPDATE", /*filled=*/-5, /*total=*/-100, true);
  CHECK(neg.filled_qty == 0);
  CHECK(neg.pending_qty == 0);

  // filled > total must clamp pending to 0, never a negative.
  const FillSnapshot over = normalize_fill("UPDATE", /*filled=*/120, /*total=*/100, true);
  CHECK(over.filled_qty == 120);
  CHECK(over.pending_qty == 0);
  CHECK(over.canonical_state == OrderState::Filled);  // filled>0, pending==0, total>0
}

TEST_CASE("detail is redaction-safe: a token-shaped raw status does not survive", "[fillnorm]") {
  // A raw status carrying a long token-shaped run must never appear in detail.
  const std::string secret = "abcd1234efgh5678ijkl9012mnop";  // >=20-char alnum run
  const FillSnapshot snap = normalize_fill(secret, 0, 0, false);
  CHECK(snap.detail.find(secret) == std::string::npos);
  // detail names the canonical state, not the raw broker text.
  CHECK(snap.detail.find(std::string(to_string(snap.canonical_state))) != std::string::npos);
}

TEST_CASE("a default-constructed FillSnapshot is fail-closed", "[fillnorm]") {
  const FillSnapshot snap;  // forgotten / unconstructed
  CHECK(snap.canonical_state == OrderState::Unknown);
  CHECK(snap.filled_qty == 0);
  CHECK(snap.pending_qty == 0);
  CHECK_FALSE(snap.authoritative);
  CHECK_FALSE(snap.exit_qty_trustworthy);
  CHECK_FALSE(is_authoritative(snap));
  CHECK(exit_qty_for(snap) == 0);  // never trusts a fill it did not observe
}

TEST_CASE("canonical state names reuse the stable domain::to_string contract", "[fillnorm]") {
  // The normalizer reuses domain::to_string(OrderState) — assert the stable names
  // for the states it emits (renames are breaking observability changes).
  CHECK(to_string(OrderState::PartiallyFilled) == "PARTIALLY_FILLED");
  CHECK(to_string(OrderState::Filled) == "FILLED");
  CHECK(to_string(OrderState::Acknowledged) == "ACKNOWLEDGED");
  CHECK(to_string(OrderState::Cancelled) == "CANCELLED");
  CHECK(to_string(OrderState::Rejected) == "REJECTED");
  CHECK(to_string(OrderState::Unknown) == "UNKNOWN");
}

TEST_CASE("HIGH fix: filled>0 with a ZERO/omitted total is still Filled (drive off quantity)") {
  // A broker that omitted/zeroed the order total must NOT make a real fill fall
  // through to a not-filled state.
  const FillSnapshot snap =
      normalize_fill("UPDATE", /*filled=*/50, /*total=*/0, /*reconcile=*/true);
  CHECK(snap.canonical_state == OrderState::Filled);
  CHECK(snap.filled_qty == 50);
  CHECK(snap.pending_qty == 0);
  CHECK(exit_qty_for(snap) == 50);
}

TEST_CASE("Rejected carries a partial fill (the filled qty is preserved)") {
  const FillSnapshot snap = normalize_fill("REJECTED", /*filled=*/15, /*total=*/100, true);
  CHECK(snap.canonical_state == OrderState::Rejected);
  CHECK(snap.filled_qty == 15);  // a reject after a partial still leaves 15 done
  CHECK(exit_qty_for(snap) == 15);
}

TEST_CASE("a PARTIAL fill arriving as a PUSH sizes no exit (reconcile first)") {
  const FillSnapshot push = normalize_fill("UPDATE", 30, 100, /*reconcile=*/false);
  CHECK(push.canonical_state == OrderState::PartiallyFilled);  // state is still correct
  CHECK_FALSE(push.authoritative);
  CHECK(exit_qty_for(push) == 0);  // ...but a push must never drive an exit
}
