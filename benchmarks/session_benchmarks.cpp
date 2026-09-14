#include "eme/session/capture_session.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
namespace session = eme::session;

class Fixture final {
public:
    explicit Fixture(const std::size_t records) {
        path = std::filesystem::temp_directory_path() /
            ("eme-session-bench-" + std::to_string(Clock::now().time_since_epoch().count()));
        auto parsed = eme::gateway::kalshi::parse_metadata_snapshot(
            R"({"schema_version":1,"metadata_version":11,"venue":"kalshi","markets":[{"id":42,"ticker":"BENCH"}],"constraints":[]})");
        auto created = session::create_session(path,
            std::get<eme::gateway::kalshi::MetadataSnapshot>(parsed));
        if (std::holds_alternative<session::SessionError>(created)) {
            throw std::runtime_error{"fixture creation failed"};
        }
        auto& writer = *std::get<std::unique_ptr<session::SessionWriter>>(created);
        for (std::size_t index = 0U; index < records; ++index) {
            const auto sequence = static_cast<std::uint64_t>(index + 1U);
            eme::journal::RawMarketRecord record{
                eme::journal::current_schema_version, 11U, 1U, {}, {}, sequence, std::nullopt,
                "orderbook_snapshot", "{\"type\":\"orderbook_snapshot\",\"sid\":2,\"seq\":" +
                std::to_string(sequence) + ",\"msg\":{\"market_ticker\":\"BENCH\","
                "\"yes_dollars_fp\":[[\"0.4000\",\"1.00\"]],\"no_dollars_fp\":[]}}"};
            if (writer.append(record)) { throw std::runtime_error{"fixture append failed"}; }
        }
        const auto result = writer.finalize();
        if (std::holds_alternative<session::SessionError>(result)) {
            throw std::runtime_error{"fixture finalize failed"};
        }
        manifest = std::get<session::SessionManifest>(result);
    }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    std::filesystem::path path;
    session::SessionManifest manifest;
};

std::uint64_t raw_verify(const Fixture& fixture) {
    auto opened = eme::journal::open_raw_journal_reader(fixture.path / session::journal_filename);
    if (std::holds_alternative<eme::journal::JournalError>(opened)) {
        throw std::runtime_error{"raw open failed"};
    }
    auto& reader = *std::get<std::unique_ptr<eme::journal::RawJournalReader>>(opened);
    while (true) {
        const auto next = reader.read_next();
        if (std::holds_alternative<eme::journal::EndOfJournal>(next)) { break; }
        if (std::holds_alternative<eme::journal::JournalError>(next)) {
            throw std::runtime_error{"raw validation failed"};
        }
    }
    if (reader.records_read() != fixture.manifest.records) {
        throw std::runtime_error{"raw count mismatch"};
    }
    return reader.records_read();
}

std::uint64_t session_verify(const Fixture& fixture) {
    const auto result = session::verify_session(fixture.path);
    const auto* verified = std::get_if<session::VerifiedSession>(&result);
    if (!verified || verified->manifest != fixture.manifest ||
        verified->metadata.markets().find("BENCH") != 42U) {
        throw std::runtime_error{"session validation mismatch"};
    }
    return verified->manifest.records;
}

void measure(const Fixture& fixture, const std::size_t samples, const bool full_session) {
    std::vector<double> times;
    times.reserve(samples);
    std::uint64_t digest = 14'695'981'039'346'656'037ULL;
    for (std::size_t index = 0U; index < samples + 2U; ++index) {
        const auto start = Clock::now();
        const auto result = full_session ? session_verify(fixture) : raw_verify(fixture);
        const auto stop = Clock::now();
        digest = (digest ^ result) * 1'099'511'628'211ULL;
        if (index >= 2U) {
            times.push_back(std::chrono::duration<double, std::nano>{stop - start}.count());
        }
    }
    double total = 0.0;
    for (const auto value : times) { total += value; }
    std::sort(times.begin(), times.end());
    const auto percentile = [&](const std::size_t percent) {
        return times[(samples * percent + 99U) / 100U - 1U];
    };
    std::cout << (full_session ? "session_verify_" : "raw_journal_verify_")
              << fixture.manifest.records << ',' << samples << ",1," << fixture.manifest.journal.bytes
              << ',' << samples << ',' << std::fixed << std::setprecision(3)
              << total / static_cast<double>(samples) << ',' << percentile(50U) << ','
              << percentile(95U) << ',' << percentile(99U) << ',' << digest << '\n';
}
}  // namespace

int main(const int argc, const char* const argv[]) {
    std::size_t samples = 10U;
    bool smoke = false;
    if (argc == 2 && std::string_view{argv[1]} == "--smoke") { samples = 1U; smoke = true; }
    else if (argc == 3 && std::string_view{argv[1]} == "--samples") {
        const std::string_view value{argv[2]};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), samples);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
            samples == 0U || samples > 10'000U) { return 2; }
    } else if (argc != 1) { return 2; }
    try {
        std::cout << "# eme_benchmarks_v1,fixture=session_snapshot_v1,warmup_batches=2,file_cache=warm\n"
                  << "# benchmark_source_sha256=" << EME_SESSION_BENCH_SOURCE_SHA256 << '\n'
                  << "scenario,samples,batch_size,bytes_per_op,measured_ops,mean_ns_per_op,"
                     "p50_batch_ns_per_op,p95_batch_ns_per_op,p99_batch_ns_per_op,digest\n";
        if (smoke) { Fixture fixture{32U}; measure(fixture, samples, true); }
        else {
            for (const std::size_t count : {0U, 1'000U, 100'000U}) {
                Fixture fixture{count};
                measure(fixture, samples, false); measure(fixture, samples, true);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
    return std::cout ? 0 : 1;
}
