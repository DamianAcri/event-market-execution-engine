#include "eme/gateway/kalshi/metadata_snapshot.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace kalshi = eme::gateway::kalshi;
using Clock = std::chrono::steady_clock;

std::string fixture(const std::size_t markets) {
    std::string input = R"({"schema_version":1,"metadata_version":1,"venue":"kalshi","markets":[)";
    for (std::size_t index = 0U; index < markets; ++index) {
        if (index != 0U) { input += ','; }
        input += "{\"id\":" + std::to_string(index + 1U) + ",\"ticker\":\"SYNTHETIC-" +
                 std::to_string(index) + "\"}";
    }
    input += "],\"constraints\":[";
    for (std::size_t index = 0U; index < markets * 2U; ++index) {
        if (index != 0U) { input += ','; }
        input += "{\"id\":" + std::to_string(index + 1U) +
            ",\"semantic_version\":1,\"key\":\"relationship-" + std::to_string(index) +
            "\",\"provenance\":\"synthetic load fixture v1\",\"relationship\":{\"type\":\"implication\",\"antecedent\":" +
            std::to_string(index % markets + 1U) + ",\"consequent\":" +
            std::to_string((index + 1U) % markets + 1U) + "}}";
    }
    return input + "]}";
}

std::uint64_t digest(const std::string_view bytes) {
    std::uint64_t value = 14'695'981'039'346'656'037ULL;
    for (const char byte : bytes) {
        value = (value ^ static_cast<unsigned char>(byte)) * 1'099'511'628'211ULL;
    }
    return value;
}

void measure(const std::size_t markets, const std::size_t samples) {
    const auto input = fixture(markets);
    std::vector<double> times;
    times.reserve(samples);
    std::string canonical;
    std::uint64_t output = 0U;
    for (std::size_t index = 0U; index < samples + 2U; ++index) {
        const auto start = Clock::now();
        const auto loaded = kalshi::parse_metadata_snapshot(input);
        const auto end = Clock::now();
        const auto* snapshot = std::get_if<kalshi::MetadataSnapshot>(&loaded);
        if (snapshot == nullptr || snapshot->markets().size() != markets ||
            snapshot->constraints().size() != markets * 2U) {
            throw std::runtime_error{"metadata fixture failed"};
        }
        if (index == 0U) { canonical = snapshot->canonical_json(); }
        if (snapshot->canonical_json() != canonical) {
            throw std::runtime_error{"non-deterministic metadata output"};
        }
        // Full output validation and destruction are outside the load timer.
        for (std::size_t market = 1U; market <= markets; ++market) {
            if (snapshot->constraints().dependencies(static_cast<std::uint32_t>(market)).size() != 4U) {
                throw std::runtime_error{"metadata dependency index failed"};
            }
        }
        output = (output ^ digest(snapshot->canonical_json())) * 1'099'511'628'211ULL;
        if (index >= 2U) {
            times.push_back(std::chrono::duration<double, std::nano>{end - start}.count());
        }
    }
    double total = 0.0;
    for (const auto time : times) { total += time; }
    std::sort(times.begin(), times.end());
    const auto percentile = [&](const std::size_t percent) {
        return times[(samples * percent + 99U) / 100U - 1U];
    };
    std::cout << "metadata_load_" << markets << 'x' << markets * 2U << ',' << samples
              << ",1," << input.size() << ',' << samples << ',' << std::fixed << std::setprecision(3)
              << total / static_cast<double>(samples) << ',' << percentile(50U) << ','
              << percentile(95U) << ',' << percentile(99U) << ',' << output << '\n';
}

}  // namespace

int main(const int argc, const char* const argv[]) {
    std::size_t samples = 20U;
    if (argc == 2 && std::string_view{argv[1]} == "--smoke") {
        samples = 1U;
    } else if (argc == 3 && std::string_view{argv[1]} == "--samples") {
        const std::string_view value{argv[2]};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), samples);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
            samples == 0U || samples > 10'000U) { return 2; }
    } else if (argc != 1) { return 2; }
    try {
        std::cout << "# eme_benchmarks_v1,fixture=metadata_chain_v1,warmup_batches=2\n"
                  << "# benchmark_source_sha256=" << EME_METADATA_BENCH_SOURCE_SHA256 << '\n'
                  << "scenario,samples,batch_size,bytes_per_op,measured_ops,mean_ns_per_op,"
                     "p50_batch_ns_per_op,p95_batch_ns_per_op,p99_batch_ns_per_op,digest\n";
        for (const std::size_t markets : {32U, 512U, 4096U}) { measure(markets, samples); }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return std::cout ? 0 : 1;
}
