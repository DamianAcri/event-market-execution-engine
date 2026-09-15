#pragma once

#include "eme/core/execution_cost.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace eme::core {

// Buy prices, ascending and unique; NO prices have already been complemented.
// One assumed fill per price level. The caller supplies available, unconsumed
// depth and verifies freshness, independent legs and a $1 paired payout floor.
struct BuyLevel final { Price price; Quantity quantity; };
struct BuyDepth final { std::span<const BuyLevel> levels; FeePolicy fees; };
struct SizingLimits final {
    Quantity cap;
    Quantity step;
    Cash available;
    Cash minimum_margin;
    std::uint64_t max_evaluations;
};
struct SizedLeg final {
    Price limit = *Price::from_raw(0);
    Cash notional = *Cash::from_raw(0);
    Cash debit = *Cash::from_raw(0);
    Cash reservation = *Cash::from_raw(0);
};
struct SizedPair final {
    Quantity quantity = *Quantity::from_raw(0);
    std::array<SizedLeg, 2U> legs;
    Cash payout_floor = *Cash::from_raw(0);
    Cash net_margin = *Cash::from_raw(0);
};
enum class SizingStatus : std::uint8_t {
    optimal, no_positive_margin, no_depth, insufficient_cash,
    search_budget_exceeded, invalid_input, arithmetic_error
};
struct SizingResult final {
    SizingStatus status{SizingStatus::invalid_input};
    std::optional<SizedPair> quote;
    std::uint64_t evaluated_quantities{};
    std::uint64_t intervals_pruned{};
};

// Exact on the configured grid, or an explicit incomplete/error status with no
// quote. Maximizes full-acquisition net margin, then minimizes reservation and q.
// Does not allocate, mutate books or perform I/O. Maximum supported cap is 1e8
// centicontracts, input depth 10,001 levels per leg, fee coefficient <= 1.0.
[[nodiscard]] SizingResult size_buy_pair(
    const std::array<BuyDepth, 2U>& depth, const SizingLimits& limits) noexcept;

// Conservative peak reserve, including worst per-grid-fill rounding, compatible
// with offline IOC execution. This is a funding bound, not a fee estimate.
[[nodiscard]] std::optional<Cash> buy_reservation(
    Quantity quantity, Quantity step, Price limit, FeePolicy policy) noexcept;

}  // namespace eme::core
