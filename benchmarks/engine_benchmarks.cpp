#include "eme/market/market_state.hpp"
#include "journal/journal_codec.hpp"
#ifdef EME_BENCH_KALSHI
#include "eme/gateway/kalshi/orderbook_processor.hpp"
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t warmup_batches = 16U;
constexpr std::size_t batch_size = 64U;
constexpr std::uint32_t fixture_seed = 0x51a7e123U;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

std::uint64_t mix(const std::uint64_t state, const std::uint64_t value) {
    return (state ^ value) * 1'099'511'628'211ULL;
}

template <typename Operation>
void measure(const std::string_view name, const std::size_t samples,
             const std::size_t batch, const std::size_t bytes_per_op,
             Operation operation) {
    std::uint64_t digest = 14'695'981'039'346'656'037ULL;
    std::size_t operation_index = 0U;
    for (std::size_t index = 0; index < warmup_batches * batch; ++index) {
        digest = mix(digest, operation(operation_index++));
    }
    std::vector<double> batch_means(samples);
    double elapsed_ns = 0.0;
    for (auto& sample : batch_means) {
        const auto start = Clock::now();
        for (std::size_t index = 0; index < batch; ++index) {
            digest = mix(digest, operation(operation_index++));
        }
        const auto end = Clock::now();
        const auto elapsed = std::chrono::duration<double, std::nano>{end - start}.count();
        elapsed_ns += elapsed;
        sample = elapsed / static_cast<double>(batch);
    }
    std::sort(batch_means.begin(), batch_means.end());
    const auto percentile = [&](const std::size_t percent) {
        return batch_means[(samples * percent + 99U) / 100U - 1U];
    };
    const auto operations = samples * batch;
    std::cout << name << ',' << samples << ',' << batch << ',' << bytes_per_op << ','
              << operations << ',' << std::fixed << std::setprecision(3)
              << elapsed_ns / static_cast<double>(operations) << ','
              << percentile(50U) << ',' << percentile(95U) << ',' << percentile(99U)
              << ',' << digest << '\n';
}

std::vector<std::vector<char>> payloads(const std::size_t size) {
    std::vector<std::vector<char>> result(32U, std::vector<char>(size));
    auto seed = fixture_seed;
    for (auto& bytes : result) {
        for (auto& value : bytes) {
            seed = seed * 1'664'525U + 1'013'904'223U;
            value = std::bit_cast<char>(static_cast<unsigned char>(seed >> 24U));
        }
    }
    return result;
}

void benchmark_journal(const std::size_t samples) {
    namespace codec = eme::journal::codec;
    for (const std::size_t size : {256U, 4096U, 65'536U}) {
        const auto buffers = payloads(size);
        const auto batch = size == 65'536U ? 4U : batch_size;
        measure("crc_" + std::to_string(size), samples, batch, size,
                [&](const std::size_t index) {
                    return codec::checksum(buffers[index % buffers.size()]);
                });
    }

    for (const std::size_t size : {256U, 4096U}) {
        const auto buffers = payloads(size);
        std::vector<eme::journal::RawMarketRecord> records;
        for (std::size_t index = 0; index < buffers.size(); ++index) {
            eme::journal::RawMarketRecord record;
            record.metadata_version = 1U;
            record.connection_generation = 1U;
            record.sequence = index;
            record.channel = "synthetic";
            record.payload.assign(buffers[index].begin(), buffers[index].end());
            records.push_back(std::move(record));
        }
        // Validate complete values outside the timed loop.
        for (const auto& record : records) {
            const auto encoded = codec::encode(record);
            const auto& bytes = std::get<std::vector<char>>(encoded);
            const auto decoded = codec::decode(bytes, 0U);
            require(std::get<eme::journal::RawMarketRecord>(decoded) == record,
                    "journal fixture does not round trip");
        }
        measure("journal_roundtrip_" + std::to_string(size), samples, batch_size, size,
                [&](const std::size_t index) {
                    auto encoded = codec::encode(records[index % records.size()]);
                    const auto& bytes = std::get<std::vector<char>>(encoded);
                    const auto crc = codec::checksum(bytes);
                    const auto decoded = codec::decode(bytes, index);
                    const auto& result = std::get<eme::journal::RawMarketRecord>(decoded);
                    return mix(crc, result.sequence + result.payload.size());
                });
    }
}

eme::core::Price price(const std::size_t value) {
    return *eme::core::Price::from_raw(static_cast<std::int64_t>(value));
}

void benchmark_books(const std::size_t samples, const bool churn) {
    constexpr std::size_t markets = 32U;
    constexpr std::size_t levels = 64U;
    eme::market::MarketState state;
    require(state.open_connection(1U), "book connection failed");
    for (std::size_t id = 1U; id <= markets; ++id) {
        eme::market::BookSnapshot snapshot;
        snapshot.market_id = static_cast<eme::market::MarketId>(id);
        snapshot.connection_generation = 1U;
        snapshot.stream_id = id;
        snapshot.sequence = 0U;
        for (std::size_t level = 0; level < levels; ++level) {
            snapshot.bids.push_back({price(4000U + level), *eme::core::Quantity::from_raw(1000)});
            snapshot.asks.push_back({price(6000U + level), *eme::core::Quantity::from_raw(1000)});
        }
        require(std::get<eme::book::BookUpdateResult>(state.apply(snapshot)) ==
                    eme::book::BookUpdateResult::applied, "book snapshot failed");
    }
    const auto count = (warmup_batches + samples) * batch_size;
    std::vector<eme::market::BookDelta> events;
    events.reserve(count);
    std::vector<std::int64_t> expected;
    expected.reserve(count);
    std::array<std::array<std::int64_t, levels>, markets> quantities{};
    for (auto& market : quantities) {
        market.fill(1000);
    }
    for (std::size_t index = 0; index < count; ++index) {
        const auto market = (index * 17U) % markets;
        const auto visit = index / markets;
        const auto level = (churn ? visit / 2U : visit) % levels;
        auto& quantity = quantities[market][level];
        const std::int64_t change = churn ? (visit % 2U == 0U ? -1000 : 1000)
                                         : ((visit / levels) % 2U == 0U ? 1 : -1);
        quantity += change;
        eme::market::BookDelta event{
            static_cast<eme::market::MarketId>(market + 1U),
            1U, market + 1U, visit + 1U, {}, eme::book::Side::bid,
            price(4000U + level), eme::core::QuantityDelta::from_raw(change)};
        events.push_back(event);
        expected.push_back(quantity);
    }
    measure(churn ? "book_churn_32x64" : "book_update_32x64", samples, batch_size, 0U,
            [&](const std::size_t index) {
                const auto& event = events[index];
                require(std::get<eme::book::BookUpdateResult>(state.apply(event)) ==
                            eme::book::BookUpdateResult::applied, "book delta failed");
                const auto* book = state.find_book(event.market_id);
                const auto quantity = book->quantity_at(event.side, event.price).raw();
                require(quantity == expected[index], "book quantity differs from reference");
                return mix(event.sequence, static_cast<std::uint64_t>(quantity));
            });
    // Validate every level, not just the level touched by the final update.
    for (std::size_t market = 0; market < markets; ++market) {
        const auto* book = state.find_book(static_cast<eme::market::MarketId>(market + 1U));
        require(book != nullptr && book->state() == eme::book::BookState::valid,
                "terminal book is invalid");
        for (std::size_t level = 0; level < levels; ++level) {
            require(book->quantity_at(eme::book::Side::bid, price(4000U + level)).raw() ==
                        quantities[market][level], "terminal bid differs from reference");
            require(book->quantity_at(eme::book::Side::ask, price(6000U + level)).raw() == 1000,
                    "untouched ask changed");
        }
    }
}

#ifdef EME_BENCH_KALSHI
void benchmark_gateway(const std::size_t samples, const bool journal_replay) {
    namespace kalshi = eme::gateway::kalshi;
    namespace journal = eme::journal;
    kalshi::MarketRegistry registry{1U};
    require(registry.register_market(1U, "SYNTHETIC") ==
                kalshi::MarketRegistrationResult::registered, "registry failed");
    kalshi::OrderBookProcessor processor{registry};
    require(processor.open_connection(1U), "gateway connection failed");
    journal::RawMarketRecord snapshot;
    snapshot.metadata_version = 1U;
    snapshot.connection_generation = 1U;
    snapshot.channel = "orderbook_snapshot";
    snapshot.payload = R"({"type":"orderbook_snapshot","sid":1,"seq":0,"msg":{"market_ticker":"SYNTHETIC","yes_dollars_fp":[["0.4000","10.00"]],"no_dollars_fp":[["0.6000","10.00"]]}})";
    require(std::get<eme::book::BookUpdateResult>(processor.process(snapshot)) ==
                eme::book::BookUpdateResult::applied, "gateway snapshot failed");
    const auto count = (warmup_batches + samples) * batch_size;
    std::vector<journal::RawMarketRecord> records;
    std::vector<std::vector<char>> encoded;
    std::vector<std::uint32_t> checksums;
    records.reserve(count);
    encoded.reserve(count);
    checksums.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        auto record = snapshot;
        record.channel = "orderbook_delta";
        record.sequence = index + 1U;
        record.payload = "{\"type\":\"orderbook_delta\",\"sid\":1,\"seq\":" +
            std::to_string(record.sequence) +
            ",\"msg\":{\"market_ticker\":\"SYNTHETIC\",\"price_dollars\":\"0.4000\",\"delta_fp\":\"" +
            (index % 2U == 0U ? "1.00" : "-1.00") + "\",\"side\":\"yes\"}}";
        if (journal_replay) {
            encoded.push_back(std::get<std::vector<char>>(journal::codec::encode(record)));
            checksums.push_back(journal::codec::checksum(encoded.back()));
        }
        records.push_back(std::move(record));
    }
    measure(journal_replay ? "verified_memory_replay" : "kalshi_decode_normalize_apply",
            samples, batch_size, 0U, [&](const std::size_t index) {
                kalshi::ProcessingResult result;
                if (journal_replay) {
                    require(journal::codec::checksum(encoded[index]) == checksums[index],
                            "replay checksum differs");
                    auto decoded = journal::codec::decode(encoded[index], index);
                    result = processor.process(std::get<journal::RawMarketRecord>(decoded));
                } else {
                    result = processor.process(records[index]);
                }
                require(std::get<eme::book::BookUpdateResult>(result) ==
                            eme::book::BookUpdateResult::applied, "gateway delta failed");
                const auto* book = processor.state().find_book(1U);
                const auto quantity = book->quantity_at(eme::book::Side::bid, price(4000U)).raw();
                require(quantity == (index % 2U == 0U ? 1100 : 1000), "replay quantity differs");
                require(book->last_sequence() == index + 1U, "replay sequence differs");
                return mix(*book->last_sequence(), static_cast<std::uint64_t>(quantity));
            });
}
#endif

}  // namespace

int main(const int argc, const char* const argv[]) {
    try {
        std::size_t samples = 200U;
        if (argc == 2 && std::string_view{argv[1]} == "--smoke") {
            samples = 3U;
        } else if (argc == 3 && std::string_view{argv[1]} == "--samples") {
            const std::string_view value{argv[2]};
            const auto result = std::from_chars(value.data(), value.data() + value.size(), samples);
            require(result.ec == std::errc{} && result.ptr == value.data() + value.size() &&
                        samples >= 1U && samples <= 10'000U, "samples must be in [1, 10000]");
        } else if (argc != 1) {
            throw std::runtime_error{"usage: eme_benchmarks [--smoke | --samples N]"};
        }
        std::cout << "# eme_benchmarks_v1,seed=" << fixture_seed
                  << ",warmup_batches=" << warmup_batches << '\n'
                  << "# benchmark_source_sha256=" << EME_BENCH_SOURCE_SHA256 << '\n'
                  << "scenario,samples,batch_size,bytes_per_op,measured_ops,mean_ns_per_op,"
                     "p50_batch_ns_per_op,p95_batch_ns_per_op,p99_batch_ns_per_op,digest\n";
        benchmark_journal(samples);
        benchmark_books(samples, false);
        benchmark_books(samples, true);
#ifdef EME_BENCH_KALSHI
        benchmark_gateway(samples, false);
        benchmark_gateway(samples, true);
#endif
    } catch (const std::exception& error) {
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
