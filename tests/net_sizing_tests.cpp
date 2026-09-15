#include "eme/core/net_sizing.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <random>
#include <vector>

namespace {
using namespace eme::core;
using eme::test::price;
using eme::test::quantity;
Cash cash(const std::int64_t n) { return *Cash::from_raw(n); }

// Independent exhaustive reference: restart each complete order for each q.
// All fixtures are small enough to use a direct integer fee numerator. This
// deliberately does not share the production fee, reservation or cursor code.
std::optional<SizedPair> exhaustive(const std::array<BuyDepth, 2U>& books, const SizingLimits& limits) {
    std::optional<SizedPair> best;
    std::int64_t best_reserve = 0;
    for (auto q = limits.step.raw(); q <= limits.cap.raw(); q += limits.step.raw()) {
        SizedPair trial;
        trial.quantity = quantity(q);
        trial.payout_floor = cash(q * 10'000);
        std::int64_t cost = 0;
        std::int64_t reserve = 0;
        bool filled = true;
        for (std::size_t leg = 0U; leg < 2U; ++leg) {
            const auto fee = books[leg].fees;
            const auto quantum = static_cast<std::int64_t>(fee.balance_quantum_micro);
            std::int64_t remaining = q;
            std::int64_t credit = 0;
            std::int64_t notional = 0;
            std::int64_t debit = 0;
            std::int64_t last_price = 0;
            for (const auto& level : books[leg].levels) {
                const auto fill = std::min(remaining, level.quantity.raw() / limits.step.raw() * limits.step.raw());
                if (fill == 0) { continue; }
                last_price = level.price.raw();
                const auto value = fill * last_price;
                const auto numerator = static_cast<std::uint64_t>(fill * last_price * (10'000 - last_price)) * fee.coefficient_ppm;
                const auto trade_fee = static_cast<std::int64_t>((numerator + 9'999'999'999ULL) / 10'000'000'000ULL);
                const auto rounding = (quantum - (value + trade_fee) % quantum) % quantum;
                credit += rounding;
                const auto rebate = std::min(credit / quantum, (trade_fee + rounding) / quantum) * quantum;
                credit -= rebate;
                notional += value;
                debit += value + trade_fee + rounding - rebate;
                remaining -= fill;
                if (remaining == 0) { break; }
            }
            if (remaining != 0) { filled = false; break; }
            const auto peak_fee = (static_cast<std::uint64_t>(q) * fee.coefficient_ppm * 25'000'000U +
                9'999'999'999ULL) / 10'000'000'000ULL;
            const auto leg_reserve = q * last_price + static_cast<std::int64_t>(peak_fee) +
                q / limits.step.raw() * (quantum + 1);
            trial.legs[leg] = {price(last_price), cash(notional), cash(debit), cash(leg_reserve)};
            reserve += leg_reserve;
            cost += debit;
        }
        const auto margin = q * 10'000 - cost;
        if (!filled || reserve > limits.available.raw() || margin <= limits.minimum_margin.raw()) { continue; }
        if (!best || margin > best->net_margin.raw() ||
            (margin == best->net_margin.raw() && reserve < best_reserve)) {
            trial.net_margin = cash(margin);
            best = trial;
            best_reserve = reserve;
        }
    }
    return best;
}
bool equal(const SizedPair& a, const SizedPair& b) {
    if (a.quantity != b.quantity || a.net_margin != b.net_margin || a.payout_floor != b.payout_floor) { return false; }
    for (std::size_t i = 0U; i < 2U; ++i) {
        if (a.legs[i].limit != b.legs[i].limit || a.legs[i].debit != b.legs[i].debit ||
            a.legs[i].reservation != b.legs[i].reservation || a.legs[i].notional != b.legs[i].notional) { return false; }
    }
    return true;
}
}  // namespace

int main() {
    eme::test::Context test;
    const std::array a{BuyLevel{price(3000), quantity(500)}};
    const std::array b{BuyLevel{price(6000), quantity(200)}, BuyLevel{price(8000), quantity(300)}};
    std::array<BuyDepth, 2U> books{{{a, {70'000U, 100U}}, {b, {70'000U, 100U}}}};
    SizingLimits limits{quantity(500), quantity(100), cash(100'000'000), cash(0), 100'000U};
    const auto recovered = size_buy_pair(books, limits);
    test.expect(recovered.quote && recovered.quote->quantity.raw() == 200 && recovered.quote->net_margin.raw() == 137'000,
        "choose two profitable contracts although buying five loses money");
    const auto reference = exhaustive(books, limits);
    test.expect(reference && recovered.quote && equal(*reference, *recovered.quote), "entire quote matches independent fee ledger");
    limits.max_evaluations = 1U;
    const auto incomplete = size_buy_pair(books, limits);
    test.expect(incomplete.status == SizingStatus::search_budget_exceeded && !incomplete.quote,
        "budget exhaustion never reports absence of opportunities or an unproven optimum");
    limits.max_evaluations = 100'000U;
    limits.available = cash(0);
    test.expect(size_buy_pair(books, limits).status == SizingStatus::insufficient_cash, "no unfunded quote");
    limits.available = cash(100'000'000);
    books[1U].levels = {};
    test.expect(size_buy_pair(books, limits).status == SizingStatus::no_depth, "missing leg cannot be sized");
    const std::array unordered{BuyLevel{price(8000), quantity(100)}, BuyLevel{price(6000), quantity(100)}};
    books[1U].levels = unordered;
    test.expect(size_buy_pair(books, limits).status == SizingStatus::invalid_input, "reject unordered depth");
    books[1U].levels = b;
    limits.step = quantity(2);
    test.expect(size_buy_pair(books, limits).status == SizingStatus::invalid_input, "reject unsupported grid");
    limits.step = quantity(100);
    books[0U].fees.coefficient_ppm = 1'000'001U;
    test.expect(size_buy_pair(books, limits).status == SizingStatus::invalid_input, "reject unsupported fee policy");

    std::mt19937 random{192'031U};
    for (std::size_t sample = 0U; sample < 6000U; ++sample) {
        const std::int64_t step = sample % 2U == 0U ? 1 : 100;
        std::array<std::vector<BuyLevel>, 2U> levels;
        for (auto& leg : levels) {
            std::int64_t p = static_cast<std::int64_t>(random() % 3500U);
            for (std::size_t n = 0U, count = 1U + random() % 7U; n < count && p <= 10'000; ++n) {
                leg.push_back({price(p), quantity(static_cast<std::int64_t>(random() % 15U) * step + step / 2)});
                p += static_cast<std::int64_t>(1U + random() % 1800U);
            }
        }
        constexpr std::array<std::uint32_t, 5U> coefficients{0U, 1U, 35'000U, 70'000U, 1'000'000U};
        for (std::size_t leg = 0U; leg < 2U; ++leg) {
            books[leg] = {levels[leg], {coefficients[random() % coefficients.size()], random() % 2U == 0U ? 100U : 10'000U}};
        }
        limits = {quantity(static_cast<std::int64_t>(1U + random() % 70U) * step), quantity(step),
            cash(static_cast<std::int64_t>(random() % 900'000U) * step), cash(static_cast<std::int64_t>(random() % 1000U)), 100'000U};
        const auto expected = exhaustive(books, limits);
        const auto actual = size_buy_pair(books, limits);
        test.expect(actual.status != SizingStatus::search_budget_exceeded && actual.quote.has_value() == expected.has_value() &&
            (!expected || equal(*expected, *actual.quote)), "random multi-level, funded, rounded optimum equals exhaustive independent ledger");
    }
    const std::array big_a{BuyLevel{price(3000), quantity(100'000'000)}};
    const std::array big_b{BuyLevel{price(6000), quantity(100'000'000)}};
    books = {{{big_a, {70'000U, 100U}}, {big_b, {70'000U, 100U}}}};
    limits = {quantity(100'000'000), quantity(1), cash(10'000'000'000'000LL), cash(0), 1000U};
    const auto large = size_buy_pair(books, limits);
    test.expect(large.quote && large.quote->quantity == limits.cap && large.quote->net_margin.raw() == 68'500'000'000LL &&
        large.evaluated_quantities < 100U, "one hundred million quantity choices pruned without wide arithmetic");
    const std::array flat_b{BuyLevel{price(7000), quantity(100'000'000)}};
    books = {{{big_a, {0U, 100U}}, {flat_b, {0U, 100U}}}};
    const auto flat = size_buy_pair(books, limits);
    test.expect(flat.status == SizingStatus::no_positive_margin && flat.evaluated_quantities == 0U,
        "nonpositive best-price edge proves absence of profit before sizing");
    return test.result();
}
