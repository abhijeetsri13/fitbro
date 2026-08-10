#pragma once

// broker_exec::marketdata — tradability-aware market-data view (Story 3.5,
// FR-19, IBR-3).
//
// A strategy must be able to ask "is this price TRADABLE?" — not merely "what is
// the last price?". This view ingests ticks through an injected seam
// (`on_tick`/`on_connect`/`on_disconnect`) and classifies each symbol into an
// explicit `MarketDataState` (Unknown/Live/Stale/Delayed/Disconnected). Only a
// `Live` symbol is tradable; `require_tradable()` is the price-sensitive-entry
// gate that fails closed with a `DataStale` Error (BlockStrategy) otherwise
// (AC-2).
//
// Injected seam, NO websocket dependency — the tradability LOGIC lives here; the
// real IXWebSocket transport is a thin adapter behind this seam (a follow-up,
// like the cpr transport behind the kite HttpClient seam). The configured REST
// fallback is likewise just another source that calls `on_tick(...)` when the WS
// is not Live. This module depends inward only: domain (Price), ports
// (ClockPort, AlertSink), errors.
//
// Two clocks, never interchanged (read through the injected ClockPort):
//   * staleness  — measured on the MONOTONIC clock (now_steady - received_at):
//     "we stopped receiving ticks". Immune to wall-clock jumps.
//   * delay      — measured on the WALL clock (now_wall - exchange_ts): "the feed
//     is lagging the exchange". This is an exchange-vs-local comparison and so
//     must use wall time.
//
// Auth-aware reconnect (AC-3) — a reconnect that fails on AUTH routes to session
// re-establishment (a `SessionExpired` Error, action ReEstablishSession) plus an
// operator alert; it does NOT spin an endless socket-retry loop. A transport
// failure stays disconnected and returns ok() (the transport retries with
// bounded backoff — admission/retry is the transport's concern, not this view's).
//
// Conventions: no double/float (Price is integer paise; durations are integer
// std::chrono); Result<T> is no-throw; no OS APIs, no `#ifdef`. NOT thread-safe:
// it lives on, and is driven by, the single main loop.

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "broker_exec/domain/money.hpp"
#include "broker_exec/ports/alert_sink.hpp"
#include "broker_exec/ports/clock_port.hpp"
#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::marketdata {

// Explicit tradability classification for a symbol's market data. Stable,
// log-friendly names (see to_string). Only `Live` is tradable (AC-2).
enum class MarketDataState {
  Unknown,       // no tick ever seen for this symbol
  Live,          // fresh and not lagging — tradable
  Stale,         // we stopped receiving (steady age > staleness_threshold)
  Delayed,       // feed lags the exchange (wall lag > delay_threshold)
  Disconnected   // the socket is down
};

// Outcome of a transport reconnect attempt, fed in by the WS adapter seam.
enum class ReconnectResult {
  Reconnected,      // socket back up
  TransportFailure, // network/socket failure (transport retries with backoff)
  AuthFailure       // re-auth refused — route to session re-establishment (AC-3)
};

// A single market-data tick. `exchange_ts` is the exchange's wall-clock stamp
// (used for the delay test); `ltp` is integer-paise (no float).
struct Tick {
  std::string symbol;
  domain::Price ltp;
  std::chrono::system_clock::time_point exchange_ts;
};

// Stable, log/serialization-friendly state names (NFR-8 observability contract).
[[nodiscard]] std::string_view to_string(MarketDataState state) noexcept;

// The tradability-aware view over a tick stream. NOT thread-safe (single main
// loop). The injected `ClockPort` MUST outlive the view.
class MarketDataView {
 public:
  // Bind the view to a monotonic+wall clock and the two freshness thresholds.
  // `staleness_threshold` bounds the steady age (now_steady - received_at) before
  // a symbol is Stale; `delay_threshold` bounds the wall lag (now_wall -
  // exchange_ts) before it is Delayed. Both boundaries are strict (`>`): an age
  // exactly equal to the threshold is still Live (not yet Stale/Delayed).
  MarketDataView(const ports::ClockPort& clock,
                 std::chrono::milliseconds staleness_threshold,
                 std::chrono::milliseconds delay_threshold)
      : clock_(clock),
        staleness_threshold_(staleness_threshold),
        delay_threshold_(delay_threshold) {}

  // Ingest a tick. DE-DUP / out-of-order guard: if an entry already exists for
  // the symbol AND `t.exchange_ts <= stored.exchange_ts`, the tick is IGNORED (a
  // duplicate or older tick NEVER regresses the view). Otherwise the ltp +
  // exchange_ts are stored and `received_at` is stamped with now_steady().
  void on_tick(const Tick& t);

  // Connection-flag toggles (driven by the transport seam). noexcept: they only
  // flip a bool.
  void on_disconnect() noexcept { connected_ = false; }
  void on_connect() noexcept { connected_ = true; }

  // Classify the symbol. Priority order (first match wins):
  //   1) socket down            -> Disconnected
  //   2) no tick / never seen   -> Unknown
  //   3) steady age > staleness -> Stale
  //   4) wall lag  > delay      -> Delayed
  //   5) otherwise              -> Live
  [[nodiscard]] MarketDataState state_for(std::string_view symbol) const;

  // True ONLY when state_for == Live (AC-2). Stale/Delayed/Disconnected/Unknown
  // are all non-tradable.
  [[nodiscard]] bool is_tradable(std::string_view symbol) const;

  // The price-sensitive-entry gate: ok() iff the symbol is Live; otherwise a
  // typed `DataStale` Error (action BlockStrategy) naming the blocking state.
  [[nodiscard]] Result<ports::Ok> require_tradable(std::string_view symbol) const;

  // Last price for display, regardless of tradability — nullopt if never seen.
  [[nodiscard]] std::optional<domain::Price> ltp(std::string_view symbol) const;

  // Handle a transport reconnect outcome (AC-1, AC-3):
  //   * Reconnected      -> on_connect(); ok().
  //   * TransportFailure -> on_disconnect() (guaranteed Disconnected regardless
  //                         of caller ordering); ok() (the transport retries with
  //                         bounded backoff — not an endless loop here).
  //   * AuthFailure      -> on_disconnect(); send an operator alert; return a
  //                         `SessionExpired` Error (ReEstablishSession). Routes to
  //                         session validation; does NOT spin a socket-retry loop.
  [[nodiscard]] Result<ports::Ok> handle_reconnect(ReconnectResult r, ports::AlertSink& alerts);

 private:
  // Per-symbol stored state. `received_at` is the monotonic stamp of the last
  // accepted tick (for staleness); `exchange_ts` is its wall stamp (for delay and
  // the de-dup guard). `seen` distinguishes "no tick yet" from a zero price.
  struct Entry {
    domain::Price ltp{};
    std::chrono::system_clock::time_point exchange_ts{};
    std::chrono::steady_clock::time_point received_at{};
    bool seen = false;
  };

  const ports::ClockPort& clock_;
  std::chrono::milliseconds staleness_threshold_;
  std::chrono::milliseconds delay_threshold_;
  std::unordered_map<std::string, Entry> entries_;
  // Fail closed: the WS view begins DISCONNECTED until the socket connects, so a
  // tick injected before any on_connect() never reads as Live with no live socket.
  bool connected_ = false;
};

}  // namespace broker_exec::marketdata
