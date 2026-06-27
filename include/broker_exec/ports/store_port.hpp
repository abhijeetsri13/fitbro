#pragma once

// broker_exec::ports::StorePort — the abstract durable record store.
//
// The persistence seam for the write-ahead intent log / projection (Stories
// 1.5/1.6). The core appends opaque records (already-serialized strings — one
// JSON line per the WAL contract), replays them head-to-tail on boot to rebuild
// in-memory state, and looks records up by client_ref for idempotency. This
// story freezes the abstraction only; the concrete fsync'd/hash-chained impl
// and the SQLite projection land in 1.5/1.6.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`. (The
// concrete impl reaches durability via `broker_exec::platform::durable_sync`,
// never raw fsync — but that is below this seam.)

#include <functional>
#include <optional>
#include <string>

#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::ports {

// Abstract append-only record store with replay and client_ref lookup. Records
// are opaque to the port (serialization is the caller's concern); the store
// only persists, orders, and returns them. Header-only, pure-virtual.
class StorePort {
 public:
  virtual ~StorePort() = default;

  // Durably append one record. On success the record is persisted (the concrete
  // impl is responsible for fsync-before-acknowledge per the WAL contract).
  [[nodiscard]] virtual Result<Ok> append(const std::string& record) = 0;

  // Replay every persisted record in append order, invoking `on_record` for
  // each. Used on boot to rebuild the in-memory client-ref index and enumerate
  // every order that might have been sent. Stops and surfaces an Error if the
  // backing log is unreadable or its integrity check fails.
  [[nodiscard]] virtual Result<Ok> replay_all(
      const std::function<void(const std::string& record)>& on_record) = 0;

  // Look up the most recent record for a client_ref, if any. A present value is
  // the duplicate-detection signal for idempotency (Story 1.7); `std::nullopt`
  // means "never seen". An Error is reserved for a store failure, not absence.
  [[nodiscard]] virtual Result<std::optional<std::string>> find_by_client_ref(
      const std::string& client_ref) const = 0;
};

}  // namespace broker_exec::ports
