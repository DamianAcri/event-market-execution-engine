#include "eme/core/execution_cost.hpp"

#include <algorithm>
#include <limits>

namespace eme::core {
namespace {
constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

// Division during multiplication keeps the uncommon large-quantity case
// portable to MSVC. The normal case uses a single hardware division.
std::optional<std::uint64_t> ceil_product(const std::uint64_t a, const std::uint64_t b) noexcept {
    constexpr std::uint64_t denominator = 10'000'000'000ULL;
    if (b == 0U) { return 0U; }
    if (a <= std::numeric_limits<std::uint64_t>::max() / b) {
        const auto product = a * b;
        const auto result = product / denominator + (product % denominator != 0U ? 1U : 0U);
        return result <= maximum ? std::optional{result} : std::nullopt;
    }
    std::uint64_t quotient = 0U;
    std::uint64_t remainder = 0U;
    for (int bit = 63; bit >= 0; --bit) {
        if (quotient > maximum / 2U) { return std::nullopt; }
        quotient *= 2U;
        remainder *= 2U;
        if (((b >> bit) & 1U) != 0U) {
            const auto add = a / denominator;
            if (quotient > maximum - add) { return std::nullopt; }
            quotient += add;
            remainder += a % denominator;
        }
        const auto carry = remainder / denominator;
        if (quotient > maximum - carry) { return std::nullopt; }
        quotient += carry;
        remainder %= denominator;
    }
    if (remainder != 0U) {
        if (quotient == maximum) { return std::nullopt; }
        ++quotient;
    }
    return quotient;
}
}  // namespace

std::optional<FillCharge> charge_buy_fill(const Quantity quantity, const Price price,
    const FeePolicy policy, FeeAccumulator& accumulator) noexcept {
    const auto quantum = static_cast<std::uint64_t>(policy.balance_quantum_micro);
    if ((quantum != 100U && quantum != 10'000U) || policy.coefficient_ppm > 1'000'000U ||
        quantity.raw() == 0) { return std::nullopt; }
    const auto q = static_cast<std::uint64_t>(quantity.raw());
    const auto p = static_cast<std::uint64_t>(price.raw());
    if (p != 0U && q > maximum / p) { return std::nullopt; }
    const auto notional = q * p;
    const auto fee = ceil_product(q, static_cast<std::uint64_t>(policy.coefficient_ppm) * p * (10'000U - p));
    if (!fee || notional > maximum - *fee) { return std::nullopt; }
    const auto total = notional + *fee;
    const auto rounding = (quantum - total % quantum) % quantum;
    if (total > maximum - rounding ||
        accumulator.rounding_micro > std::numeric_limits<std::uint64_t>::max() - rounding) {
        return std::nullopt;
    }
    const auto accumulated = accumulator.rounding_micro + rounding;
    // A rebate may not make this fill's total fee negative.
    const auto rebate = std::min(accumulated / quantum, (*fee + rounding) / quantum) * quantum;
    accumulator.rounding_micro = accumulated - rebate;
    const auto cash = [](const std::uint64_t raw) { return *Cash::from_raw(static_cast<std::int64_t>(raw)); };
    return FillCharge{cash(notional), cash(*fee), cash(rounding), cash(rebate), cash(total + rounding - rebate)};
}
}  // namespace eme::core
