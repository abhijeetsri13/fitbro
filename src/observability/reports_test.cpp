#include "broker_exec/observability/reports.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "broker_exec/observability/audit_event.hpp"

using broker_exec::observability::AuditEvent;
using broker_exec::observability::DailyReport;
using broker_exec::observability::ErrorReport;
using broker_exec::observability::EventType;
using broker_exec::observability::ReconciliationReport;
using broker_exec::observability::ReportGenerator;
using json = nlohmann::json;

namespace {

// Build an AuditEvent with the columns the reports actually read.
[[nodiscard]] AuditEvent make_event(EventType type, std::string client_ref = {},
                                    std::string strategy = {}) {
  AuditEvent ev;
  ev.type = type;
  ev.client_ref = std::move(client_ref);
  ev.strategy = std::move(strategy);
  return ev;
}

}  // namespace

TEST_CASE("daily report counts events and sums integer P&L (AC-1)") {
  std::vector<AuditEvent> events;
  events.push_back(make_event(EventType::OrderPlaced, "a", "alpha"));
  events.push_back(make_event(EventType::OrderPlaced, "b", "alpha"));
  events.push_back(make_event(EventType::OrderPlaced, "c", "beta"));

  AuditEvent fill1 = make_event(EventType::OrderFilled, "a");
  fill1.fields["pnl"] = 10000;  // integer paise
  events.push_back(fill1);

  AuditEvent fill2 = make_event(EventType::OrderFilled, "b");
  fill2.fields["pnl"] = 5000;  // integer paise
  events.push_back(fill2);

  events.push_back(make_event(EventType::OrderRejected, "c"));

  // A non-integer (float) pnl must be IGNORED — never coerced to a double.
  AuditEvent floaty = make_event(EventType::OrderFilled, "d");
  floaty.fields["pnl"] = 1.5;
  events.push_back(floaty);

  const DailyReport report = ReportGenerator::daily(events);

  CHECK(report.orders_placed == 3);
  CHECK(report.filled == 3);  // two integer-pnl fills + the float-pnl fill
  CHECK(report.rejected == 1);
  CHECK(report.cancelled == 0);
  CHECK(report.unknown == 0);
  // 10000 + 5000 only — the 1.5 float pnl is ignored, not added.
  CHECK(report.realized_pnl_paise == 15000);
  REQUIRE(report.orders_per_strategy.size() == 2);
  CHECK(report.orders_per_strategy.at("alpha") == 2);
  CHECK(report.orders_per_strategy.at("beta") == 1);
}

TEST_CASE("error report carries only Error events, in input order (AC-1)") {
  std::vector<AuditEvent> events;
  events.push_back(make_event(EventType::OrderPlaced, "a"));

  AuditEvent err1 = make_event(EventType::Error, "a");
  err1.fields["message"] = "risk breach";
  events.push_back(err1);

  events.push_back(make_event(EventType::OrderFilled, "a"));

  AuditEvent err2 = make_event(EventType::Error, "b");
  err2.fields["message"] = "broker timeout";
  events.push_back(err2);

  events.push_back(make_event(EventType::OrderAcknowledged, "b"));

  const ErrorReport report = ReportGenerator::errors(events);

  REQUIRE(report.errors.size() == 2);
  CHECK(report.errors[0].client_ref == "a");
  CHECK(report.errors[0].detail == "risk breach");
  CHECK(report.errors[1].client_ref == "b");
  CHECK(report.errors[1].detail == "broker timeout");
}

TEST_CASE("reconciliation states intended/sent/confirmed/reconciled + discrepancies (AC-2)") {
  std::vector<AuditEvent> events;
  // Order "a": full happy path -> no discrepancy.
  events.push_back(make_event(EventType::OrderPlaced, "a"));
  events.push_back(make_event(EventType::OrderAcknowledged, "a"));
  events.push_back(make_event(EventType::OrderFilled, "a"));
  events.push_back(make_event(EventType::ReconcileResult, "a"));

  // Order "b": placed but never confirmed -> "intended but not confirmed".
  events.push_back(make_event(EventType::OrderPlaced, "b"));

  // Order "c": filled but not reconciled -> "filled but not reconciled".
  events.push_back(make_event(EventType::OrderPlaced, "c"));
  events.push_back(make_event(EventType::OrderFilled, "c"));

  const ReconciliationReport report = ReportGenerator::reconciliation(events);

  REQUIRE(report.orders.size() == 3);
  // Orders are sorted by client_ref: a, b, c.
  CHECK(report.orders[0].client_ref == "a");
  CHECK(report.orders[1].client_ref == "b");
  CHECK(report.orders[2].client_ref == "c");

  const auto& a = report.orders[0];
  CHECK(a.intended);
  CHECK(a.sent);
  CHECK(a.confirmed);
  CHECK(a.reconciled);
  CHECK(a.final_state == "order.filled");

  const auto& b = report.orders[1];
  CHECK(b.intended);
  CHECK(b.sent);
  CHECK_FALSE(b.confirmed);
  CHECK_FALSE(b.reconciled);
  CHECK(b.final_state == "open");

  const auto& c = report.orders[2];
  CHECK(c.confirmed);
  CHECK_FALSE(c.reconciled);
  CHECK(c.final_state == "order.filled");

  CHECK(report.intended == 3);
  CHECK(report.sent == 3);
  CHECK(report.confirmed == 2);
  CHECK(report.reconciled == 1);

  REQUIRE(report.discrepancies.size() == 2);
  CHECK(report.discrepancies[0] == "client_ref b: intended but not confirmed");
  CHECK(report.discrepancies[1] == "client_ref c: filled but not reconciled");
}

TEST_CASE("reports are reproducible — byte-identical to_json on the same input (AC-3)") {
  std::vector<AuditEvent> events;
  events.push_back(make_event(EventType::OrderPlaced, "c", "beta"));
  events.push_back(make_event(EventType::OrderPlaced, "a", "alpha"));
  events.push_back(make_event(EventType::OrderAcknowledged, "a"));
  events.push_back(make_event(EventType::OrderFilled, "a"));
  events.push_back(make_event(EventType::ReconcileResult, "a"));

  AuditEvent err = make_event(EventType::Error, "c");
  err.fields["message"] = "rejected";
  events.push_back(err);

  CHECK(ReportGenerator::daily(events).to_json() == ReportGenerator::daily(events).to_json());
  CHECK(ReportGenerator::errors(events).to_json() == ReportGenerator::errors(events).to_json());
  CHECK(ReportGenerator::reconciliation(events).to_json() ==
        ReportGenerator::reconciliation(events).to_json());
}

TEST_CASE("each report renders parseable JSON with the expected keys (AC-3)") {
  std::vector<AuditEvent> events;
  events.push_back(make_event(EventType::OrderPlaced, "a", "alpha"));
  AuditEvent fill = make_event(EventType::OrderFilled, "a");
  fill.fields["pnl"] = 42000;
  events.push_back(fill);
  AuditEvent err = make_event(EventType::Error, "a");
  err.fields["message"] = "boom";
  events.push_back(err);

  const DailyReport daily = ReportGenerator::daily(events);
  const json daily_json = json::parse(daily.to_json(), nullptr, /*allow_exceptions=*/false);
  REQUIRE_FALSE(daily_json.is_discarded());
  CHECK(daily_json.at("orders_placed").get<int>() == 1);
  CHECK(daily_json.at("realized_pnl_paise").get<long long>() == 42000);
  CHECK(daily_json.at("realized_pnl_paise").is_number_integer());  // never a float

  const ErrorReport errors = ReportGenerator::errors(events);
  const json error_json = json::parse(errors.to_json(), nullptr, /*allow_exceptions=*/false);
  REQUIRE_FALSE(error_json.is_discarded());
  REQUIRE(error_json.at("errors").is_array());
  CHECK(error_json.at("errors").size() == 1);

  const ReconciliationReport recon = ReportGenerator::reconciliation(events);
  const json recon_json = json::parse(recon.to_json(), nullptr, /*allow_exceptions=*/false);
  REQUIRE_FALSE(recon_json.is_discarded());
  CHECK(recon_json.at("intended").get<int>() == 1);
  REQUIRE(recon_json.at("discrepancies").is_array());
}

TEST_CASE("error report detail is scrubbed — no token-shaped secret leaks (Fix 1)") {
  // The in-memory AuditEvent.fields are RAW: StructuredLogger (Story 4.1) scrubs
  // only the rendered LOG LINE, not the in-memory event. So a token-shaped value
  // in `fields` must be scrubbed by the report itself (domain::scrub). A 32-char
  // alnum synthetic token (mixes letters+digits -> the bare high-entropy rule).
  const std::string token = "ABCDEF0123456789ABCDEF0123456789";  // 32-char alnum

  std::vector<AuditEvent> events;
  // (1) the "message" string path.
  AuditEvent err1 = make_event(EventType::Error, "a");
  err1.fields["message"] = "auth failed " + token;
  events.push_back(err1);
  // (2) the compact-dump path (no "message" key — a token in another field).
  AuditEvent err2 = make_event(EventType::Error, "b");
  err2.fields["api_response"] = token;
  events.push_back(err2);

  const ErrorReport report = ReportGenerator::errors(events);
  REQUIRE(report.errors.size() == 2);
  // The token never survives into the stored detail (either path).
  CHECK(report.errors[0].detail.find(token) == std::string::npos);
  CHECK(report.errors[1].detail.find(token) == std::string::npos);

  const std::string rendered = report.to_json();
  // ZERO occurrences of the token anywhere in the rendered (persisted) report.
  CHECK(rendered.find(token) == std::string::npos);
  // ...and the redacted report still parses as JSON.
  const json parsed = json::parse(rendered, nullptr, /*allow_exceptions=*/false);
  REQUIRE_FALSE(parsed.is_discarded());
}

TEST_CASE("reconciliation treats a broker reject/cancel as a confirmed verdict (Fix 2)") {
  std::vector<AuditEvent> events;
  // "a": placed + rejected -> a CONFIRMED broker verdict, NOT a silent gap.
  events.push_back(make_event(EventType::OrderPlaced, "a"));
  events.push_back(make_event(EventType::OrderRejected, "a"));
  // "b": placed only, NO broker verdict -> the dangerous silent/UNKNOWN gap.
  events.push_back(make_event(EventType::OrderPlaced, "b"));
  // "c": placed + filled, NO reconcile -> "filled but not reconciled".
  events.push_back(make_event(EventType::OrderPlaced, "c"));
  events.push_back(make_event(EventType::OrderFilled, "c"));
  // "d": placed + filled + reconcile -> clean, no discrepancy.
  events.push_back(make_event(EventType::OrderPlaced, "d"));
  events.push_back(make_event(EventType::OrderFilled, "d"));
  events.push_back(make_event(EventType::ReconcileResult, "d"));

  const ReconciliationReport report = ReportGenerator::reconciliation(events);

  REQUIRE(report.orders.size() == 4);
  const auto& a = report.orders[0];  // sorted by client_ref
  CHECK(a.client_ref == "a");
  CHECK(a.intended);
  CHECK(a.sent);
  CHECK(a.confirmed);  // a reject IS the broker confirming the outcome
  CHECK_FALSE(a.reconciled);
  CHECK(a.final_state == "order.rejected");

  // Discrepancies, in client_ref order: ONLY b (silent) and c (filled, no recon).
  REQUIRE(report.discrepancies.size() == 2);
  CHECK(report.discrepancies[0] == "client_ref b: intended but not confirmed");
  CHECK(report.discrepancies[1] == "client_ref c: filled but not reconciled");

  // The placed+rejected order "a" produces NO discrepancy at all; nor does the
  // clean placed+filled+reconcile order "d".
  for (const std::string& d : report.discrepancies) {
    CHECK(d.find("client_ref a:") == std::string::npos);
    CHECK(d.find("client_ref d:") == std::string::npos);
  }
}

// ── IMP-15: the reports' own provenance columns stay legible ─────────────────

TEST_CASE("a real client_ref survives every report VERBATIM (IMP-15)") {
  // EXACTLY the shape make_client_ref mints — `<strategy>-<sig8>-<uuid>` with the
  // canonical 8-4-4-4-12 uuid text (uuid.cpp format_uuid_v4) — plus an IMP-13
  // exit ref and a slicer child.
  const std::string ref = "alpha-1a2b3c4d-deadbeef-cafe-4bab-8abe-0123456789ab";
  const std::string exit_ref = ref + "#X";
  const std::string child_ref = ref + "#2";

  std::vector<AuditEvent> events;
  events.push_back(make_event(EventType::OrderPlaced, ref, "alpha"));
  events.push_back(make_event(EventType::OrderFilled, ref));  // filled, never reconciled
  events.push_back(make_event(EventType::OrderPlaced, exit_ref, "alpha"));  // never confirmed
  events.push_back(make_event(EventType::OrderPlaced, child_ref, "alpha"));
  events.push_back(make_event(EventType::OrderAcknowledged, child_ref));
  AuditEvent err = make_event(EventType::Error, ref);
  err.fields["message"] = "broker rejected the exit";
  events.push_back(err);

  // (1) The error report names the order it is about.
  const ErrorReport errors = ReportGenerator::errors(events);
  REQUIRE(errors.errors.size() == 1);
  CHECK(errors.errors[0].client_ref == ref);
  CHECK(errors.to_json().find(ref) != std::string::npos);

  // (2) So does every reconciliation row...
  const ReconciliationReport recon = ReportGenerator::reconciliation(events);
  REQUIRE(recon.orders.size() == 3);
  const json parsed = json::parse(recon.to_json(), nullptr, /*allow_exceptions=*/false);
  REQUIRE_FALSE(parsed.is_discarded());
  std::vector<std::string> rendered_refs;
  for (const auto& row : parsed.at("orders")) {
    rendered_refs.push_back(row.at("client_ref").get<std::string>());
  }
  // Sorted by client_ref: the parent, then "#2", then "#X".
  REQUIRE(rendered_refs.size() == 3);
  CHECK(rendered_refs[0] == ref);
  CHECK(rendered_refs[1] == child_ref);
  CHECK(rendered_refs[2] == exit_ref);

  // (3) ...and every discrepancy line NAMES the order, which is its entire job.
  REQUIRE(recon.discrepancies.size() == 2);
  CHECK(recon.discrepancies[0] == "client_ref " + ref + ": filled but not reconciled");
  CHECK(recon.discrepancies[1] == "client_ref " + exit_ref + ": intended but not confirmed");
  CHECK(recon.to_json().find("REDACTED") == std::string::npos);
}

TEST_CASE("a token-shaped value in the client_ref column IS redacted in a report (IMP-15)") {
  // The reports persist/serve their output, so an anomalous value in the id
  // column fails closed — it used to be copied through raw.
  const std::string token = "ABCDEF0123456789ABCDEF0123456789";  // 32-char alnum, no structure

  std::vector<AuditEvent> events;
  events.push_back(make_event(EventType::OrderPlaced, token));
  AuditEvent err = make_event(EventType::Error, token);
  err.fields["message"] = "boom";
  events.push_back(err);

  const ErrorReport errors = ReportGenerator::errors(events);
  REQUIRE(errors.errors.size() == 1);
  CHECK(errors.errors[0].client_ref.find(token) == std::string::npos);
  CHECK(errors.to_json().find(token) == std::string::npos);

  const ReconciliationReport recon = ReportGenerator::reconciliation(events);
  CHECK(recon.to_json().find(token) == std::string::npos);
  REQUIRE(recon.discrepancies.size() == 1);
  CHECK(recon.discrepancies[0].find(token) == std::string::npos);
}

TEST_CASE("two distinct anomalous refs stay two rows: grouping keys on the RAW ref (IMP-15)") {
  // Sanitizing happens at OUTPUT only. If it happened before grouping, these two
  // would both become the marker and collapse into a single, wrong row.
  const std::string token_a = "AAAAAA0123456789AAAAAA0123456789";
  const std::string token_b = "BBBBBB0123456789BBBBBB0123456789";

  std::vector<AuditEvent> events;
  events.push_back(make_event(EventType::OrderPlaced, token_a));
  events.push_back(make_event(EventType::OrderPlaced, token_b));
  events.push_back(make_event(EventType::OrderFilled, token_b));

  const ReconciliationReport recon = ReportGenerator::reconciliation(events);
  CHECK(recon.orders.size() == 2);  // still two orders, not one merged row
  CHECK(recon.intended == 2);
  CHECK(recon.sent == 2);
  CHECK(recon.to_json().find(token_a) == std::string::npos);
  CHECK(recon.to_json().find(token_b) == std::string::npos);
}

TEST_CASE("the daily report's per-strategy KEYS are sanitized at OUTPUT (IMP-15)") {
  // `strategy` is a typed column a caller fills, and this report is persisted, so
  // a credential parked in it used to be emitted verbatim AS A JSON KEY.
  const std::string secret = "abcd1234EFGH5678ijkl9012MNOP3456";
  const std::string pasted = "access_token=" + secret;

  std::vector<AuditEvent> events;
  events.push_back(make_event(EventType::OrderPlaced, "a", "alpha"));
  events.push_back(make_event(EventType::OrderPlaced, "b", "alpha"));
  events.push_back(make_event(EventType::OrderPlaced, "c", pasted));

  const DailyReport report = ReportGenerator::daily(events);

  // Grouping still keys on the RAW strategy — sanitizing before the count could
  // merge two distinct names into one row.
  REQUIRE(report.orders_per_strategy.size() == 2);
  CHECK(report.orders_per_strategy.at(pasted) == 1);

  const std::string rendered = report.to_json();
  // ZERO occurrences of the credential anywhere in the persisted report.
  CHECK(rendered.find(secret) == std::string::npos);

  const json parsed = json::parse(rendered, nullptr, /*allow_exceptions=*/false);
  REQUIRE_FALSE(parsed.is_discarded());
  const json& per = parsed.at("orders_per_strategy");
  CHECK(per.at("alpha").get<int>() == 2);  // an ordinary name is untouched
  CHECK(per.contains("access_token=***REDACTED***"));
  CHECK(per.at("access_token=***REDACTED***").get<int>() == 1);
}

TEST_CASE("two strategies that sanitize to the SAME key have their counts summed (IMP-15)") {
  // The reason the old code gave for skipping this column ("sanitizing a key
  // could merge rows") is real — so it is handled, not used as an excuse: group
  // on RAW, sanitize at output, and += on collision so nothing is under-counted.
  const std::string token_a = "AAAAAA0123456789AAAAAA0123456789";
  const std::string token_b = "BBBBBB0123456789BBBBBB0123456789";

  std::vector<AuditEvent> events;
  events.push_back(make_event(EventType::OrderPlaced, "a", token_a));
  events.push_back(make_event(EventType::OrderPlaced, "b", token_a));
  events.push_back(make_event(EventType::OrderPlaced, "c", token_b));

  const DailyReport report = ReportGenerator::daily(events);
  CHECK(report.orders_placed == 3);
  REQUIRE(report.orders_per_strategy.size() == 2);  // two RAW rows, still distinct

  const std::string rendered = report.to_json();
  CHECK(rendered.find(token_a) == std::string::npos);
  CHECK(rendered.find(token_b) == std::string::npos);

  const json parsed = json::parse(rendered, nullptr, /*allow_exceptions=*/false);
  REQUIRE_FALSE(parsed.is_discarded());
  const json& per = parsed.at("orders_per_strategy");
  // Both sanitize to the marker: ONE key carrying the FULL count, not 2 or a
  // silently overwritten 1.
  REQUIRE(per.size() == 1);
  CHECK(per.at("***REDACTED***").get<int>() == 3);

  // Still deterministic under the remapping.
  CHECK(ReportGenerator::daily(events).to_json() == rendered);
}

TEST_CASE("a real strategy name survives the daily report VERBATIM (IMP-15)") {
  std::vector<AuditEvent> events;
  events.push_back(make_event(EventType::OrderPlaced, "a", "alpha"));
  events.push_back(make_event(EventType::OrderPlaced, "b", "momentum_v2"));
  events.push_back(make_event(EventType::OrderPlaced, "c", "alpha-beta"));

  const json parsed =
      json::parse(ReportGenerator::daily(events).to_json(), nullptr, /*allow_exceptions=*/false);
  REQUIRE_FALSE(parsed.is_discarded());
  const json& per = parsed.at("orders_per_strategy");
  CHECK(per.at("alpha").get<int>() == 1);
  CHECK(per.at("momentum_v2").get<int>() == 1);
  CHECK(per.at("alpha-beta").get<int>() == 1);
}

TEST_CASE("reports are byte-identical across permuted input orderings (AC-3)") {
  // Two DIFFERENT orderings of the SAME events must yield byte-identical daily()
  // and reconciliation() output — proving the sorted/std::map determinism, not
  // merely same-input-twice.
  AuditEvent placed_a = make_event(EventType::OrderPlaced, "a", "alpha");
  AuditEvent ack_a = make_event(EventType::OrderAcknowledged, "a");
  AuditEvent filled_a = make_event(EventType::OrderFilled, "a");
  filled_a.fields["pnl"] = 7000;
  AuditEvent recon_a = make_event(EventType::ReconcileResult, "a");
  AuditEvent placed_b = make_event(EventType::OrderPlaced, "b", "beta");
  AuditEvent rejected_b = make_event(EventType::OrderRejected, "b");
  AuditEvent placed_c = make_event(EventType::OrderPlaced, "c", "alpha");

  const std::vector<AuditEvent> order1 = {placed_a, ack_a,      filled_a, recon_a,
                                          placed_b, rejected_b, placed_c};
  const std::vector<AuditEvent> order2 = {placed_c, placed_b, rejected_b, recon_a,
                                          filled_a, ack_a,    placed_a};

  CHECK(ReportGenerator::daily(order1).to_json() == ReportGenerator::daily(order2).to_json());
  CHECK(ReportGenerator::reconciliation(order1).to_json() ==
        ReportGenerator::reconciliation(order2).to_json());
}
