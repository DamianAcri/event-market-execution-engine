#include "eme/core/net_sizing.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
using namespace eme::core;
Quantity q(const std::int64_t n) { return *Quantity::from_raw(n); }
Price p(const std::int64_t n) { return *Price::from_raw(n); }
Cash c(const std::int64_t n) { return *Cash::from_raw(n); }
struct Result { std::int64_t quantity{}; std::int64_t margin{}; std::int64_t reserve{}; };
Result exhaustive(const std::array<BuyDepth, 2U>& books, const SizingLimits& limits) {
    Result best;
    for (auto count = limits.step.raw(); count <= limits.cap.raw(); count += limits.step.raw()) {
        std::int64_t cost = 0;
        std::int64_t reserve = 0;
        bool complete = true;
        for (const auto& leg : books) {
            auto remaining = count;
            FeeAccumulator accumulator;
            auto limit = p(0);
            for (const auto& level : leg.levels) {
                const auto fill = std::min(remaining, level.quantity.raw() / limits.step.raw() * limits.step.raw());
                if (fill == 0) { continue; }
                limit = level.price;
                const auto charge = charge_buy_fill(q(fill), limit, leg.fees, accumulator);
                if (!charge) { std::terminate(); }
                cost += charge->debit.raw();
                remaining -= fill;
                if (remaining == 0) { break; }
            }
            if (remaining != 0) { complete = false; break; }
            reserve += buy_reservation(q(count), limits.step, limit, leg.fees)->raw();
        }
        const auto margin = count * 10'000 - cost;
        if (complete && reserve <= limits.available.raw() && margin > limits.minimum_margin.raw() &&
            (margin > best.margin || (margin == best.margin && reserve < best.reserve))) {
            best = {count, margin, reserve};
        }
    }
    return best;
}
}  // namespace

int main(const int argc, const char* const argv[]) {
    const bool smoke = argc == 2 && std::string_view{argv[1]} == "--smoke";
    const std::size_t samples = smoke ? 1U : 12U;
    constexpr std::size_t iterations = 10U;
    std::cout << "workload,sample,mode,iterations,total_ns,quantity_centicontracts,margin_micro_usd,evaluations\n";
    for (std::size_t workload = 0U; workload < 4U; ++workload) {
        std::array<std::vector<BuyLevel>, 2U> levels;
        const std::string_view name = workload == 0U ? "interior_optimum" : workload == 1U ? "fractional_depth" :
            workload == 2U ? "funding_bound" : "zero_edge";
        SizingLimits limits{q(10'000), q(1), c(100'000'000), c(0), 100'000U};
        std::array<BuyDepth, 2U> books;
        levels[0U] = {{p(3000), q(10'000)}};
        levels[1U] = {{p(6000), q(4000)}, {p(8000), q(6000)}};
        if (workload == 1U) {
            levels[0U].clear(); levels[1U].clear();
            for (std::int64_t i = 0; i < 32; ++i) {
                levels[0U].push_back({p(1000 + i * 100), q(350)});
                levels[1U].push_back({p(3000 + i * 123), q(370)});
            }
        }
        if (workload == 2U) { limits.available = c(20'000'000); }
        if (workload == 3U) { levels[1U] = {{p(7000), q(10'000)}}; }
        for (std::size_t i = 0U; i < 2U; ++i) { books[i] = {levels[i], {workload == 3U ? 0U : 70'000U, 100U}}; }
        const auto reference = exhaustive(books, limits);
        const auto optimized = size_buy_pair(books, limits);
        if ((reference.quantity != 0) != optimized.quote.has_value() ||
            (optimized.quote && (optimized.quote->quantity.raw() != reference.quantity || optimized.quote->net_margin.raw() != reference.margin))) {
            std::cerr << "reference mismatch\n"; return 1;
        }
        for (std::size_t sample = 0U; sample < samples; ++sample) {
            for (std::size_t order = 0U; order < 2U; ++order) {
                const bool fast = (sample + order) % 2U == 0U;
                std::int64_t checksum = 0;
                const auto start = std::chrono::steady_clock::now();
                for (std::size_t repeat = 0U; repeat < iterations; ++repeat) {
                    if (fast) {
                        const auto result = size_buy_pair(books, limits);
                        checksum += result.quote ? result.quote->net_margin.raw() : 0;
                    } else { checksum += exhaustive(books, limits).margin; }
                }
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
                if (checksum != reference.margin * static_cast<std::int64_t>(iterations)) { return 1; }
                std::cout << name << ',' << sample << ',' << (fast ? "bounded_exact" : "exhaustive") << ',' << iterations << ',' << elapsed << ','
                    << reference.quantity << ',' << reference.margin << ',' << (fast ? optimized.evaluated_quantities : 10'000U) << '\n';
            }
        }
    }
}
