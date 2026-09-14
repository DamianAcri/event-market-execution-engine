#include "eme/opportunity/candidate_tracker.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
namespace op = eme::opportunity;

void measure(const std::uint32_t definitions, const std::uint32_t fanout, const std::size_t samples) {
    eme::constraint::ConstraintRegistry registry;
    eme::market::MarketState state; (void)state.open_connection(1U);
    for (std::uint32_t id = 1U; id <= definitions; ++id) {
        const auto left = id <= fanout ? 1U : id * 2U - 1U;
        const auto right = id * 2U;
        if (registry.add({id, 1U, "pair-" + std::to_string(id), "synthetic fanout fixture",
            eme::constraint::Complement{left, right}}) != eme::constraint::ConstraintRegistrationResult::registered) {
            throw std::runtime_error{"registry fixture failed"};
        }
        for (const auto market : {left, right}) {
            if (state.find_book(market)) { continue; }
            const auto applied = state.apply(eme::market::BookSnapshot{market, 1U, 1U, 1U, {}, {},
                {{*eme::core::Price::from_raw(4000), *eme::core::Quantity::from_raw(market == 1U ? 100 : 200)}}});
            if (applied != eme::market::MarketApplyResult{eme::book::BookUpdateResult::applied}) {
                throw std::runtime_error{"market fixture failed"};
            }
        }
    }
    op::CandidateTracker tracker{1U, registry};
    if (tracker.refresh_all(state).size() != definitions) { throw std::runtime_error{"initial candidates missing"}; }
    std::vector<double> times; times.reserve(samples);
    std::uint64_t digest = 14695981039346656037ULL;
    const auto fold = [&digest](const std::uint64_t value) { digest = (digest ^ value) * 1099511628211ULL; };
    constexpr std::size_t batch = 16U;
    std::uint64_t sequence = 1U;
    for (std::size_t sample = 0U; sample < samples + 4U; ++sample) {
        const auto start = Clock::now();
        for (std::size_t index = 0U; index < batch; ++index) {
            ++sequence;
            const auto applied = state.apply(eme::market::BookDelta{1U, 1U, 1U, sequence, {}, eme::book::Side::ask,
                *eme::core::Price::from_raw(4000), eme::core::QuantityDelta::from_raw(sequence % 2U == 0U ? 1 : -1)});
            if (applied != eme::market::MarketApplyResult{eme::book::BookUpdateResult::applied}) {
                throw std::runtime_error{"delta fixture failed"};
            }
            const auto events = tracker.refresh(1U, state);
            if (events.size() != fanout || tracker.active_count() != definitions) {
                throw std::runtime_error{"candidate output count mismatch"};
            }
            for (const auto& event : events) {
                if (event.kind != op::CandidateEventKind::updated || !event.quote) {
                    throw std::runtime_error{"candidate output kind mismatch"};
                }
                fold(event.id.metadata_version); fold(event.id.constraint_id); fold(event.id.semantic_version);
                fold(static_cast<std::uint64_t>(event.id.direction));
                for (std::size_t leg = 0U; leg < 2U; ++leg) {
                    fold(event.id.legs[leg].market_id); fold(static_cast<std::uint64_t>(event.id.legs[leg].outcome));
                    fold(static_cast<std::uint64_t>(event.quote->acquisition_prices[leg].raw()));
                }
                fold(static_cast<std::uint64_t>(event.kind)); fold(static_cast<std::uint64_t>(event.reason));
                fold(static_cast<std::uint64_t>(event.quote->quantity.raw()));
                fold(static_cast<std::uint64_t>(event.quote->acquisition_cost.raw()));
                fold(static_cast<std::uint64_t>(event.quote->minimum_payout.raw()));
                fold(static_cast<std::uint64_t>(event.quote->gross_margin.raw()));
            }
        }
        const auto stop = Clock::now();
        if (sample >= 4U) {
            times.push_back(std::chrono::duration<double, std::nano>{stop - start}.count() / static_cast<double>(batch));
        }
    }
    double total = 0.0; for (const auto time : times) { total += time; }
    std::sort(times.begin(), times.end());
    const auto percentile = [&](const std::size_t percent) { return times[(samples * percent + 99U) / 100U - 1U]; };
    std::cout << "candidate_update_" << definitions << "_fanout_" << fanout << ',' << samples << ',' << batch
        << ",0," << samples * batch << ',' << std::fixed << std::setprecision(3)
        << total / static_cast<double>(samples) << ',' << percentile(50U) << ',' << percentile(95U) << ','
        << percentile(99U) << ',' << digest << '\n';
}
}  // namespace

int main(const int argc, const char* const argv[]) {
    std::size_t samples = 50U;
    bool smoke = false;
    if (argc == 2 && std::string_view{argv[1]} == "--smoke") { smoke = true; samples = 1U; }
    else if (argc == 3 && std::string_view{argv[1]} == "--samples") {
        const std::string_view value{argv[2]};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), samples);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || samples == 0U || samples > 10000U) { return 2; }
    } else if (argc != 1) { return 2; }
    try {
        std::cout << "# eme_benchmarks_v1,fixture=candidate_fanout_v1,warmup_batches=4\n"
                  << "# benchmark_source_sha256=" << EME_CANDIDATE_BENCH_SOURCE_SHA256 << '\n'
                  << "scenario,samples,batch_size,bytes_per_op,measured_ops,mean_ns_per_op,"
                     "p50_batch_ns_per_op,p95_batch_ns_per_op,p99_batch_ns_per_op,digest\n";
        if (smoke) { measure(32U, 4U, samples); }
        else { measure(64U, 1U, samples); measure(1024U, 1U, samples); measure(8192U, 1U, samples); measure(8192U, 32U, samples); }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return std::cout ? 0 : 1;
}
