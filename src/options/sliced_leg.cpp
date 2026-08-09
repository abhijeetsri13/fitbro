#include "broker_exec/options/sliced_leg.hpp"

#include <string>
#include <utility>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/slicing/freeze_slicer.hpp"

namespace broker_exec::options {

namespace {

// A redaction-safe tag for an Error: the stable error-category NAME only. The raw
// broker text / message is NEVER copied into `detail` or an alert, so no
// token-shaped content can leak (the alert/`detail` redaction-safe contract).
[[nodiscard]] std::string error_tag(const errors::Error& err) {
  return std::string(errors::to_string(err.category));
}

}  // namespace

std::string_view to_string(ChildPlacement placement) noexcept {
  switch (placement) {
    case ChildPlacement::Acked:
      return "Acked";
    case ChildPlacement::AlreadyPlaced:
      return "AlreadyPlaced";
    case ChildPlacement::Unknown:
      return "Unknown";
  }
  return "Unknown";
}

std::string_view to_string(SlicedLegOutcome outcome) noexcept {
  switch (outcome) {
    case SlicedLegOutcome::FilledSliced:
      return "FilledSliced";
    case SlicedLegOutcome::UnknownPaused:
      return "UnknownPaused";
    case SlicedLegOutcome::SliceRejected:
      return "SliceRejected";
  }
  return "Unknown";
}

SlicedLegResult execute_sliced_leg(const domain::OrderIntent& parent, const domain::Instrument& inst,
                                   const SlicedLegSeams& seams, ports::AlertSink& alerts) {
  SlicedLegResult result;
  result.outcome = SlicedLegOutcome::SliceRejected;
  result.placed_count = 0;
  result.deduped_count = 0;

  // ── Step 0: FAIL-CLOSED — a missing placer places NOTHING ──────────────────
  // Mirrors basket's null place_leg: we never proceed without a way to place.
  if (!seams.place_child) {
    result.detail = "sliced leg rejected: place_child seam not configured (nothing placed)";
    return result;
  }

  // ── Step 1: SLICE via the REAL FreezeSlicer (deterministic <parent>#<k>) ───
  // On a Validation Error (bad lot/freeze/qty, or a `#`-child re-slice) we fail
  // closed: nothing placed, the stable error-category tag in `detail`.
  auto sliced = slicing::FreezeSlicer{}.slice(parent, inst);
  if (!sliced) {
    result.detail = "sliced leg rejected: slicer error: " + error_tag(sliced.error());
    return result;
  }
  const std::vector<domain::OrderIntent>& children = sliced.value();

  // UNKNOWN-pause helper (Step 3 / AC-3): record the paused child, STOP, and fire
  // ONE best-effort Critical alert. Alerting runs AFTER the pause is recorded;
  // the send Result is swallowed AND wrapped in try/catch so a dead or THROWING
  // sink cannot break the no-throw contract (mirrors 5.1 / 5.2).
  const auto pause_for_unknown = [&](const std::string& ref,
                                     const std::string& reason) -> SlicedLegResult& {
    result.children.push_back(ChildResult{ref, ChildPlacement::Unknown, ""});
    result.paused_at_ref = ref;
    result.outcome = SlicedLegOutcome::UnknownPaused;
    // `result.detail` is an IN-PROCESS typed record (never run through a scrubbing
    // sink), so it keeps naming the child ref inline.
    result.detail =
        "sliced leg UNKNOWN at " + ref + " (" + reason + "): placement STOPPED, reconcile before resume";
    // THE ALERT NAMES THE CHILD (IMP-16) via the TYPED ports::AlertContext rather
    // than by interpolation: a sink scrubs the whole free-form body, and a slicer
    // child ref (`<parent>#<k>`) is one long token-shaped run, so the interpolated
    // form reached the operator as `child ***REDACTED*** is ambiguous`. The typed
    // column is rendered through the whole-column allowlist instead.
    ports::AlertContext provenance;
    provenance.client_ref = ref;
    provenance.strategy = parent.strategy;
    // ...and the INSTRUMENT, under its own (uppercase-alnum) shape rule: an option
    // symbol of >=20 chars is a token-shaped run to scrub(), so it could no more
    // have survived the body than the ref could.
    provenance.symbol = parent.symbol;
    try {
      (void)alerts.send_with_context(
          ports::AlertLevel::Critical,
          "SLICED LEG UNKNOWN: child order is ambiguous; placement STOPPED, "
          "reconcile before resume",
          provenance);
    } catch (...) {  // NOLINT(bugprone-empty-catch): alerting is strictly best-effort
    }
    return result;
  };

  // ── Step 2: PLACE the children IN SLICE ORDER (k=1..N) ─────────────────────
  // Children are emitted in a deterministic k=1..N order so they compose cleanly
  // with rate-limiting / exit-priority — but THAT pacing lives INSIDE the
  // `place_child` seam (the token bucket / reserved exit lane the caller wires in),
  // not here: this executor places siblings back-to-back and never blocks. The
  // deterministic ordering is what makes the seam's per-child throttling meaningful.
  result.children.reserve(children.size());
  for (const domain::OrderIntent& child : children) {
    // (a) Idempotency probe. A null seam means "not known to be placed" (proceed
    //     to place — the store's UNIQUE(client_ref) is the real backstop). `true`
    //     dedupes (NO re-send — the SIGKILL-recovery dedupe). An Error is
    //     fail-closed as UNKNOWN: we cannot prove it is safe to (re)send (AC-3).
    if (seams.already_placed) {
      auto known = seams.already_placed(child.client_ref);
      if (!known) {
        return pause_for_unknown(child.client_ref, "already_placed check errored");
      }
      if (known.value()) {
        result.children.push_back(ChildResult{child.client_ref, ChildPlacement::AlreadyPlaced, ""});
        ++result.deduped_count;
        continue;  // already on the broker — do NOT re-send
      }
    }

    // (b) Place the child. An Error is an ambiguous mutating failure that must NOT
    //     be blindly retried (the dispatch no-blind-retry rule) => UNKNOWN-pause.
    auto placed = seams.place_child(child);
    if (!placed) {
      return pause_for_unknown(child.client_ref, "place_child errored");
    }
    auto [state, broker_order_id] = placed.value();
    if (state == ChildPlacement::Acked) {
      result.children.push_back(
          ChildResult{child.client_ref, ChildPlacement::Acked, std::move(broker_order_id)});
      ++result.placed_count;
      continue;
    }
    // Anything not Acked (Unknown — the placer never returns AlreadyPlaced) is an
    // ambiguous placement => pause the whole leg.
    return pause_for_unknown(child.client_ref, "place_child returned Unknown");
  }

  // ── Step 4: SUCCESS — every child ended Acked or AlreadyPlaced ─────────────
  result.outcome = SlicedLegOutcome::FilledSliced;
  result.detail = "sliced leg complete: " + std::to_string(children.size()) + " children (placed " +
                  std::to_string(result.placed_count) + ", deduped " +
                  std::to_string(result.deduped_count) + ")";
  return result;
}

}  // namespace broker_exec::options
