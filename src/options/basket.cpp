#include "broker_exec/options/basket.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "broker_exec/errors/error.hpp"
#include "broker_exec/ports/ports_common.hpp"

namespace broker_exec::options {

namespace {

// A redaction-safe tag for an Error: the stable error-category NAME only. The
// raw broker text / message is NEVER copied into `detail` or an alert, so no
// token-shaped content can leak (the alert/`detail` redaction-safe contract).
[[nodiscard]] std::string error_tag(const errors::Error& err) {
  return std::string(errors::to_string(err.category));
}

// Deterministic basket-id fallback used when config.basket_id is empty (AC-3):
// stable, no clock / no random, so the same basket shape always yields the same
// handle. The leg count keeps it human-recognizable without echoing any secret.
[[nodiscard]] std::string fallback_basket_id(std::size_t leg_count) {
  return "basket-" + std::to_string(leg_count);
}

// The pre-flight validation + Kahn topological sort, fused: one pass that both
// REJECTS an ill-formed basket (empty/duplicate leg_id, unknown depends_on, any
// cycle) and, when valid, yields the execution order. Returns true with `order`
// filled (indices into `legs`, dependency-respecting) on success; false (basket
// is Blocked) otherwise. Placement has NOT happened yet — failing here means
// NOTHING is ever sent (fail-closed).
[[nodiscard]] bool topological_order(const std::vector<BasketLeg>& legs,
                                     std::vector<std::size_t>& order) {
  const std::size_t n = legs.size();

  // Map leg_id -> index; reject empty and duplicate ids as we go.
  std::unordered_map<std::string, std::size_t> id_to_index;
  id_to_index.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (legs[i].leg_id.empty()) {
      return false;  // empty leg_id -> Blocked
    }
    if (!id_to_index.emplace(legs[i].leg_id, i).second) {
      return false;  // duplicate leg_id -> Blocked
    }
  }

  // Build the dependency graph: an edge prereq -> dependent. in_degree[i] is the
  // number of prerequisites still unsatisfied for leg i. An unknown depends_on
  // id fails closed.
  std::vector<std::size_t> in_degree(n, 0);
  std::vector<std::vector<std::size_t>> dependents(n);
  for (std::size_t i = 0; i < n; ++i) {
    for (const std::string& dep : legs[i].depends_on) {
      const auto it = id_to_index.find(dep);
      if (it == id_to_index.end()) {
        return false;  // depends_on names an unknown leg -> Blocked
      }
      dependents[it->second].push_back(i);
      ++in_degree[i];
    }
  }

  // Kahn's algorithm. Seed roots (in_degree 0) in input order and push newly
  // freed nodes in input order, so the resulting order is fully deterministic.
  order.clear();
  order.reserve(n);
  std::vector<std::size_t> queue;
  queue.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (in_degree[i] == 0) {
      queue.push_back(i);
    }
  }
  for (std::size_t head = 0; head < queue.size(); ++head) {
    const std::size_t u = queue[head];
    order.push_back(u);
    for (const std::size_t v : dependents[u]) {
      if (--in_degree[v] == 0) {
        queue.push_back(v);
      }
    }
  }

  // If Kahn could not order every node, a cycle remains -> Blocked.
  return order.size() == n;
}

}  // namespace

std::string_view to_string(LegStatus status) noexcept {
  switch (status) {
    case LegStatus::Executed:
      return "Executed";
    case LegStatus::Failed:
      return "Failed";
    case LegStatus::SkippedUnmetDependency:
      return "SkippedUnmetDependency";
    case LegStatus::Unwound:
      return "Unwound";
  }
  return "Unknown";
}

std::string_view to_string(BasketOutcome outcome) noexcept {
  switch (outcome) {
    case BasketOutcome::Complete:
      return "Complete";
    case BasketOutcome::PartiallyExecutedUnwound:
      return "PartiallyExecutedUnwound";
    case BasketOutcome::PartiallyExecutedLeft:
      return "PartiallyExecutedLeft";
    case BasketOutcome::Blocked:
      return "Blocked";
  }
  return "Unknown";
}

BasketResult execute_basket(const std::vector<BasketLeg>& legs, const BasketConfig& config,
                            const BasketSeams& seams, ports::AlertSink& alerts) {
  BasketResult result;
  result.tracked_as_single_unit = config.track_as_single_unit;
  result.basket_id =
      config.basket_id.empty() ? fallback_basket_id(legs.size()) : config.basket_id;

  // ── Step 1: PRE-FLIGHT, fail-closed BEFORE any placement ──────────────────
  // A missing placer fails closed exactly like an ill-formed graph: we never
  // partially place a basket with no way to place. Topological validation
  // rejects empty/duplicate leg_id, unknown deps, and ALL cycles.
  std::vector<std::size_t> order;
  if (!seams.place_leg || !topological_order(legs, order)) {
    result.outcome = BasketOutcome::Blocked;
    result.detail = !seams.place_leg
                        ? "basket blocked: place_leg seam not configured (nothing placed)"
                        : "basket blocked: invalid basket "
                          "(empty/duplicate leg_id, unknown dependency, or cycle); nothing placed";
    // Report every leg as skipped: nothing was sent.
    result.legs.reserve(legs.size());
    for (const BasketLeg& leg : legs) {
      result.legs.push_back(
          LegResult{leg.leg_id, LegStatus::SkippedUnmetDependency, "", "basket blocked pre-flight"});
    }
    return result;
  }

  const std::size_t n = legs.size();

  // Map leg_id -> index for prerequisite lookups during execution. (The graph is
  // already validated, so every id resolves.)
  std::unordered_map<std::string, std::size_t> id_to_index;
  id_to_index.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    id_to_index.emplace(legs[i].leg_id, i);
  }

  // Per-leg working state, indexed by leg index.
  std::vector<LegStatus> status(n, LegStatus::SkippedUnmetDependency);
  std::vector<std::string> order_id(n);
  std::vector<std::string> detail(n);

  // ── Step 2: EXECUTE in topological order ──────────────────────────────────
  // A leg is placed ONLY if EVERY prerequisite ended Executed. A failed/skipped
  // prerequisite makes this leg SkippedUnmetDependency WITHOUT calling place_leg
  // (the never-orphan invariant), and the skip propagates to its dependents
  // because they, in turn, see a non-Executed prerequisite.
  for (const std::size_t i : order) {
    bool prerequisites_met = true;
    for (const std::string& dep : legs[i].depends_on) {
      if (status[id_to_index.at(dep)] != LegStatus::Executed) {
        prerequisites_met = false;
        break;
      }
    }
    if (!prerequisites_met) {
      status[i] = LegStatus::SkippedUnmetDependency;
      detail[i] = "skipped: a prerequisite leg was not Executed (never orphaned)";
      continue;  // place_leg is NOT called for a skipped leg
    }

    auto ack = seams.place_leg(legs[i]);
    if (!ack) {
      status[i] = LegStatus::Failed;
      detail[i] = "leg placement failed: " + error_tag(ack.error());
      continue;
    }
    status[i] = LegStatus::Executed;
    order_id[i] = ack.value().broker_order_id;
    detail[i] = "leg executed";
  }

  // ── Step 3: PARTIAL DETECTION + POLICY (AC-2) ─────────────────────────────
  bool any_failed_or_skipped = false;
  for (std::size_t i = 0; i < n; ++i) {
    if (status[i] == LegStatus::Failed || status[i] == LegStatus::SkippedUnmetDependency) {
      any_failed_or_skipped = true;
      break;
    }
  }

  if (!any_failed_or_skipped) {
    // Every leg Executed: the basket is whole. NO alert, NO unwind.
    result.outcome = BasketOutcome::Complete;
    result.detail = "basket complete: all legs executed";
  } else if (config.on_leg_failure == LegFailurePolicy::UnwindExecuted) {
    // Unwind the executed legs NEWEST-FIRST (reverse execution order). A
    // null/Error unwind leaves that leg Executed (NOT Unwound) and escalates in
    // its detail — the failure to unwind a LIVE leg must stay visible.
    std::string live_legs;  // leg_ids that could NOT be unwound and remain LIVE
    for (std::size_t k = order.size(); k-- > 0;) {
      const std::size_t i = order[k];
      if (status[i] != LegStatus::Executed) {
        continue;
      }
      bool unwound = false;
      if (seams.unwind_leg) {
        auto unwind = seams.unwind_leg(order_id[i]);
        if (unwind) {
          unwound = true;
        } else {
          detail[i] = "CRITICAL: unwind failed, leg still LIVE: " + error_tag(unwind.error());
        }
      } else {
        detail[i] = "CRITICAL: unwind seam not configured, leg still LIVE";
      }
      if (unwound) {
        status[i] = LegStatus::Unwound;
        detail[i] = "leg unwound during remediation";
      } else {
        // Stays Executed; escalated above. Record the leg_id (non-secret) so the
        // loudest channel — the Critical alert — names the LIVE orphan, not just
        // the per-leg detail an operator might not open.
        if (!live_legs.empty()) {
          live_legs += ",";
        }
        live_legs += legs[i].leg_id;
      }
    }
    const bool any_unwind_failed = !live_legs.empty();
    result.outcome = BasketOutcome::PartiallyExecutedUnwound;
    result.detail = any_unwind_failed
                        ? "basket partial: unwind attempted; legs still LIVE: " + live_legs
                        : "basket partial: executed legs unwound per policy";
    // Best-effort Critical alert AFTER the unwinds. Result SWALLOWED + wrapped so
    // a dead/THROWING sink cannot derail the (already-run) remediation or break
    // the no-throw contract (mirrors 5.1's emergency-before-alert discipline).
    const std::string alert_msg =
        any_unwind_failed
            ? "BASKET PARTIAL: a leg failed and one or more executed legs could NOT be "
              "unwound and remain LIVE: " +
                  live_legs
            : "BASKET PARTIAL: a leg failed; all executed legs unwound per policy";
    try {
      (void)alerts.send(ports::AlertLevel::Critical, alert_msg);
    } catch (...) {  // NOLINT(bugprone-empty-catch): alerting is strictly best-effort
    }
  } else {
    // LeaveAndAlert: do NOT touch unwind_leg. Leave executed legs in place and
    // raise a Warning so the operator knows the basket is partial.
    result.outcome = BasketOutcome::PartiallyExecutedLeft;
    result.detail = "basket partial: executed legs left in place per policy; operator alerted";
    try {
      (void)alerts.send(ports::AlertLevel::Warning,
                        "BASKET PARTIAL: a leg failed; executed legs LEFT in place per policy");
    } catch (...) {  // NOLINT(bugprone-empty-catch): alerting is strictly best-effort
    }
  }

  // Emit per-leg results in INPUT order (stable for callers/audit).
  result.legs.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    result.legs.push_back(LegResult{legs[i].leg_id, status[i], order_id[i], detail[i]});
  }
  return result;
}

}  // namespace broker_exec::options
