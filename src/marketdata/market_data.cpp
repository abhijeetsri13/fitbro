#include "broker_exec/marketdata/market_data.hpp"

#include <chrono>
#include <string>

#include "broker_exec/errors/error.hpp"

namespace broker_exec::marketdata {

namespace {

using errors::ErrorCategory;
using errors::make_error;

}  // namespace

std::string_view to_string(MarketDataState state) noexcept {
  switch (state) {
    case MarketDataState::Unknown:
      return "Unknown";
    case MarketDataState::Live:
      return "Live";
    case MarketDataState::Stale:
      return "Stale";
    case MarketDataState::Delayed:
      return "Delayed";
    case MarketDataState::Disconnected:
      return "Disconnected";
  }
  return "Unknown";
}

void MarketDataView::on_tick(const Tick& t) {
  Entry& entry = entries_[t.symbol];
  // DE-DUP / out-of-order guard: an already-seen symbol never regresses on a
  // duplicate or older tick. Strictly-newer exchange_ts is required to advance.
  if (entry.seen && t.exchange_ts <= entry.exchange_ts) {
    return;
  }
  entry.ltp = t.ltp;
  entry.exchange_ts = t.exchange_ts;
  entry.received_at = clock_.now_steady();
  entry.seen = true;
}

MarketDataState MarketDataView::state_for(std::string_view symbol) const {
  // 1) Socket down dominates everything: no symbol is tradable while disconnected.
  if (!connected_) {
    return MarketDataState::Disconnected;
  }
  // 2) Never seen a tick for this symbol.
  const auto it = entries_.find(std::string(symbol));
  if (it == entries_.end() || !it->second.seen) {
    return MarketDataState::Unknown;
  }
  const Entry& entry = it->second;
  // 3) Staleness on the MONOTONIC clock: we stopped receiving. Strict `>`, so an
  // age exactly equal to the threshold is still Live (integer durations, no float).
  const std::chrono::steady_clock::duration steady_age = clock_.now_steady() - entry.received_at;
  if (steady_age >
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(staleness_threshold_)) {
    return MarketDataState::Stale;
  }
  // 4) Delay on the WALL clock: the feed lags the exchange. Strict `>` likewise.
  const std::chrono::system_clock::duration wall_lag = clock_.now_wall() - entry.exchange_ts;
  if (wall_lag >
      std::chrono::duration_cast<std::chrono::system_clock::duration>(delay_threshold_)) {
    return MarketDataState::Delayed;
  }
  // 5) Fresh and not lagging.
  return MarketDataState::Live;
}

bool MarketDataView::is_tradable(std::string_view symbol) const {
  return state_for(symbol) == MarketDataState::Live;
}

Result<ports::Ok> MarketDataView::require_tradable(std::string_view symbol) const {
  const MarketDataState state = state_for(symbol);
  if (state == MarketDataState::Live) {
    return ports::ok();
  }
  // Price-sensitive-entry block (AC-2): a DataStale Error (BlockStrategy default)
  // naming the symbol and the blocking state.
  return fail(make_error(ErrorCategory::DataStale,
                         "market data not tradable for " + std::string(symbol) + ": " +
                             std::string(to_string(state))));
}

std::optional<domain::Price> MarketDataView::ltp(std::string_view symbol) const {
  const auto it = entries_.find(std::string(symbol));
  if (it == entries_.end() || !it->second.seen) {
    return std::nullopt;
  }
  return it->second.ltp;
}

Result<ports::Ok> MarketDataView::handle_reconnect(ReconnectResult r, ports::AlertSink& alerts) {
  switch (r) {
    case ReconnectResult::Reconnected:
      on_connect();
      return ports::ok();
    case ReconnectResult::TransportFailure:
      // Guarantee Disconnected regardless of caller ordering (a first-attempt
      // failure must not stay connected). The transport retries with bounded
      // backoff — not an endless loop here.
      on_disconnect();
      return ports::ok();
    case ReconnectResult::AuthFailure:
      // AC-3: an auth-failed reconnect routes to session re-establishment and
      // does NOT spin a socket-retry loop. Mark disconnected, alert the operator,
      // and surface a SessionExpired Error (action ReEstablishSession).
      on_disconnect();
      (void)alerts.send(ports::AlertLevel::Error,
                        "market-data reconnect failed on auth; routing to session "
                        "re-establishment (no socket retry)");
      return fail(make_error(ErrorCategory::SessionExpired,
                             "market-data WS auth failed; re-establish session"));
  }
  // Unreachable: all enumerators handled above.
  return ports::ok();
}

}  // namespace broker_exec::marketdata
