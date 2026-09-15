#include "eme/core/net_sizing.hpp"

#include <algorithm>
#include <limits>

namespace eme::core {
namespace {
constexpr std::int64_t quantity_bound = 100'000'000;
constexpr std::int64_t cash_bound = 1'000'000'000'000'000;
constexpr std::int64_t fee_denominator = 10'000'000'000;
bool valid_fee(const FeePolicy fee) noexcept {
    return fee.coefficient_ppm <= 1'000'000U &&
        (fee.balance_quantum_micro == 100U || fee.balance_quantum_micro == 10'000U);
}
Cash cash(const std::int64_t value) noexcept { return *Cash::from_raw(value); }
Quantity quantity(const std::int64_t value) noexcept { return *Quantity::from_raw(value); }

struct Cursor final {
    BuyDepth depth;
    std::int64_t step{};
    std::int64_t cap{};
    std::size_t index{};
    std::int64_t prefix_quantity{};
    std::int64_t prefix_notional{};
    std::int64_t prefix_debit{};
    FeeAccumulator accumulator{};

    std::int64_t size() const noexcept {
        return std::min(depth.levels[index].quantity.raw(), cap) / step * step;
    }
    bool seek() noexcept {
        while (index < depth.levels.size() && size() == 0) { ++index; }
        return index < depth.levels.size();
    }
    std::int64_t end() const noexcept { return prefix_quantity + size(); }
    bool advance() noexcept {
        const auto count = size();
        const auto charge = charge_buy_fill(quantity(count), depth.levels[index].price,
                                             depth.fees, accumulator);
        if (!charge) { return false; }
        prefix_quantity += count;
        prefix_notional += charge->notional.raw();
        prefix_debit += charge->debit.raw();
        ++index;
        return true;
    }
};

struct Evaluation final {
    std::int64_t q{};
    std::array<SizedLeg, 2U> legs;
    std::int64_t margin{};
    std::int64_t reserve{};
    std::int64_t upper{};
};

class Search final {
public:
    Search(const std::array<BuyDepth, 2U>& depth, const SizingLimits& limits) noexcept
        : limits_{limits}, cursors_{{{depth[0U], limits.step.raw(), limits.cap.raw()},
                                    {depth[1U], limits.step.raw(), limits.cap.raw()}}} {}

    SizingResult run() noexcept {
        result_.status = SizingStatus::no_positive_margin;
        if (!cursors_[0U].seek() || !cursors_[1U].seek()) {
            result_.status = SizingStatus::no_depth;
            return result_;
        }
        // Ascending acquisition prices and nonnegative net fees prove that no
        // deeper quantity can earn a positive margin if even the best pair
        // costs a dollar before fees. Avoid any sizing work on this common path.
        if (price(0U).raw() + price(1U).raw() >= 10'000) { return result_; }
        const auto step = limits_.step.raw();
        const auto cap = limits_.cap.raw() / step * step;
        std::int64_t first = step;
        bool funded = false;
        while (first <= cap) {
            if (!cursors_[0U].seek() || !cursors_[1U].seek()) { break; }
            const auto end = std::min({cap, cursors_[0U].end(), cursors_[1U].end()});
            // Funding is monotone even though rounded net profit need not be.
            auto last = end;
            if (reservation(last) > limits_.available.raw()) {
                auto lo = first / step - 1;
                auto hi = last / step;
                while (lo < hi) {
                    const auto mid = lo + (hi - lo + 1) / 2;
                    if (reservation(mid * step) <= limits_.available.raw()) { lo = mid; }
                    else { hi = mid - 1; }
                }
                last = lo * step;
            }
            if (last < first) { break; }
            funded = true;
            // Continuous (before rounding) margin is linear while both active
            // prices are fixed. Its slope fits int64 without wide arithmetic.
            slope_ = fee_denominator * (10'000 - price(0U).raw() - price(1U).raw());
            for (std::size_t leg = 0U; leg < 2U; ++leg) {
                const auto p = price(leg).raw();
                slope_ -= static_cast<std::int64_t>(cursors_[leg].depth.fees.coefficient_ppm) * p * (10'000 - p);
            }
            const auto low = evaluate(first);
            if (!low) { return failed(); }
            const auto high = last == first ? low : evaluate(last);
            if (!high || !interval(*low, *high)) { return failed(); }
            if (last < end || end == cap) { break; }
            first = end + step;
            for (auto& cursor : cursors_) {
                if (cursor.end() == end && !cursor.advance()) {
                    result_.status = SizingStatus::arithmetic_error;
                    return failed();
                }
            }
        }
        if (best_) {
            result_.status = SizingStatus::optimal;
            result_.quote = SizedPair{quantity(best_->q), best_->legs,
                cash(best_->q * 10'000), cash(best_->margin)};
        } else if (!funded) {
            result_.status = cursors_[0U].prefix_quantity == 0 &&
                (!cursors_[0U].seek() || !cursors_[1U].seek()) ?
                SizingStatus::no_depth : SizingStatus::insufficient_cash;
        }
        return result_;
    }

private:
    Price price(const std::size_t leg) const noexcept {
        return cursors_[leg].depth.levels[cursors_[leg].index].price;
    }
    std::int64_t reservation(const std::int64_t q) const noexcept {
        std::int64_t total = 0;
        for (std::size_t leg = 0U; leg < 2U; ++leg) {
            const auto reserve = buy_reservation(quantity(q), limits_.step, price(leg), cursors_[leg].depth.fees);
            if (!reserve) { return std::numeric_limits<std::int64_t>::max(); }
            total += reserve->raw();
        }
        return total;
    }
    std::optional<Evaluation> evaluate(const std::int64_t q) noexcept {
        if (result_.evaluated_quantities == limits_.max_evaluations) {
            result_.status = SizingStatus::search_budget_exceeded;
            return std::nullopt;
        }
        ++result_.evaluated_quantities;
        Evaluation value;
        value.q = q;
        value.margin = q * 10'000;
        std::int64_t rounding_remainder = 0;
        std::uint64_t fee_ceil_error = 0U;
        for (std::size_t leg = 0U; leg < 2U; ++leg) {
            const auto& cursor = cursors_[leg];
            auto accumulator = cursor.accumulator;
            const auto charge = charge_buy_fill(quantity(q - cursor.prefix_quantity), price(leg),
                                                cursor.depth.fees, accumulator);
            const auto reserve = buy_reservation(quantity(q), limits_.step, price(leg), cursor.depth.fees);
            if (!charge || !reserve) {
                result_.status = SizingStatus::arithmetic_error;
                return std::nullopt;
            }
            const auto debit = cursor.prefix_debit + charge->debit.raw();
            value.legs[leg] = {price(leg), cash(cursor.prefix_notional + charge->notional.raw()),
                               cash(debit), *reserve};
            value.margin -= debit;
            value.reserve += reserve->raw();
            rounding_remainder += static_cast<std::int64_t>(accumulator.rounding_micro);
            const auto p = static_cast<std::uint64_t>(price(leg).raw());
            const auto factor = static_cast<std::uint64_t>(cursor.depth.fees.coefficient_ppm) * p * (10'000U - p);
            const auto remainder = (static_cast<std::uint64_t>(q - cursor.prefix_quantity) *
                (factor % fee_denominator)) % fee_denominator;
            if (remainder != 0U) { fee_ceil_error += fee_denominator - remainder; }
        }
        // Total debit = notional + summed ceiled trade fees + remaining rounding
        // accumulator. Remove it and the exact fractional fee-ceil errors to
        // obtain floor(continuous margin). Products for the remainders are
        // reduced first, so they fit uint64 on every supported compiler.
        value.upper = value.margin + rounding_remainder +
            static_cast<std::int64_t>(fee_ceil_error / fee_denominator);
        if (value.margin > limits_.minimum_margin.raw() &&
            (!best_ || value.margin > best_->margin ||
             (value.margin == best_->margin &&
              (value.reserve < best_->reserve || (value.reserve == best_->reserve && q < best_->q))))) {
            best_ = value;
        }
        return value;
    }
    bool interval(const Evaluation& low, const Evaluation& high) noexcept {
        if (high.q - low.q <= limits_.step.raw()) { return true; }
        const auto upper = slope_ >= 0 ? high.upper : low.upper;
        const auto floor = best_ ? best_->margin : limits_.minimum_margin.raw();
        if (upper < floor || (upper == floor && (!best_ || low.q >= best_->q))) {
            ++result_.intervals_pruned;
            return true;
        }
        const auto steps = (high.q - low.q) / limits_.step.raw();
        const auto mid = evaluate(low.q + (steps / 2) * limits_.step.raw());
        if (!mid) { return false; }
        return interval(low, *mid) && interval(*mid, high);
    }
    SizingResult failed() noexcept { result_.quote.reset(); return result_; }
    const SizingLimits& limits_;
    std::array<Cursor, 2U> cursors_;
    SizingResult result_;
    std::optional<Evaluation> best_;
    std::int64_t slope_{};
};
}  // namespace

std::optional<Cash> buy_reservation(const Quantity q, const Quantity step,
    const Price limit, const FeePolicy fee) noexcept {
    if (q.raw() > quantity_bound || (step.raw() != 1 && step.raw() != 100) ||
        q.raw() % step.raw() != 0 || !valid_fee(fee)) { return std::nullopt; }
    if (q.raw() == 0) { return cash(0); }
    FeeAccumulator accumulator;
    const auto peak = charge_buy_fill(q, *Price::from_raw(5000), fee, accumulator);
    if (!peak) { return std::nullopt; }
    return cash(q.raw() * limit.raw() + peak->trade_fee.raw() +
        q.raw() / step.raw() * (static_cast<std::int64_t>(fee.balance_quantum_micro) + 1));
}

SizingResult size_buy_pair(const std::array<BuyDepth, 2U>& depth, const SizingLimits& limits) noexcept {
    if (limits.cap.raw() > quantity_bound || (limits.step.raw() != 1 && limits.step.raw() != 100) ||
        limits.cap.raw() < limits.step.raw() || limits.available.raw() > cash_bound ||
        limits.minimum_margin.raw() > cash_bound || limits.max_evaluations == 0U) { return {}; }
    for (const auto& leg : depth) {
        if (!valid_fee(leg.fees) || leg.levels.size() > 10'001U) { return {}; }
        std::int64_t previous = -1;
        for (const auto& level : leg.levels) {
            if (level.price.raw() <= previous) { return {}; }
            previous = level.price.raw();
        }
    }
    return Search{depth, limits}.run();
}
}  // namespace eme::core
