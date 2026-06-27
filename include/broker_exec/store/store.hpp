#pragma once

// broker_exec::store — the SQLite read-model projection (Story 1.6, FR-7 backstop,
// NFR-4 schema versioning).
//
// WHAT THIS IS: a queryable, crash-safe SQLite projection of the durable state
// the engine derives from the write-ahead intent log (Story 1.5). It is NOT the
// source of truth — the intent log is. The projection exists so state queries
// (find an order by client_ref, list open positions, ...) are cheap and do not
// require replaying the log. Because it is a derived view, a corrupt or
// half-migrated projection is *recoverable*: we drop and rebuild it from the
// intent log rather than refuse to start (see open_or_rebuild / reset).
//
// SCHEMA VERSIONING (NFR-4): every database carries a single `schema_version`
// row. Numbered migrations apply transactionally, forward-only:
//   * a database at an OLDER readable version is migrated forward on open;
//   * a database at an UNKNOWN/NEWER version refuses to start with a typed
//     Error (DoNotRetry) — we never silently run against a schema we predate.
//
// DURABILITY: opened with WAL journaling and `synchronous=FULL` so a crash
// cannot leave a torn page (the projection is still rebuildable, but we make
// the common path durable anyway).
//
// CROSS-PLATFORM: SQLite C API + C++20 stdlib only. Paths flow through
// std::filesystem::path; the on-disk filename is taken as UTF-8 via
// path.string(). No OS APIs, no `#ifdef`. ":memory:" is supported for fast
// in-process tests (it skips WAL, which an in-memory db does not support).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "broker_exec/domain/money.hpp"
#include "broker_exec/domain/types.hpp"
#include "broker_exec/result.hpp"

// Forward-declare the opaque SQLite handle so this public header does not pull
// <sqlite3.h> into every consumer (SQLite3 is a PRIVATE link dependency).
struct sqlite3;

namespace broker_exec::store {

// The void-success type. `Result<Ok>` means "succeeded with no value, or a typed
// Error". A distinct empty struct (rather than std::monostate) so the success
// intent reads clearly at call sites: `return Ok{};`.
struct Ok {};

// ── Minimal row types for tables the domain layer does not model ───────────
// The domain (Story 1.2) models Order/Trade/Position; funds, risk events, and
// the audit trail are projection-local, so we define small value rows here.

// A point-in-time funds / margin snapshot. `fetched_at_epoch_ms` is the wall
// clock at which the broker view was taken (the freshness stamp the fail-closed
// funds gate keys on, Story 2.11). Money is exact integer paise (no float).
struct Funds {
  std::string account;                 // Owning account id (one row per account).
  domain::Money available;             // Free cash available to trade.
  domain::Money used_margin;           // Margin currently blocked.
  std::int64_t fetched_at_epoch_ms{0};  // Wall-clock stamp of the broker view.

  [[nodiscard]] bool operator==(const Funds&) const = default;
};

// A risk-engine decision worth persisting for provenance (Story 2.10 / FR-15).
// `rule` is the named rule that fired; `detail` is a short, redaction-safe note.
struct RiskEvent {
  std::int64_t id{0};            // Assigned by the store on insert (0 = unset).
  std::string client_ref;        // Order the event relates to (may be empty).
  std::string rule;              // Named risk rule, e.g. "max_lots".
  std::string detail;            // Short human-readable note (log-safe).
  std::int64_t at_epoch_ms{0};   // Wall-clock stamp.

  [[nodiscard]] bool operator==(const RiskEvent&) const = default;
};

// A generic audit record (Story 4.1 / FR-27). The projection only needs to store
// and read these back; structured interpretation happens in the observability
// layer. `payload` is an already-redaction-safe string (e.g. a JSON line).
struct AuditRecord {
  std::int64_t id{0};            // Assigned by the store on insert (0 = unset).
  std::int64_t seq{0};           // Monotonic provenance sequence (intent-log seq).
  std::string client_ref;        // Related order, if any.
  std::string event;             // Event kind, e.g. "ORDER_SENT".
  std::string payload;           // Redaction-safe detail blob.
  std::int64_t at_epoch_ms{0};   // Wall-clock stamp.

  [[nodiscard]] bool operator==(const AuditRecord&) const = default;
};

// The queryable SQLite projection. Move-only (it owns a sqlite3 connection).
//
// Consumed by Story 1.7 (idempotency: insert_order/find_order + UNIQUE(client_ref))
// and the runtime rebuild path (Story 1.7+: replay the intent log into a freshly
// reset() store). Every fallible call returns Result<...>; nothing throws across
// the boundary (conventions.md "Errors").
class Store {
 public:
  // ── Lifecycle ────────────────────────────────────────────────────────────

  // Open the projection at `path`, applying any pending forward migrations.
  //   * a NEWER/unknown schema_version -> Error (category Internal, DoNotRetry):
  //     refuse to start rather than run against a schema we predate (NFR-4);
  //   * corruption / a half-applied migration -> Error. Callers that can rebuild
  //     from the intent log should prefer open_or_rebuild() instead.
  // Use ":memory:" or a temp file path in tests.
  [[nodiscard]] static Result<Store> open(std::filesystem::path path);

  // Open like open(), but treat corruption / a half-migrated projection as
  // RECOVERABLE: reset() to an empty current-schema database and report
  // needs_rebuild=true so the caller re-applies the intent log. A NEWER/unknown
  // schema_version is still a hard refuse-to-start Error (that is an operator/
  // deployment fault, not corruption — see NFR-4).
  //
  // OpenOutcome is declared here and DEFINED out-of-line below the class: it has
  // a by-value `Store store` member, which requires the (enclosing) Store type to
  // be complete — so its definition must follow the class. The member-function
  // declaration only needs the type to be declared.
  struct OpenOutcome;
  [[nodiscard]] static Result<OpenOutcome> open_or_rebuild(std::filesystem::path path);

  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  Store(Store&&) noexcept;
  Store& operator=(Store&&) noexcept;
  ~Store();

  // The applied schema version (the highest migration that has run).
  [[nodiscard]] int schema_version() const noexcept;

  // ── Orders (UNIQUE(client_ref)) ──────────────────────────────────────────

  // Insert a new order. Fails with category DuplicateOrder if its client_ref
  // already exists (the UNIQUE(client_ref) idempotency backstop, Story 1.7).
  [[nodiscard]] Result<Ok> insert_order(const domain::Order& order);

  // Insert-or-update by client_ref. Used by the lifecycle FSM to project the
  // latest known state of an order it already recorded.
  [[nodiscard]] Result<Ok> upsert_order(const domain::Order& order);

  // Look up an order by its client_ref. std::nullopt if none exists.
  [[nodiscard]] Result<std::optional<domain::Order>> find_order(
      std::string_view client_ref) const;

  // All orders, ordered by client_ref for a stable, test-friendly result.
  [[nodiscard]] Result<std::vector<domain::Order>> all_orders() const;

  // ── Trades / positions ───────────────────────────────────────────────────

  // Record an execution. Idempotent on trade_id (re-inserting the same trade is
  // a no-op, so a reconcile replay does not double-count fills).
  [[nodiscard]] Result<Ok> insert_trade(const domain::Trade& trade);
  [[nodiscard]] Result<std::vector<domain::Trade>> all_trades() const;

  // Insert-or-update the net position for a symbol (one row per symbol).
  [[nodiscard]] Result<Ok> upsert_position(const domain::Position& position);
  [[nodiscard]] Result<std::optional<domain::Position>> find_position(
      std::string_view symbol) const;
  [[nodiscard]] Result<std::vector<domain::Position>> all_positions() const;

  // ── Funds / risk events / audit (projection-local rows) ──────────────────

  // Insert-or-update the funds snapshot for an account (one row per account).
  [[nodiscard]] Result<Ok> upsert_funds(const Funds& funds);
  [[nodiscard]] Result<std::optional<Funds>> find_funds(std::string_view account) const;

  // Append a risk event. Ignores `event.id` (the store assigns the rowid).
  [[nodiscard]] Result<Ok> insert_risk_event(const RiskEvent& event);
  [[nodiscard]] Result<std::vector<RiskEvent>> all_risk_events() const;

  // Append an audit record. Ignores `record.id` (the store assigns the rowid).
  [[nodiscard]] Result<Ok> insert_audit(const AuditRecord& record);
  [[nodiscard]] Result<std::vector<AuditRecord>> all_audit() const;

  // ── Rebuild entry point ──────────────────────────────────────────────────

  // Drop every table and re-create the schema at the current version. This is
  // the rebuild-from-intent-log entry point: a caller resets, then re-applies
  // the intent-log records (the replay->apply composition is Story 1.7/runtime).
  [[nodiscard]] Result<Ok> reset();

 private:
  // RAII deleter that closes the sqlite3 connection. Defined in the .cpp so the
  // public header never includes <sqlite3.h>.
  struct ConnectionDeleter {
    void operator()(sqlite3* db) const noexcept;
  };
  using Connection = std::unique_ptr<sqlite3, ConnectionDeleter>;

  // Open the raw connection + apply connection PRAGMAs. Private static so it can
  // name the private Connection type; shared by open()/open_or_rebuild()/reset().
  [[nodiscard]] static Result<Connection> open_connection(const std::filesystem::path& path);

  Store(Connection db, int version) noexcept;

  Connection db_;
  int schema_version_{0};
};

// Out-of-line definition of the nested OpenOutcome (Store is now complete, so the
// by-value `store` member is well-formed). The shape is the binding contract
// Story 1.7 consumes: `{ Store store; bool needs_rebuild; }`.
struct Store::OpenOutcome {
  Store store;                // the opened (and possibly rebuilt) projection.
  bool needs_rebuild{false};  // true => caller must replay the intent log.
};

}  // namespace broker_exec::store
