#include "broker_exec/observability/reports.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

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

  const std::vector<AuditEvent> order1 = {placed_a, ack_a,     filled_a, recon_a,
                                          placed_b, rejected_b, placed_c};
  const std::vector<AuditEvent> order2 = {placed_c, placed_b, rejected_b, recon_a,
                                          filled_a, ack_a,    placed_a};

  CHECK(ReportGenerator::daily(order1).to_json() == ReportGenerator::daily(order2).to_json());
  CHECK(ReportGenerator::reconciliation(order1).to_json() ==
        ReportGenerator::reconciliation(order2).to_json());
}
