#pragma once

#include "eme/core/fixed_point.hpp"

#include <cstdint>
#include <optional>

namespace eme::core {

// Explicit account/product assumptions, never inferred from a ticker.
struct FeePolicy final {
    std::uint32_t coefficient_ppm{}; // 70,000 = 0.07, including product multiplier
    std::uint32_t balance_quantum_micro{}; // 100 (direct) or 10,000 (non-direct)
};

struct FeeAccumulator final { std::uint64_t rounding_micro{}; };

struct FillCharge final {
    Cash notional;
    Cash trade_fee;
    Cash rounding_fee;
    Cash rebate;
    Cash debit;
};

struct FillCredit final {
    Cash notional;
    Cash trade_fee;
    Cash rounding_fee;
    Cash rebate;
    Cash credit;
};

// ceil(coefficient * contracts * price * (1-price)) to one microdollar,
// then balance rounding and capped per-order rebates. No floating point or
// compiler-specific wide integers. Accumulator changes only on success.
[[nodiscard]] std::optional<FillCharge> charge_buy_fill(
    Quantity quantity, Price price, FeePolicy policy, FeeAccumulator& accumulator) noexcept;

// Positive sale revenue less fees, floored to the account precision before the
// capped order rebate. Uses the same fee model and accumulator contract as buys.
[[nodiscard]] std::optional<FillCredit> credit_sell_fill(
    Quantity quantity, Price price, FeePolicy policy, FeeAccumulator& accumulator) noexcept;

}  // namespace eme::core
