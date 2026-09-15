#include "eme/session/readonly_feed.hpp"
#include "test_support.hpp"
#include <array>
#include <limits>

using namespace eme;
int main() {
    test::Context test;
    gateway::kalshi::MarketRegistry markets{1U};
    (void)markets.register_market(1U, "A");
    (void)markets.register_market(2U, "B");
    const std::array<market::MarketId, 2> selection{1U, 2U};
    const auto exercise = [&](std::string bad) {
        session::ReadOnlyFeed feed{markets, selection};
        journal::RawMarketRecord record;
        record.metadata_version = 1U;
        record.connection_generation = 1U;
        const auto apply = [&](std::string channel, std::string payload) {
            record.channel = std::move(channel); record.payload = std::move(payload);
            record.received_at += std::chrono::nanoseconds{1};
            return feed.accept(record);
        };
        test.expect(apply("ws.attempt.v1", "{}").event == session::FeedEvent::control, "attempt");
        test.expect(apply("ws.open.v1", "{}").event == session::FeedEvent::control, "open");
        for (const auto& command : feed.commands()) { test.expect(apply("ws.send.v1", command).event == session::FeedEvent::control, "send"); }
        test.expect(apply("ws.receive.v1", R"({"type":"subscribed","id":2,"msg":{"channel":"orderbook_delta","sid":12}})").event == session::FeedEvent::control, "out-of-order command acknowledgements");
        test.expect(apply("ws.receive.v1", R"({"type":"subscribed","id":1,"msg":{"channel":"orderbook_delta","sid":11}})").event == session::FeedEvent::control, "acknowledgement");
        test.expect(apply("ws.receive.v1", R"({"type":"orderbook_snapshot","sid":11,"seq":40,"msg":{"market_ticker":"A","yes_dollars_fp":[["0.7000","5.00"]],"no_dollars_fp":[]}})").event == session::FeedEvent::market, "arbitrary initial wire sequence");
        test.expect(apply("ws.receive.v1", R"({"type":"orderbook_snapshot","sid":12,"seq":8,"msg":{"market_ticker":"B","yes_dollars_fp":[],"no_dollars_fp":[["0.6000","5.00"]]}})").event == session::FeedEvent::market, "independent subscription sequence");
        test.expect(apply("ws.receive.v1", R"({"type":"orderbook_delta","sid":11,"seq":41,"msg":{"market_ticker":"A","side":"yes","price_dollars":"0.7000","delta_fp":"1.00"}})").event == session::FeedEvent::market, "interleaved subscriptions preserve sequence");
        test.expect(feed.state().valid_book_count() == 2U, "two valid books");
        test.expect(apply("ws.receive.v1", std::move(bad)).event == session::FeedEvent::invalidated, "invalid feed fails generation");
        test.expect(!feed.healthy() && feed.state().valid_book_count() == 0U, "no stale tradable books after failure");
        test.expect(apply("ws.close.v1", "{}").event == session::FeedEvent::control, "explicit terminal close");
        record.connection_generation = 2U;
        test.expect(apply("ws.attempt.v1", "{}").event == session::FeedEvent::control, "new generation");
        test.expect(apply("ws.open.v1", "{}").event == session::FeedEvent::control, "reconnected");
        test.expect(feed.state().valid_book_count() == 0U, "recovery needs fresh snapshots");
        for (const auto& command : feed.commands()) { (void)apply("ws.send.v1", command); }
        (void)apply("ws.receive.v1", R"({"type":"subscribed","id":1,"msg":{"channel":"orderbook_delta","sid":11}})");
        test.expect(apply("ws.receive.v1", R"({"type":"orderbook_snapshot","sid":11,"seq":1,"msg":{"market_ticker":"A","yes_dollars_fp":[["0.7000","5.00"]],"no_dollars_fp":[]}})").event == session::FeedEvent::market, "recovery resets wire sequence with reused subscription ID");
        feed.abort();
        test.expect(!feed.healthy() && feed.state().valid_book_count() == 0U, "recorder failure invalidates without a writable journal");
        test.expect(apply("ws.close.v1", "{}").event == session::FeedEvent::control, "close recovered connection");
    };
    for (const auto* bad : {
        R"({"type":"orderbook_delta","sid":11,"seq":43,"msg":{"market_ticker":"A","side":"yes","price_dollars":"0.7000","delta_fp":"1.00"}})",
        R"({"type":"orderbook_delta","sid":11,"seq":41,"msg":{"market_ticker":"A","side":"yes","price_dollars":"0.7000","delta_fp":"1.00"}})",
        R"({"type":"orderbook_delta","sid":12,"seq":9,"msg":{"market_ticker":"A","side":"yes","price_dollars":"0.7000","delta_fp":"1.00"}})",
        R"({"type":"subscribed","id":1,"msg":{"channel":"orderbook_delta","sid":11}})",
        R"({"type":"error","sid":11,"seq":42,"msg":{"code":25}})",
        R"({"type":"ok","sid":11,"seq":42,"msg":{}})",
        R"({"type":"orderbook_delta","type":"error"})", "not JSON", "[]"
    }) { exercise(bad); }
    return test.result();
}
