#pragma once

// broker_exec::capabilities — per-broker capability model + early rejection
// (Story 2.5, FR-2).
//
// A strategy must never fail mid-trade on a feature the broker lacks. This
// module is the broker-neutral value type that answers "does this broker
// support capability X?" so a loader/gate can reject UNSUPPORTED work up front,
// at load/request time, with a typed error.
//
// POSTURE = REJECT-ONLY (MVP). There is no per-capability substitution; an
// unsupported capability is a hard, typed rejection (no silent fallback). That
// is a "should-have-soon" deferral [architecture.md#RCT-3 CAP-2 de-conflated].
//
// TRI-STATE WITH FAIL-CLOSED DEFAULT. Each capability is Supported, Unsupported,
// or Unknown. UNKNOWN == UNSUPPORTED at the gate (AC-2): an unverified capability
// never silently passes. `support_of()` exposes the raw tri-state for diagnostics;
// `supports()`/`require()` collapse it to a yes/no decision that only Supported
// passes.
//
// BOUNDARY. Pure value logic. Depends inward ONLY on `errors` (Result/Error in
// the public API). No transport/adapter/SDK dependency. Adapters (Story 2.14)
// attach a CapabilitySet to the BrokerPort; the validation gate (Story 2.8) and
// safe-start (Story 2.13) consult it.
//
// Cross-platform: C++20 standard library only. No OS APIs, no `#ifdef`. No
// double/float. Result<Ok> is no-throw.

#include <array>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>

#include "broker_exec/ports/ports_common.hpp"
#include "broker_exec/result.hpp"

namespace broker_exec::capabilities {

// The broker-contract dimensions modelled today. Only what is plausibly used now
// plus the capabilities the architecture calls out; the set is additive later.
// Keep `kCapabilityCount` as the last sentinel so the backing array stays sized
// to the enum.
enum class Capability {
  PlaceOrder,
  ModifyOrder,
  CancelOrder,
  SquareOff,
  BasketMargin,
  OrderUpdateWebsocket,
  HeadlessSessionRefresh,
  GttOrders,
  AmoOrders,
  CoverOrder,
  BracketOrder,
  TagCarry,
  MarginShockSim
};

// The number of modelled capabilities — the fixed size of CapabilitySet's
// backing array. Derived from the last enumerator so it tracks the enum.
inline constexpr std::size_t kCapabilityCount =
    static_cast<std::size_t>(Capability::MarginShockSim) + 1;

// Tri-state support for a capability.
//   Unknown     — not yet verified; treated as Unsupported at the gate (AC-2).
//   Supported   — certified available on this broker.
//   Unsupported — certified absent (reject-only; no substitution).
// Unknown is value 0 ON PURPOSE: a zero-initialized CapabilitySet is then
// fail-closed (everything unsupported) by construction, not by remembering to
// overwrite — the safe default for a trading gate.
enum class Support { Unknown, Supported, Unsupported };

// Stable, log/message-friendly names (used in NotSupported error messages and
// diagnostics; renames are a breaking change to the observability contract).
[[nodiscard]] std::string_view to_string(Capability capability) noexcept;
[[nodiscard]] std::string_view to_string(Support support) noexcept;

// An immutable, per-broker map from Capability -> Support. Every capability not
// explicitly set defaults to Support::Unknown (fail-closed). Built once (from a
// list of (Capability, Support) pairs or via the builder) and read-only after.
class CapabilitySet {
 public:
  // Default: every capability Unknown (and therefore unsupported at the gate).
  constexpr CapabilitySet() noexcept : support_{} {
    for (auto& entry : support_) {
      entry = Support::Unknown;
    }
  }

  // Build from a sparse list of (Capability, Support) pairs. Unlisted
  // capabilities stay Unknown. A capability listed twice takes the last value.
  CapabilitySet(std::initializer_list<std::pair<Capability, Support>> entries) noexcept
      : CapabilitySet() {
    for (const auto& [cap, sup] : entries) {
      support_[index_of(cap)] = sup;
    }
  }

  // Raw tri-state for diagnostics (Supported / Unsupported / Unknown).
  [[nodiscard]] Support support_of(Capability capability) const noexcept {
    return support_[index_of(capability)];
  }

  // The gate decision: true ONLY for Support::Supported. Unknown and Unsupported
  // both read as false (AC-2: unknown == unsupported).
  [[nodiscard]] bool supports(Capability capability) const noexcept {
    return support_of(capability) == Support::Supported;
  }

  // ok() if supported; otherwise a typed NotSupported error (DoNotRetry) that
  // names the capability. Reject-only: never substitutes.
  [[nodiscard]] Result<ports::Ok> require(Capability capability) const;

  // Fail-closed on the FIRST unsupported capability, naming it (load-time
  // strategy capability checks). ok() only if every capability is supported.
  [[nodiscard]] Result<ports::Ok> require_all(std::span<const Capability> capabilities) const;

  // A small immutable builder: set what is known, then build() the result. It
  // accumulates into a raw tri-state array (a CapabilitySet member is impossible
  // here — the enclosing type is still incomplete inside its own nested class).
  class Builder {
   public:
    constexpr Builder() noexcept {
      for (auto& entry : support_) {
        entry = Support::Unknown;
      }
    }

    Builder& set(Capability capability, Support support) noexcept {
      support_[index_of(capability)] = support;
      return *this;
    }

    [[nodiscard]] CapabilitySet build() const noexcept { return CapabilitySet(support_); }

   private:
    std::array<Support, kCapabilityCount> support_{};
  };

  [[nodiscard]] static Builder builder() noexcept { return Builder{}; }

 private:
  // Build directly from a fully-populated tri-state array (used by Builder).
  explicit constexpr CapabilitySet(const std::array<Support, kCapabilityCount>& support) noexcept
      : support_(support) {}

  [[nodiscard]] static constexpr std::size_t index_of(Capability capability) noexcept {
    return static_cast<std::size_t>(capability);
  }

  std::array<Support, kCapabilityCount> support_;
};

// The Kite (Zerodha) capability profile.
//
// Supported: PlaceOrder / ModifyOrder / CancelOrder / SquareOff — the order
// lifecycle the adapter implements today.
// Unsupported: HeadlessSessionRefresh — Kite has no headless re-auth; daily
// re-login is required (Story 2.4 reality).
// Everything else (BasketMargin, OrderUpdateWebsocket, MarginShockSim,
// GttOrders, AmoOrders, CoverOrder, BracketOrder, TagCarry) is left UNKNOWN on
// purpose: these are genuinely unverified and must read as unsupported until
// they are certified in Story 2.14. We do NOT optimistically mark them Supported.
[[nodiscard]] CapabilitySet kite_capabilities();

}  // namespace broker_exec::capabilities
