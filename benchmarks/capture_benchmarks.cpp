#include "eme/session/async_capture.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

namespace {
namespace session = eme::session;
using Clock = std::chrono::steady_clock;
double elapsed(const Clock::time_point start) { return std::chrono::duration<double, std::nano>{Clock::now() - start}.count(); }
struct Directory final {
    Directory() : path{std::filesystem::temp_directory_path() / ("eme-capture-bench-" + std::to_string(Clock::now().time_since_epoch().count()))} {
        if (!std::filesystem::create_directory(path)) { throw std::runtime_error{"temporary directory"}; }
    }
    ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    std::filesystem::path path;
};
}  // namespace
int main(const int argc, const char* const argv[]) {
    try {
        const bool smoke = argc == 2 && std::string_view{argv[1]} == "--smoke";
        const std::size_t samples = smoke ? 1U : 8U;
        const auto metadata = std::get<eme::gateway::kalshi::MetadataSnapshot>(
            eme::gateway::kalshi::parse_metadata_snapshot(R"({"schema_version":1,"metadata_version":11,"venue":"kalshi",
            "markets":[{"id":42,"ticker":"TEST"}],"constraints":[]})"));
        std::cout << "arrival_pattern,payload_bytes,sample,mode,records,producer_p50_ns,producer_p95_ns,producer_p99_ns,start_lateness_p99_ns,total_finalize_ns,queue_high_water_bytes,journal_sha256\n";
        for (const bool paced : {false, true}) {
            const std::size_t count = smoke ? 32U : paced ? 512U : 4096U;
            for (const std::size_t size : {128U, 512U, 4096U}) {
                std::string expected;
                for (std::size_t sample = 0U; sample < samples; ++sample) {
                    for (std::size_t order = 0U; order < 2U; ++order) {
                        const bool async = (sample + order) % 2U == 0U;
                        Directory directory;
                        std::vector<eme::journal::RawMarketRecord> records;
                        records.reserve(count);
                        for (std::size_t i = 0U; i < count; ++i) {
                            eme::journal::RawMarketRecord record;
                            record.metadata_version = 11U; record.connection_generation = 1U; record.sequence = i + 1U;
                            record.channel = "synthetic"; record.payload.assign(size, 'x');
                            records.push_back(std::move(record));
                        }
                        std::vector<double> times; times.reserve(count);
                        std::vector<double> late; late.reserve(count);
                        std::unique_ptr<session::AsyncCaptureWriter> background;
                        std::unique_ptr<session::SessionWriter> direct;
                        if (async) {
                            background = std::move(std::get<std::unique_ptr<session::AsyncCaptureWriter>>(
                                session::create_async_capture(directory.path / "session", metadata, {count, 64U * 1024U * 1024U})));
                        } else {
                            direct = std::move(std::get<std::unique_ptr<session::SessionWriter>>(
                                session::create_session(directory.path / "session", metadata)));
                        }
                        const auto all = Clock::now();
                        std::size_t ordinal = 0U;
                        for (auto& record : records) {
                            const auto scheduled = all + std::chrono::microseconds{static_cast<std::int64_t>(ordinal++) * 100};
                            if (paced) { std::this_thread::sleep_until(scheduled); }
                            const auto start = Clock::now();
                            if (async) {
                                if (background->try_append(std::move(record)) != session::CaptureAppendResult::queued) {
                                    throw std::runtime_error{"capture budget exhausted"};
                                }
                            } else if (direct->append(record)) { throw std::runtime_error{"journal append"}; }
                            times.push_back(elapsed(start));
                            if (paced) { late.push_back(std::chrono::duration<double, std::nano>{start - scheduled}.count()); }
                        }
                        const auto result = async ? background->finish() : direct->finalize();
                        const auto total = elapsed(all);
                        const auto* manifest = std::get_if<session::SessionManifest>(&result);
                        if (!manifest || manifest->records != count) { throw std::runtime_error{"capture finalization"}; }
                        if (expected.empty()) { expected = manifest->journal.sha256; }
                        if (manifest->journal.sha256 != expected) { throw std::runtime_error{"output mismatch"}; }
                        std::sort(times.begin(), times.end());
                        std::sort(late.begin(), late.end());
                        const auto percentile = [&](const std::size_t value) { return times[(count * value + 99U) / 100U - 1U]; };
                        std::cout << (paced ? "scheduled_100us" : "burst") << ',' << size << ',' << sample << ',' << (async ? "background" : "synchronous") << ',' << count << ','
                            << percentile(50U) << ',' << percentile(95U) << ',' << percentile(99U) << ','
                            << (paced ? late[(count * 99U + 99U) / 100U - 1U] : 0.0) << ',' << total << ','
                            << (async ? background->stats().retained_bytes_high_water : 0U) << ',' << expected << '\n';
                    }
                }
            }
        }
        return std::cout ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
