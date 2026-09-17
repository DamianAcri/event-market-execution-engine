#include "eme/session/readonly_feed.hpp"
#include "eme/session/replay.hpp"
#include "test_support.hpp"
#include <array>
#include <filesystem>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

using namespace eme;

namespace {
constexpr auto first_snapshot = R"({"type":"orderbook_snapshot","sid":11,"seq":40,"msg":{"market_ticker":"A","yes_dollars_fp":[["0.7000","5.00"]],"no_dollars_fp":[["0.7500","4.00"]]}})";
constexpr auto second_snapshot = R"({"type":"orderbook_snapshot","sid":11,"seq":41,"msg":{"market_ticker":"B","yes_dollars_fp":[["0.5000","3.00"]],"no_dollars_fp":[["0.6000","2.00"]]}})";
constexpr auto book_ack = R"({"type":"subscribed","id":1,"msg":{"channel":"orderbook_delta","sid":11}})";
constexpr auto trade_ack = R"({"type":"subscribed","id":2,"msg":{"channel":"trade","sid":12}})";
constexpr auto observed_trade = R"({"type":"trade","sid":12,"seq":80,"msg":{"trade_id":"public-trade-1","market_ticker":"A","yes_price_dollars":"0.3600","no_price_dollars":"0.6400","count_fp":"136.25","taker_side":"no","taker_outcome_side":"no","is_block_trade":false,"ts":1669149841,"ts_ms":1669149841123}})";

std::string replace(std::string text, const std::string_view from, const std::string_view to) {
    const auto at = text.find(from);
    if (at == std::string::npos) { throw std::runtime_error{"missing trade fixture field"}; }
    text.replace(at, from.size(), to);
    return text;
}

struct TradeFixture final {
    TradeFixture(const gateway::kalshi::MarketRegistry& markets,
                 const std::span<const market::MarketId> selection)
        : feed{markets, selection, session::FeedProtocol::book_and_trades_v1} {
        record.metadata_version = record.connection_generation = 1U;
    }
    session::FeedUpdate apply(const std::string_view channel, const std::string_view payload) {
        record.channel = channel;
        record.payload = payload;
        record.received_at += std::chrono::nanoseconds{1};
        records.push_back(record);
        return feed.accept(record);
    }
    session::FeedUpdate receive(const std::string_view payload) { return apply("ws.receive.v1", payload); }
    void start() {
        (void)apply("ws.attempt.v1", "{}");
        (void)apply("ws.open.v1", "{}");
        for (const auto& command : feed.commands()) { (void)apply("ws.send.v1", command); }
    }
    void ready() {
        start();
        (void)receive(trade_ack);
        (void)receive(book_ack);
        (void)receive(first_snapshot);
        (void)receive(second_snapshot);
    }
    session::ReadOnlyFeed feed;
    journal::RawMarketRecord record;
    std::vector<journal::RawMarketRecord> records;
};

void public_trade_subscriptions(test::Context& test,
    const gateway::kalshi::MarketRegistry& markets, const std::span<const market::MarketId> selection) {
    TradeFixture fixture{markets, selection};
    const auto& commands = fixture.feed.commands();
    test.expect(commands.size() == 2U &&
        commands[0] == R"({"cmd":"subscribe","id":1,"params":{"channels":["orderbook_delta"],"market_tickers":["A","B"],"use_yes_price":true}})" &&
        commands[1] == R"({"cmd":"subscribe","id":2,"params":{"channels":["trade"],"market_tickers":["A","B"]}})",
        "book and public trades subscribe separately to the same bounded selection");
    (void)fixture.apply("ws.attempt.v1", "{}");
    (void)fixture.apply("ws.open.v1", "{}");
    test.expect(!fixture.feed.ready(), "open socket is not ready before subscriptions and snapshots");
    (void)fixture.apply("ws.send.v1", commands[0]);
    (void)fixture.receive(book_ack);
    (void)fixture.receive(first_snapshot);
    (void)fixture.receive(second_snapshot);
    test.expect(!fixture.feed.ready() && fixture.feed.state().valid_book_count() == 2U,
        "all books alone do not make an unacknowledged trade subscription ready");
    test.expect(fixture.apply("ws.send.v1", commands[1]).event == session::FeedEvent::control &&
        fixture.feed.state().valid_book_count() == 2U &&
        fixture.feed.state().find_book(1U)->last_sequence() == 40U,
        "sending public-trade subscription never resets already received books");
    test.expect(fixture.receive(trade_ack).event == session::FeedEvent::control && fixture.feed.ready(),
        "readiness requires both acknowledgements and all snapshots");

    TradeFixture reversed{markets, selection};
    reversed.start();
    test.expect(reversed.receive(trade_ack).event == session::FeedEvent::control,
        "trade acknowledgement may precede book acknowledgement");
    const auto early = reversed.receive(observed_trade);
    test.expect(early.event == session::FeedEvent::public_trade && early.trade &&
        !reversed.feed.ready() && reversed.feed.state().book_count() == 0U,
        "public trade received before snapshots is observable but never creates a book");
    (void)reversed.receive(book_ack);
    (void)reversed.receive(first_snapshot);
    (void)reversed.receive(second_snapshot);
    test.expect(reversed.feed.ready(), "reversed acknowledgements reach the same readiness state");

    for (const auto* bad : {
        R"({"type":"subscribed","id":2,"msg":{"channel":"orderbook_delta","sid":12}})",
        R"({"type":"subscribed","id":1,"msg":{"channel":"trade","sid":11}})",
        R"({"type":"subscribed","id":3,"msg":{"channel":"trade","sid":13}})",
        R"({"type":"subscribed","id":0,"msg":{"channel":"trade","sid":12}})",
        R"({"type":"subscribed","id":2,"msg":{"channel":"trade","sid":0}})"
    }) {
        TradeFixture invalid{markets, selection};
        invalid.start();
        test.expect(invalid.receive(bad).event == session::FeedEvent::invalidated && !invalid.feed.ready(),
            "unexpected channel, command id or subscription id invalidates the generation");
    }
    TradeFixture collision{markets, selection};
    collision.start();
    (void)collision.receive(book_ack);
    test.expect(collision.receive(R"({"type":"subscribed","id":2,"msg":{"channel":"trade","sid":11}})").event == session::FeedEvent::invalidated,
        "book and trade acknowledgements cannot share a subscription id");
    TradeFixture unacknowledged{markets, selection};
    unacknowledged.start();
    (void)unacknowledged.receive(book_ack);
    test.expect(unacknowledged.receive(observed_trade).event == session::FeedEvent::invalidated,
        "unacknowledged public-trade messages are not accepted");
}

void public_trade_normalization(test::Context& test,
    const gateway::kalshi::MarketRegistry& markets, const std::span<const market::MarketId> selection) {
    TradeFixture fixture{markets, selection};
    fixture.ready();
    const auto update = fixture.receive(observed_trade);
    test.expect(update.event == session::FeedEvent::public_trade && update.market_id == 1U && update.trade,
        "selected public trade has a distinct normalized event");
    if (update.trade) {
        const auto& trade = *update.trade;
        test.expect(trade.market_id == 1U && trade.trade_id == "public-trade-1" &&
            trade.yes_price_1e4 == 3600 && trade.quantity_centicontracts == 13625 &&
            trade.exchange_time_ms == 1669149841123U && !trade.taker_yes &&
            trade.block_trade.has_value() && !*trade.block_trade,
            "NO taker retains YES price, exact fractional quantity, milliseconds and false block flag");
    }
    test.expect(fixture.feed.state().find_book(1U)->last_sequence() == 40U &&
        fixture.feed.state().find_book(2U)->last_sequence() == 41U &&
        fixture.feed.state().find_book(1U)->quantity_at(book::Side::bid, test::price(7000)).raw() == 500 &&
        fixture.feed.state().find_book(1U)->quantity_at(book::Side::ask, test::price(7500)).raw() == 400,
        "observed trade never decrements depth or overwrites a book's last sequence");
    const auto delta = fixture.receive(R"({"type":"orderbook_delta","sid":11,"seq":42,"msg":{"market_ticker":"A","side":"yes","price_dollars":"0.7000","delta_fp":"-1.00"}})");
    test.expect(delta.event == session::FeedEvent::market && !delta.trade &&
        fixture.feed.state().find_book(1U)->quantity_at(book::Side::bid, test::price(7000)).raw() == 400,
        "book sequence continues independently after a public trade and changes depth exactly once");
    auto second = replace(observed_trade, "\"seq\":80", "\"seq\":81");
    second = replace(second, "public-trade-1", "public-trade-2");
    second = replace(second, "\"market_ticker\":\"A\"", "\"market_ticker\":\"B\"");
    second = replace(second, "\"taker_side\":\"no\",", "");
    second = replace(second, "\"taker_outcome_side\":\"no\"", "\"taker_outcome_side\":\"yes\"");
    second = replace(second, ",\"ts_ms\":1669149841123", "");
    second = replace(second, ",\"is_block_trade\":false", "");
    const auto next = fixture.receive(second);
    test.expect(next.event == session::FeedEvent::public_trade && next.trade &&
        next.trade->market_id == 2U && next.trade->taker_yes &&
        next.trade->exchange_time_ms == 1669149841000U && !next.trade->block_trade.has_value(),
        "trade sequence is shared across selected markets; absent milliseconds and block flag retain documented meaning");
    auto third = replace(observed_trade, "\"seq\":80", "\"seq\":82");
    third = replace(third, "public-trade-1", "public-trade-3");
    third = replace(third, ",\"taker_outcome_side\":\"no\"", "");
    third = replace(third, "\"is_block_trade\":false", "\"is_block_trade\":true");
    const auto legacy = fixture.receive(third);
    test.expect(legacy.trade && !legacy.trade->taker_yes && legacy.trade->block_trade == true,
        "legacy taker_side and explicit block-trade flag are preserved");
    auto millis_only = replace(observed_trade, "\"seq\":80", "\"seq\":83");
    millis_only = replace(millis_only, "public-trade-1", "public-trade-4");
    millis_only = replace(millis_only, "\"ts\":1669149841,", "");
    millis_only = replace(millis_only, "\"taker_side\":\"no\"", "\"taker_side\":\"no\",\"taker_book_side\":\"ask\"");
    const auto current = fixture.receive(millis_only);
    test.expect(current.trade && current.trade->exchange_time_ms == 1669149841123U && !current.trade->taker_yes,
        "current trade schema accepts milliseconds alone and consistent taker book side");
    test.expect(fixture.feed.ready() && fixture.feed.state().find_book(1U)->last_sequence() == 42U &&
        fixture.feed.state().find_book(2U)->last_sequence() == 41U,
        "interleaved public trades leave current books ready and their sequence unchanged");
    (void)fixture.apply("ws.close.v1", "{}");
    test.expect(!fixture.feed.ready() && fixture.feed.state().valid_book_count() == 0U,
        "disconnect revokes readiness even after valid book and trade traffic");
    fixture.record.connection_generation = 2U;
    fixture.ready();
    auto reset = replace(observed_trade, "\"seq\":80", "\"seq\":1");
    test.expect(fixture.receive(reset).event == session::FeedEvent::public_trade && fixture.feed.ready(),
        "reconnect resets trade sequence even when the venue reuses subscription ids");
}

void public_trade_rejections(test::Context& test,
    const gateway::kalshi::MarketRegistry& markets, const std::span<const market::MarketId> selection) {
    const auto invalid = [&](const std::string& payload, const std::string_view message) {
        TradeFixture fixture{markets, selection};
        fixture.ready();
        test.expect(fixture.receive(payload).event == session::FeedEvent::invalidated &&
            !fixture.feed.healthy() && !fixture.feed.ready() && fixture.feed.state().valid_book_count() == 0U,
            message);
    };
    for (const auto& [from, to] : std::array<std::pair<std::string_view, std::string_view>, 31>{{
        {"\"sid\":12", "\"sid\":11"},
        {"\"sid\":12", "\"sid\":13"},
        {"\"seq\":80", "\"seq\":0"},
        {"\"seq\":80", "\"seq\":-1"},
        {"\"market_ticker\":\"A\"", "\"market_ticker\":\"UNKNOWN\""},
        {"\"market_ticker\":\"A\"", "\"market_ticker\":\"C\""},
        {"\"yes_price_dollars\":\"0.3600\"", "\"yes_price_dollars\":\"1.3600\""},
        {"\"yes_price_dollars\":\"0.3600\"", "\"yes_price_dollars\":0.36"},
        {"\"no_price_dollars\":\"0.6400\"", "\"no_price_dollars\":\"0.6300\""},
        {"\"count_fp\":\"136.25\"", "\"count_fp\":\"0.00\""},
        {"\"count_fp\":\"136.25\"", "\"count_fp\":\"-1.00\""},
        {"\"count_fp\":\"136.25\"", "\"count_fp\":\"1.001\""},
        {"\"count_fp\":\"136.25\"", "\"count_fp\":1"},
        {"\"trade_id\":\"public-trade-1\"", "\"trade_id\":\"\""},
        {"\"trade_id\":\"public-trade-1\"", "\"trade_id\":null"},
        {"\"taker_side\":\"no\"", "\"taker_side\":\"yes\""},
        {"\"taker_side\":\"no\",\"taker_outcome_side\":\"no\"", "\"taker_side\":\"bid\""},
        {"\"taker_side\":\"no\",\"taker_outcome_side\":\"no\",", ""},
        {"\"taker_side\":\"no\"", "\"taker_side\":\"no\",\"taker_book_side\":\"bid\""},
        {"\"taker_side\":\"no\"", "\"taker_side\":\"no\",\"taker_book_side\":\"unknown\""},
        {"\"taker_side\":\"no\"", "\"taker_side\":\"no\",\"taker_book_side\":null"},
        {"\"is_block_trade\":false", "\"is_block_trade\":0"},
        {"\"is_block_trade\":false", "\"is_block_trade\":null"},
        {"\"ts\":1669149841", "\"ts\":-1"},
        {"\"ts\":1669149841", "\"ts\":1669149841.5"},
        {"\"ts\":1669149841", "\"ts\":\"1669149841\""},
        {"\"ts_ms\":1669149841123", "\"ts_ms\":1669149842123"},
        {"\"ts_ms\":1669149841123", "\"ts_ms\":-1"},
        {"\"ts_ms\":1669149841123", "\"ts_ms\":1669149841123.5"},
        {"\"ts\":1669149841", "\"ts\":18446744073709552"},
        {"\"seq\":80", "\"seq\":80,\"seq\":80"}
    }}) {
        invalid(replace(observed_trade, from, to), "malformed public trade invalidates every book");
    }
    invalid(replace(observed_trade, "public-trade-1", std::string(129U, 'x')),
        "oversized public trade identity is rejected");
    invalid(R"({"type":"orderbook_delta","sid":12,"seq":42,"msg":{"market_ticker":"A","side":"yes","price_dollars":"0.7000","delta_fp":"1.00"}})",
        "book delta cannot consume the trade subscription's sequence");
    for (const auto sequence : {"80", "79", "82"}) {
        TradeFixture fixture{markets, selection};
        fixture.ready();
        (void)fixture.receive(observed_trade);
        test.expect(fixture.receive(replace(observed_trade, "\"seq\":80", std::string{"\"seq\":"} + sequence)).event == session::FeedEvent::invalidated &&
            fixture.feed.state().valid_book_count() == 0U,
            "duplicate, regressing or gapped trade sequences invalidate the generation");
    }
    TradeFixture overflow{markets, selection};
    overflow.ready();
    const auto maximum = replace(observed_trade, "\"seq\":80", "\"seq\":" + std::to_string(std::numeric_limits<std::uint64_t>::max()));
    test.expect(overflow.receive(maximum).event == session::FeedEvent::public_trade &&
        overflow.receive(replace(observed_trade, "\"seq\":80", "\"seq\":1")).event == session::FeedEvent::invalidated,
        "maximum initial trade sequence never permits unsigned wraparound");
}

void public_trade_replay(test::Context& test,
    const gateway::kalshi::MarketRegistry& markets, const std::span<const market::MarketId> selection) {
    struct Directory final {
        Directory() : path{std::filesystem::temp_directory_path() / ("eme-public-trade-replay-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))} {}
        ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
        std::filesystem::path path;
    } directory;
    TradeFixture fixture{markets, selection};
    fixture.ready();
    const auto books_time = fixture.record.received_at.time_since_epoch().count();
    fixture.record.received_at += std::chrono::seconds{30};
    (void)fixture.receive(observed_trade);
    (void)fixture.receive(replace(observed_trade, "\"seq\":80", "\"seq\":81"));
    (void)fixture.apply("ws.close.v1", "{}");
    const auto parsed = gateway::kalshi::parse_metadata_snapshot(R"({"schema_version":1,"metadata_version":1,"venue":"kalshi",
        "markets":[{"id":1,"ticker":"A"},{"id":2,"ticker":"B"}],"constraints":[
        {"id":7,"semantic_version":1,"key":"a-implies-b","provenance":"Synthetic trade isolation fixture",
         "relationship":{"type":"implication","antecedent":1,"consequent":2}}]})");
    auto created = session::create_session(directory.path, std::get<gateway::kalshi::MetadataSnapshot>(parsed));
    auto& writer = *std::get<std::unique_ptr<session::SessionWriter>>(created);
    for (const auto& record : fixture.records) {
        if (writer.append(record)) { throw std::runtime_error{"public trade fixture append failed"}; }
    }
    test.expect(std::holds_alternative<session::SessionManifest>(writer.finalize()),
        "mixed book and trade journal finalizes normally");
    auto verified = session::verify_session(directory.path);
    session::ReplayPlan plan;
    plan.source_kind = "synthetic";
    plan.provenance = "Public trade replay isolation fixture";
    plan.ws_controller = plan.shared_subscription = plan.public_trades = true;
    plan.markets.assign(selection.begin(), selection.end());
    session::ReplayInput input{directory.path, std::get<session::VerifiedSession>(std::move(verified)), std::move(plan)};
    struct Observer final : session::ReplayObserver {
        explicit Observer(test::Context& context, const std::int64_t book_time)
            : test{context}, expected_book_time{book_time} {}
        void before(const std::int64_t time, const market::MarketState&) override { before_time = time; }
        void after(const session::ReplayFrame& frame, const std::span<const opportunity::CandidateEvent> events,
                   const market::MarketState& state) override {
            if (frame.applied) { last_book_time = frame.time_ns; ++book_updates; }
            if (!frame.trade) { return; }
            ++trades;
            test.expect(!frame.applied && events.empty() && frame.market_id == 1U,
                "public trade replay is observable without triggering book freshness or candidate updates");
            test.expect(before_time == frame.time_ns && frame.time_ns > expected_book_time + 29'000'000'000LL &&
                last_book_time == expected_book_time,
                "trade arrival advances causal time but thirty seconds of trades cannot freshen an old book");
            test.expect(state.find_book(1U)->last_sequence() == 40U &&
                state.find_book(1U)->quantity_at(book::Side::bid, test::price(7000)).raw() == 500 &&
                state.find_book(2U)->last_sequence() == 41U,
                "replayed trade retains the same book as the live normalization path");
        }
        test::Context& test;
        std::int64_t expected_book_time{};
        std::int64_t before_time{};
        std::int64_t last_book_time{};
        std::uint64_t book_updates{};
        std::uint64_t trades{};
    } observer{test, books_time};
    std::ostringstream output;
    const auto replayed = session::replay(input, observer, &output);
    const auto* summary = std::get_if<session::ReplaySummary>(&replayed);
    test.expect(summary && summary->public_trades == 2U && summary->rejected_updates == 0U &&
        summary->records == fixture.records.size() && summary->candidate_events == 2U &&
        observer.book_updates == 2U && observer.trades == 2U,
        "replay counts public trades separately; the candidate opens on a snapshot and invalidates only on close");
    test.expect(output.str().find("\"public_trades\":2") != std::string::npos &&
        output.str().find("\"type\":\"public_trade\"") != std::string::npos,
        "mixed replay exports normalized trades and their separate completion count");
    Observer repeated{test, books_time};
    std::ostringstream second;
    test.expect(std::holds_alternative<session::ReplaySummary>(session::replay(input, repeated, &second)) &&
        second.str() == output.str(), "mixed public-trade replay is byte-for-byte deterministic");
}
} // namespace

int main() {
    test::Context test;
    gateway::kalshi::MarketRegistry markets{1U};
    (void)markets.register_market(1U, "A");
    (void)markets.register_market(2U, "B");
    (void)markets.register_market(3U, "C");
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
    session::ReadOnlyFeed zero_feed{markets, selection};
    journal::RawMarketRecord zero;
    zero.metadata_version = zero.connection_generation = 1U;
    const auto initial = [&](std::string channel, std::string payload) {
        zero.channel = std::move(channel); zero.payload = std::move(payload);
        return zero_feed.accept(zero);
    };
    (void)initial("ws.attempt.v1", "{}"); (void)initial("ws.open.v1", "{}");
    (void)initial("ws.send.v1", zero_feed.commands().front());
    (void)initial("ws.receive.v1", R"({"type":"subscribed","id":1,"msg":{"channel":"orderbook_delta","sid":11}})");
    test.expect(initial("ws.receive.v1", R"({"type":"orderbook_snapshot","sid":11,"seq":0,"msg":{"market_ticker":"A","yes_dollars_fp":[],"no_dollars_fp":[]}})").event == session::FeedEvent::invalidated && zero_feed.state().valid_book_count() == 0U,
        "WS initial sequence must be positive even though legacy books can represent zero");

    // A single venue subscription owns the sequence across both market books.
    // Preserve the old protocol exercises above as replay-compatibility checks.
    for (const auto* bad : {
        R"({"type":"orderbook_delta","sid":11,"seq":44,"msg":{"market_ticker":"A","side":"yes","price_dollars":"0.7000","delta_fp":"1.00"}})",
        R"({"type":"orderbook_delta","sid":11,"seq":42,"msg":{"market_ticker":"B","side":"yes","price_dollars":"0.7000","delta_fp":"1.00"}})",
        R"({"type":"orderbook_delta","sid":12,"seq":43,"msg":{"market_ticker":"A","side":"yes","price_dollars":"0.7000","delta_fp":"1.00"}})",
        R"({"type":"orderbook_delta","sid":11,"seq":43,"msg":{"market_ticker":"UNKNOWN","side":"yes","price_dollars":"0.7000","delta_fp":"1.00"}})",
        R"({"type":"orderbook_snapshot","sid":11,"seq":43,"msg":{"market_ticker":"B","yes_dollars_fp":[],"no_dollars_fp":[]}})",
        R"({"type":"subscribed","id":1,"msg":{"channel":"orderbook_delta","sid":11}})",
        R"({"type":"ok","id":2,"sid":11,"seq":43,"msg":{"market_tickers":["A","B"]}})",
        R"({"type":"error","sid":11,"seq":43,"msg":{"code":25}})",
        R"({"type":"orderbook_delta","type":"error"})"
    }) {
        session::ReadOnlyFeed feed{markets, selection, session::FeedProtocol::shared_subscription_v1};
        journal::RawMarketRecord record;
        record.metadata_version = record.connection_generation = 1U;
        const auto apply = [&](std::string channel, std::string payload) {
            record.channel = std::move(channel); record.payload = std::move(payload);
            record.received_at += std::chrono::nanoseconds{1};
            return feed.accept(record);
        };
        test.expect(feed.commands().size() == 1U && feed.commands()[0].find("market_tickers") != std::string::npos,
                    "shared subscription declares the entire selection once");
        (void)apply("ws.attempt.v1", "{}"); (void)apply("ws.open.v1", "{}");
        (void)apply("ws.send.v1", feed.commands()[0]);
        test.expect(apply("ws.receive.v1", R"({"type":"subscribed","id":1,"msg":{"channel":"orderbook_delta","sid":11}})").event == session::FeedEvent::control, "shared ack");
        test.expect(apply("ws.receive.v1", R"({"type":"orderbook_snapshot","sid":11,"seq":40,"msg":{"market_ticker":"A","yes_dollars_fp":[["0.7000","5.00"]],"no_dollars_fp":[]}})").event == session::FeedEvent::market, "shared first snapshot");
        test.expect(apply("ws.receive.v1", R"({"type":"orderbook_snapshot","sid":11,"seq":41,"msg":{"market_ticker":"B","yes_dollars_fp":[],"no_dollars_fp":[["0.6000","5.00"]]}})").event == session::FeedEvent::market, "shared second snapshot advances sequence");
        test.expect(apply("ws.receive.v1", R"({"type":"orderbook_delta","sid":11,"seq":42,"msg":{"market_ticker":"A","side":"yes","price_dollars":"0.7000","delta_fp":"1.00"}})").event == session::FeedEvent::market, "other market message accounts for apparent per-book gap");
        test.expect(feed.state().find_book(1U)->last_sequence() == 42U && feed.state().find_book(2U)->last_sequence() == 41U,
                    "each book retains its original last venue sequence");
        test.expect(feed.state().find_book(1U)->quantity_at(book::Side::bid, test::price(7000)).raw() == 600,
                    "interleaved delta updates the correct quantity");
        test.expect(apply("paper.clock.v1", "{}").event == session::FeedEvent::control &&
            feed.state().find_book(1U)->last_sequence() == 42U && feed.state().valid_book_count() == 2U,
            "local paper clock does not consume venue sequence or mutate books");
        test.expect(apply("ws.receive.v1", bad).event == session::FeedEvent::invalidated, "shared invalid input stops generation");
        test.expect(feed.state().valid_book_count() == 0U, "shared failure invalidates every book");
        (void)apply("ws.close.v1", "{}");
        test.expect(apply("paper.clock.v1", "{}").event == session::FeedEvent::control &&
            !feed.state().connected() && feed.state().valid_book_count() == 0U,
            "disconnected simulation clock never restores tradability");
        record.connection_generation = 2U;
        (void)apply("ws.attempt.v1", "{}"); (void)apply("ws.open.v1", "{}");
        (void)apply("ws.send.v1", feed.commands()[0]);
        (void)apply("ws.receive.v1", R"({"type":"subscribed","id":1,"msg":{"channel":"orderbook_delta","sid":11}})");
        test.expect(apply("ws.receive.v1", R"({"type":"orderbook_snapshot","sid":11,"seq":1,"msg":{"market_ticker":"B","market_id":""}})").event == session::FeedEvent::market, "shared reconnect resets scope and permits reversed snapshot order");
        test.expect(feed.state().valid_book_count() == 1U, "other recovered book still requires a snapshot");
        test.expect(apply("ws.receive.v1", R"({"type":"orderbook_snapshot","sid":11,"seq":2,"msg":{"market_ticker":"A","yes_dollars_fp":[["0.7000","1.00"]]}})").event == session::FeedEvent::market, "absent NO side is empty during recovery");
    }
    public_trade_subscriptions(test, markets, selection);
    public_trade_normalization(test, markets, selection);
    public_trade_rejections(test, markets, selection);
    public_trade_replay(test, markets, selection);
    return test.result();
}
