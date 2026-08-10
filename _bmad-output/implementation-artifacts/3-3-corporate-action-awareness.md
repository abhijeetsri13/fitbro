# Story 3.3: Corporate-action awareness

Status: ready-for-dev

## Story

As an operator,
I want splits/bonus/symbol changes recognized as corporate actions,
so that they are not misread as a mismatch. (FR-5)

## Acceptance Criteria

1. **Given** a configured corporate-action source **When** a held position's qty/price/symbol changes via a corporate
   action **Then** it is classified as such (position re-based, token re-resolved) without a false mismatch alert or a
   corrective order.
2. **And** absence of a configured source is surfaced, not silently assumed.

## Tasks / Subtasks

- [ ] Task 1: Add corporate-action awareness to the EXISTING `reconcile` module (AC: all)
  - [ ] `include/broker_exec/reconcile/corporate_actions.hpp` + `src/reconcile/corporate_actions.cpp`; add to
        `broker_exec_reconcile`. Reuses `domain::Position`/`Quantity`/`Price`, `ports::AlertSink`. No new dep.
- [ ] Task 2: Corporate-action model + source port (AC: 1)
  - [ ] `enum class CorporateActionKind { Split, Bonus, SymbolChange, FnoAdjustment };`
  - [ ] `struct CorporateAction { std::string symbol; CorporateActionKind kind; std::int64_t qty_num = 1; std::int64_t qty_den = 1;
        std::string new_symbol; std::int64_t new_token = 0; };` where the post-action quantity = `qty * qty_num / qty_den`
        (a 1:2 split = qty_num 2 / qty_den 1; the price multiplier is the INVERSE `qty_den/qty_num` so position VALUE is
        preserved). SymbolChange/FnoAdjustment carry `new_symbol`/`new_token`.
  - [ ] `class CorporateActionSource { public: virtual ~CorporateActionSource() = default;
        [[nodiscard]] virtual std::optional<CorporateAction> action_for(std::string_view symbol) const = 0; };`
        (the date is the source's own concern; a concrete refdata-backed source lands later — abstract here).
- [ ] Task 3: Classifier + re-base (AC: 1) — integer math, NO float
  - [ ] `struct CorporateActionOutcome { bool is_corporate_action = false; bool source_missing = false; domain::Position rebased; std::string detail; };`
  - [ ] `class CorporateActionClassifier` (ctor `(const CorporateActionSource* source, ports::AlertSink& alerts)`; a NULL
        source means NOT configured):
    `[[nodiscard]] CorporateActionOutcome classify(const domain::Position& believed, const domain::Position& broker_observed) const`:
    - If the believed and broker_observed positions AGREE (same symbol + qty + price) -> nothing changed -> is_corporate_action=false, source_missing=false (no-op).
    - Else a change exists. If `source == nullptr` (NOT configured): SURFACE it — `alerts.send(AlertLevel::Error, "corporate-action
      source not configured; cannot classify position change on <symbol>")`, set `source_missing = true`,
      `is_corporate_action = false`. (AC-2: fail-visible — the caller must NOT silently treat it as a mismatch OR a CA.)
    - Else look up `source->action_for(believed.symbol)`. If an action exists AND re-basing `believed` by it MATCHES
      `broker_observed` (re-based qty == broker qty, re-based symbol == broker symbol; price within the action's expected
      adjustment) -> `is_corporate_action = true`, `rebased` = the re-based position (qty + price re-based, symbol/token
      re-resolved). Else `is_corporate_action = false` (the caller falls through to manual-intervention/mismatch — a CA that
      does NOT explain the observed change must not be force-applied).
  - [ ] `[[nodiscard]] static domain::Position rebase(const domain::Position& pos, const CorporateAction& ca)`:
        qty' = `pos.net_qty * qty_num / qty_den` (integer; reject/flag a non-divisible result — no fractional shares);
        price' = `pos.avg_price.paise() * qty_den / qty_num` (value-preserving; integer paise); symbol' = new_symbol (if set),
        and carry new_token where the domain models it (Position has symbol + qty + price; token re-resolution is documented as
        the instrument-master refresh consuming new_token — note it). NO float.
- [ ] Task 4: Fail-visible absence (AC-2)
  - [ ] When `source == nullptr` and a change is present, the classifier surfaces it (Error alert + `source_missing`) so the
        operator knows the bot cannot reason about the change — never silently assume "no corporate action" and never silently
        treat the change as a manual intervention.
- [ ] Task 5: CMake — extend `src/reconcile/CMakeLists.txt`
  - [ ] Add `corporate_actions.cpp` to `broker_exec_reconcile`; add `corporate_actions_test.cpp` to `broker_exec_reconcile_tests`.
- [ ] Task 6: Tests (AC: 1, 2) — `src/reconcile/corporate_actions_test.cpp` (CountingAlertSink; a fake CorporateActionSource)
  - [ ] split 1:2: believed long 50 @ ₹100 (10000 paise); source returns a Split qty_num 2/qty_den 1; broker shows 100 @ ₹50
        (5000 paise) -> is_corporate_action==true, rebased qty 100 @ 5000 paise, NO false mismatch (the caller would not alert).
  - [ ] bonus 1:1 (qty doubles, price halves to preserve value) -> classified, re-based.
  - [ ] symbol change: believed symbol "OLD" 50 @ ₹100; source returns SymbolChange new_symbol "NEW" new_token 999; broker shows
        "NEW" 50 @ ₹100 -> is_corporate_action==true, rebased.symbol=="NEW" (token re-resolution noted).
  - [ ] a CA exists but does NOT match the observed change (broker shows 30, not the split's 100) -> is_corporate_action==false
        (the caller treats it as a manual/mismatch — a CA is not force-applied).
  - [ ] AC-2: source == nullptr AND a change is present -> source_missing==true, an Error alert was sent, is_corporate_action==false
        (NOT silently a CA, NOT silently manual).
  - [ ] no change (believed == broker) -> is_corporate_action==false, source_missing==false, no alert.
  - [ ] re-base integer exactness: assert the re-based qty/price are the exact expected integers (no float, value preserved); a
        non-divisible split (e.g. odd qty on a 2:3) is flagged/handled (document the choice).

## Dev Notes

- **Classify BEFORE flagging manual** — a held-position change explained by a corporate action is re-based + token re-resolved,
  NOT a mismatch (3.1) or manual intervention (3.2); the main loop consults the classifier first. [architecture.md#FR-5, RCT-4]
- **Fail-visible absence (AC-2)** — a NULL/absent corporate-action source + an unexplained change is SURFACED (Error alert +
  source_missing), never silently assumed. [architecture.md#RCT-4 "absence surfaced, not assumed"]
- **Value-preserving re-base, integer math, no float** — qty *= num/den, price *= den/num (paise). Reject fractional shares. [docs/conventions.md#Money]
- **Token re-resolution** = the instrument-master refresh (Story 2.6) consumes new_token; here the classifier carries new_symbol/new_token.
- **Reuse:** `domain::Position`/`Quantity`/`Price`, `ports::AlertSink`, the reconcile module, CountingAlertSink (tests).

### References
- [Source: epics.md#Story 3.3] [architecture.md#FR-5 corporate actions, #RCT-4 corporate-action source] [Source: src/reconcile/* (3.1/3.2 siblings)]
- [Source: docs/conventions.md] [Source: include/broker_exec/domain/types.hpp, ports/alert_sink.hpp]

## Dev Agent Record
### Agent Model Used
### Completion Notes List
### File List
