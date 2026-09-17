#pragma once

#include "eme/core/net_sizing.hpp"

namespace eme::core {

struct SizedBasket final {
    Quantity quantity = *Quantity::from_raw(0);
    std::array<SizedLeg, 3U> legs;
    Cash payout_floor = *Cash::from_raw(0);
    // Diagnostics may be negative; Cash deliberately cannot represent losses.
    std::int64_t net_margin_micro{};
};

struct BasketSizingResult final {
    SizingStatus status{SizingStatus::invalid_input};
    std::optional<SizedBasket> quote;
    std::optional<SizedBasket> one_contract_diagnostic;
    std::uint64_t evaluated_quantities{};
};

// Conditional $2 floor, equal whole-contract quantities on three buy legs.
// The caller verifies the payoff relationship, current fee policies and book
// validity. This routine cannot certify settlement rules or forecast fills.
// Exact enumeration over 1..100 whole contracts; fractional depth aggregates
// across levels, with one assumed fill per consumed level. Funding reserves
// allow centicontract fragments. No allocation, book mutation or I/O.
// Maximize full-acquisition net margin, then minimize reservation and quantity.
// An exhausted budget never returns a positive partial optimum.
[[nodiscard]] BasketSizingResult size_buy_basket(
    const std::array<BuyDepth, 3U>& depth, const SizingLimits& limits) noexcept;

}  // namespace eme::core
