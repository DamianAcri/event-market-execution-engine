#include "eme/session/execution_study.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <streambuf>
#include <vector>

namespace {
class DigestBuffer final : public std::streambuf {
public:
    std::uint64_t digest{14695981039346656037ULL};
protected:
    std::streamsize xsputn(const char* data, const std::streamsize size) override {
        for (std::streamsize i = 0; i < size; ++i) { fold(static_cast<unsigned char>(data[i])); }
        return size;
    }
    int_type overflow(const int_type value) override {
        if (!traits_type::eq_int_type(value, traits_type::eof())) { fold(static_cast<unsigned char>(traits_type::to_char_type(value))); }
        return traits_type::not_eof(value);
    }
private:
    void fold(const unsigned char value) { digest = (digest ^ value) * 1099511628211ULL; }
};
}  // namespace

int main(const int argc, const char* const argv[]) {
    if (argc != 5) { std::cerr << "Usage: eme_study_benchmarks <session> <plan> <policy> <samples>\n"; return 2; }
    std::size_t samples{};
    const std::string_view count{argv[4]};
    const auto parsed = std::from_chars(count.data(), count.data() + count.size(), samples);
    if (parsed.ec != std::errc{} || parsed.ptr != count.data() + count.size() || samples == 0U || samples > 10'000U) { return 2; }
    const auto loaded = eme::session::load_replay(argv[1], argv[2]);
    if (!std::holds_alternative<eme::session::ReplayInput>(loaded)) { std::cerr << "Invalid replay input\n"; return 1; }
    const auto& input = std::get<eme::session::ReplayInput>(loaded);
    std::vector<double> times;
    std::uint64_t expected{};
    for (std::size_t sample = 0U; sample < samples + 3U; ++sample) {
        DigestBuffer buffer; std::ostream output{&buffer};
        const auto start = std::chrono::steady_clock::now();
        const auto failure = eme::session::run_execution_study(input, argv[3], output);
        const auto end = std::chrono::steady_clock::now();
        if (failure) { std::cerr << failure->reason << '\n'; return 1; }
        if (sample == 0U) { expected = buffer.digest; }
        if (buffer.digest != expected) { std::cerr << "Nondeterministic study\n"; return 1; }
        if (sample >= 3U) { times.push_back(std::chrono::duration<double, std::nano>{end - start}.count()); }
    }
    double total{}; for (const auto time : times) { total += time; }
    std::sort(times.begin(), times.end());
    const auto percentile = [&](const std::size_t percent) { return times[(samples * percent + 99U) / 100U - 1U]; };
    std::cout << "# eme_benchmarks_v1,fixture=external_verified_study_v1,warmup_runs=3\n"
              << "# benchmark_source_sha256=" << EME_STUDY_BENCH_SOURCE_SHA256 << '\n'
              << "# manifest_sha256=" << input.plan.manifest_sha256 << ",plan_sha256=" << input.plan.plan_sha256 << '\n'
              << "scenario,samples,batch_size,bytes_per_op,measured_ops,mean_ns_per_op,p50_batch_ns_per_op,p95_batch_ns_per_op,p99_batch_ns_per_op,digest\n"
              << "complete_study," << samples << ",1," << input.session.manifest.journal.bytes << ',' << samples << ','
              << std::fixed << std::setprecision(3) << total / static_cast<double>(samples) << ','
              << percentile(50U) << ',' << percentile(95U) << ',' << percentile(99U) << ',' << expected << '\n';
    return std::cout ? 0 : 1;
}
