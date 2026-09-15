#include "eme/core/execution_cost.hpp"
#include "test_support.hpp"

#include <limits>

int main() {
    using namespace eme::core;
    eme::test::Context test;
    const auto q = [](const std::int64_t raw) { return *Quantity::from_raw(raw); };
    const auto p = [](const std::int64_t raw) { return *Price::from_raw(raw); };
    FeeAccumulator accumulator;
    const auto worked = charge_buy_fill(q(100), p(550), {70'000U, 10'000U}, accumulator);
    test.expect(worked && worked->notional.raw() == 55'000 && worked->trade_fee.raw() == 3639 &&
        worked->rounding_fee.raw() == 1361 && worked->rebate.raw() == 0 && worked->debit.raw() == 60'000,
        "venue worked example: microdollar ceil before account rounding");
    accumulator = {};
    const auto direct = charge_buy_fill(q(100), p(550), {70'000U, 100U}, accumulator);
    test.expect(direct && direct->debit.raw() == 58'700, "direct account quantum");
    accumulator = {20'000U};
    const auto free = charge_buy_fill(q(100), p(550), {0U, 10'000U}, accumulator);
    test.expect(free && free->rebate.raw() == 0 && free->debit.raw() == 60'000 && accumulator.rounding_micro == 25'000U,
        "rebate capped by this fill fee, never negative net fee");
    const auto rebate = charge_buy_fill(q(100), p(5000), {70'000U, 10'000U}, accumulator);
    test.expect(rebate && rebate->rebate.raw() == 20'000 && rebate->debit.raw() == 500'000 && accumulator.rounding_micro == 7500U,
        "unused rounding credit carries to a later fill of the same order");
    accumulator = {};
    const auto large = charge_buy_fill(q(1'000'000'000'000LL), p(5000), {70'000U, 100U}, accumulator);
    test.expect(large && large->trade_fee.raw() == 175'000'000'000'000LL && large->debit.raw() == 5'175'000'000'000'000LL,
        "large quantity uses portable checked multiply/divide");
    accumulator = {123U};
    test.expect(!charge_buy_fill(q(std::numeric_limits<std::int64_t>::max()), p(9999), {70'000U, 100U}, accumulator) &&
        accumulator.rounding_micro == 123U, "overflow leaves accumulator unchanged");
    test.expect(!charge_buy_fill(q(1), p(100), {70'000U, 1U}, accumulator) &&
        !charge_buy_fill(q(0), p(100), {70'000U, 100U}, accumulator), "reject invalid precision and zero fill");
    for (std::int64_t price = 0; price <= 10'000; price += 17) {
        for (const std::int64_t quantity : {1, 7, 100, 257}) {
            FeeAccumulator order;
            const auto charge = charge_buy_fill(q(quantity), p(price), {70'000U, 100U}, order);
            const auto numerator = static_cast<std::uint64_t>(quantity * price * (10'000 - price)) * 70'000U;
            const auto expected = static_cast<std::int64_t>((numerator + 9'999'999'999ULL) / 10'000'000'000ULL);
            test.expect(charge && charge->trade_fee.raw() == expected && charge->debit.raw() >= charge->notional.raw() &&
                charge->debit.raw() % 100 == 0, "price/quantity grid matches independent integer formula and nonnegative fees");
        }
    }
    return test.result();
}
