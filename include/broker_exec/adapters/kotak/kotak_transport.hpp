#pragma once

// broker_exec::adapters::kotak — the HTTP transport seam used by the Kotak Neo
// adapter (Story 6.1, IBR-6, TO-6).
//
// The transport abstraction (`HttpRequest`/`HttpResponse`/`HttpClient`) is
// already broker-agnostic: it landed under `adapters/kite` in Story 2.3 but
// contains NOTHING Kite-specific — only the standard library. Rather than fork a
// second identical seam (or ripple every Kite include site now), the Kotak
// adapter REUSES it through namespace aliases declared here.
//
// TIER-2 FOLLOW-UP (tracked): hoist the header to `adapters/http/http_client.hpp`
// and make both broker adapters alias it from there. That is a pure move + include
// rewrite with no behavior change, deliberately deferred so Story 6.1 does not
// touch the Kite adapter at all.
//
// NO NEW DEPENDENCY: the concrete transport stays the existing cpr/libcurl client
// (`adapters/kite/cpr_http_client.hpp`), which implements this same `HttpClient`.
// Tests drive a scripted fake instead — no network, no live credentials.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`.

#include <string_view>

#include "broker_exec/adapters/kite/http_client.hpp"

namespace broker_exec::adapters::kotak {

// The broker-agnostic transport seam, reused verbatim (see the note above).
using HttpRequest = kite::HttpRequest;
using HttpResponse = kite::HttpResponse;
using HttpClient = kite::HttpClient;

// The Kotak Neo wire paths, in ONE place so the session establisher and the REST
// client can never drift apart (the login probe and `limits()` must hit the same
// endpoint). All are relative to the transport's configured base URL
// (`https://gw-napi.kotaksecurities.com`).
//
// TIER-2 (tracked, operator step): these paths and the exact envelopes they
// return are our RECORDED ASSUMPTION from the public Kotak Neo docs, pinned by
// the committed fixtures. Anything not fixture-verified stays `Unknown` in
// `kotak_capabilities()` until a live run confirms it.
namespace endpoints {

inline constexpr std::string_view kOauthToken = "/oauth2/token";
inline constexpr std::string_view kLoginValidate = "/login/1.0/login/v2/validate";

inline constexpr std::string_view kPlaceOrder = "/Orders/2.0/quick/order/rule/ms/place";
inline constexpr std::string_view kModifyOrder = "/Orders/2.0/quick/order/vr/modify";
inline constexpr std::string_view kCancelOrder = "/Orders/2.0/quick/order/cancel";
inline constexpr std::string_view kOrderBook = "/Orders/2.0/quick/user/orders";
inline constexpr std::string_view kTradeBook = "/Orders/2.0/quick/user/trades";
inline constexpr std::string_view kPositions = "/Orders/2.0/quick/user/positions";
inline constexpr std::string_view kHoldings = "/Portfolio/1.0/portfolio/v1/holdings";
inline constexpr std::string_view kLimits = "/Orders/2.0/quick/user/limits";
inline constexpr std::string_view kScripMaster = "/Files/1.0/masterscrip/v1/file-paths";

}  // namespace endpoints

// Kotak's fixed API product header. NOT a secret — a constant product selector
// that every authenticated Neo call must carry.
inline constexpr std::string_view kNeoFinKeyHeader = "neo-fin-key";
inline constexpr std::string_view kNeoFinKeyValue = "neotradeapi";

}  // namespace broker_exec::adapters::kotak
