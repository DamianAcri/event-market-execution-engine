#include "eme/session/readonly_feed.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>

using namespace eme;
int main(int argc, char**) {
    constexpr std::uint64_t samples = 10'000U;
    const unsigned runs = argc > 1 ? 2U : 16U;
    gateway::kalshi::MarketRegistry markets{1U};
    (void)markets.register_market(1U, "A");
    const std::array<market::MarketId, 1> selected{1U};
    std::vector<journal::RawMarketRecord> records;
    for (std::uint64_t i = 0; i < samples; ++i) {
        journal::RawMarketRecord record;
        record.metadata_version = record.connection_generation = 1U;
        record.channel = "ws.receive.v1";
        record.payload = "{\"type\":\"orderbook_delta\",\"sid\":11,\"seq\":" + std::to_string(i + 2U) +
            ",\"msg\":{\"market_ticker\":\"A\",\"side\":\"yes\",\"price_dollars\":\"0.5000\",\"delta_fp\":\"0.01\"}}";
        records.push_back(std::move(record));
    }
    std::cout << "protocol,run,records,p50_ns,p99_ns,total_ns,final_quantity,final_sequence\n";
    for (unsigned run = 0U; run < runs; ++run) {
        const bool shared = run % 2U != 0U;
        session::ReadOnlyFeed feed{markets, selected, shared
            ? session::FeedProtocol::shared_subscription_v1 : session::FeedProtocol::per_market_v1};
        journal::RawMarketRecord control;
        control.metadata_version = control.connection_generation = 1U;
        const auto apply = [&](std::string channel, std::string payload) {
            control.channel = std::move(channel); control.payload = std::move(payload);
            const auto result = feed.accept(control);
            if (result.event == session::FeedEvent::invalid_history || result.event == session::FeedEvent::invalidated) { std::exit(1); }
        };
        apply("ws.attempt.v1", "{}"); apply("ws.open.v1", "{}");
        apply("ws.send.v1", feed.commands().front());
        apply("ws.receive.v1", R"({"type":"subscribed","id":1,"msg":{"channel":"orderbook_delta","sid":11}})");
        apply("ws.receive.v1", R"({"type":"orderbook_snapshot","sid":11,"seq":1,"msg":{"market_ticker":"A","yes_dollars_fp":[["0.5000","1.00"]],"no_dollars_fp":[]}})");
        std::vector<std::int64_t> elapsed;
        elapsed.reserve(samples);
        const auto start = std::chrono::steady_clock::now();
        for (const auto& record : records) {
            const auto before = std::chrono::steady_clock::now();
            if (feed.accept(record).event != session::FeedEvent::market) { return 1; }
            elapsed.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - before).count());
        }
        const auto total = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        std::sort(elapsed.begin(), elapsed.end());
        const auto* book = feed.state().find_book(1U);
        const auto quantity = book->quantity_at(book::Side::bid, *core::Price::from_raw(5000)).raw();
        if (quantity != 10'100 || book->last_sequence() != 10'001U) { return 1; }
        std::cout << (shared ? "shared" : "legacy") << ',' << run << ',' << samples << ',' << elapsed[samples / 2U] << ',' << elapsed[samples * 99U / 100U] << ','
                  << total << ',' << quantity << ',' << *book->last_sequence() << '\n';
    }
}
