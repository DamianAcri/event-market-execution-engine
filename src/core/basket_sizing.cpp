#include "eme/core/basket_sizing.hpp"

#include <algorithm>

namespace eme::core {
namespace {
constexpr std::int64_t maximum_quantity = 10'000;
constexpr std::int64_t quantity_step = 100;
constexpr std::int64_t maximum_cash = 1'000'000'000'000'000;

// Each completed price level is charged exactly once. A partially consumed
// final level is recomputed from that prefix when q grows, so increasing the
// order size does not incorrectly manufacture additional assumed fills.
struct CostCursor final {
    BuyDepth depth;
    std::size_t index{};
    std::int64_t prefix_quantity{}, prefix_notional{}, prefix_debit{};
    FeeAccumulator accumulator;

    std::optional<SizedLeg> at(const std::int64_t quantity) noexcept {
        while (index < depth.levels.size() &&
               depth.levels[index].quantity.raw() < quantity - prefix_quantity) {
            const auto& level = depth.levels[index];
            const auto charge = charge_buy_fill(level.quantity, level.price, depth.fees, accumulator);
            if (!charge) { return std::nullopt; }
            prefix_quantity += level.quantity.raw();
            prefix_notional += charge->notional.raw();
            prefix_debit += charge->debit.raw();
            ++index;
        }
        if (index == depth.levels.size()) { return std::nullopt; }
        const auto& level = depth.levels[index];
        auto partial_accumulator = accumulator;
        const auto charge = charge_buy_fill(*Quantity::from_raw(quantity - prefix_quantity),
            level.price, depth.fees, partial_accumulator);
        const auto reserve = buy_reservation(*Quantity::from_raw(quantity),
            *Quantity::from_raw(1), level.price, depth.fees);
        if (!charge || !reserve) { return std::nullopt; }
        return SizedLeg{level.price, *Cash::from_raw(prefix_notional + charge->notional.raw()),
            *Cash::from_raw(prefix_debit + charge->debit.raw()), *reserve};
    }
};
}  // namespace

BasketSizingResult size_buy_basket(const std::array<BuyDepth, 3U>& depth,
                                  const SizingLimits& limits) noexcept {
    BasketSizingResult result;
    if (limits.cap.raw() < quantity_step || limits.cap.raw() > maximum_quantity ||
        limits.cap.raw() % quantity_step != 0 || limits.step.raw() != quantity_step ||
        limits.available.raw() > maximum_cash || limits.minimum_margin.raw() > maximum_cash ||
        limits.max_evaluations == 0U) { return result; }
    auto available = limits.cap.raw();
    for (const auto& leg : depth) {
        if (leg.levels.size() > 10'001U || leg.fees.coefficient_ppm > 1'000'000U ||
            (leg.fees.balance_quantum_micro != 100U && leg.fees.balance_quantum_micro != 10'000U)) {
            return result;
        }
        std::int64_t previous = -1, total = 0;
        for (const auto& level : leg.levels) {
            if (level.price.raw() <= previous || level.quantity.raw() == 0) { return result; }
            previous = level.price.raw();
            total = std::min(limits.cap.raw(), total + std::min(level.quantity.raw(), limits.cap.raw()));
        }
        available = std::min(available, total);
    }
    if (available < quantity_step) { result.status = SizingStatus::no_depth; return result; }

    std::array<CostCursor, 3U> cursors{};
    for (std::size_t index = 0; index < cursors.size(); ++index) { cursors[index].depth = depth[index]; }
    std::optional<SizedBasket> best;
    std::int64_t best_reserve = 0;
    bool funded = false;
    result.status = SizingStatus::no_positive_margin;
    for (std::int64_t q = quantity_step; q <= available; q += quantity_step) {
        if (result.evaluated_quantities == limits.max_evaluations) {
            result.status = SizingStatus::search_budget_exceeded;
            return result;
        }
        ++result.evaluated_quantities;
        SizedBasket value;
        value.quantity = *Quantity::from_raw(q);
        value.payout_floor = *Cash::from_raw(q * 20'000);
        std::int64_t debit = 0, reserve = 0;
        for (std::size_t index = 0; index < cursors.size(); ++index) {
            const auto leg = cursors[index].at(q);
            if (!leg) { result.status = SizingStatus::arithmetic_error; return result; }
            value.legs[index] = *leg;
            debit += leg->debit.raw();
            reserve += leg->reservation.raw();
        }
        value.net_margin_micro = value.payout_floor.raw() - debit;
        if (q == quantity_step) { result.one_contract_diagnostic = value; }
        // With nonnegative fees, q and each leg's worst consumed price only
        // increase. Once this conservative reserve fails, later sizes fail too.
        if (reserve > limits.available.raw()) { break; }
        funded = true;
        if (value.net_margin_micro > limits.minimum_margin.raw() &&
            (!best || value.net_margin_micro > best->net_margin_micro ||
             (value.net_margin_micro == best->net_margin_micro &&
              (reserve < best_reserve || (reserve == best_reserve && q < best->quantity.raw()))))) {
            best = value;
            best_reserve = reserve;
        }
    }
    if (best) { result.status = SizingStatus::optimal; result.quote = best; }
    else if (!funded) { result.status = SizingStatus::insufficient_cash; }
    return result;
}

}  // namespace eme::core
