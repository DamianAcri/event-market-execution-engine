#include "eme/session/readonly_feed.hpp"
#include "study_json.hpp"
#include "gateway/kalshi/orderbook_json.hpp"

#include <algorithm>

namespace eme::session {
using detail::Json;

ReadOnlyFeed::ReadOnlyFeed(const gateway::kalshi::MarketRegistry& markets,
                         const std::span<const market::MarketId> selection)
    : markets_{markets}, processor_{markets}, selection_{selection.begin(), selection.end()} {
    if (selection.empty() || selection.size() > 64U) { detail::invalid("capture selection must contain 1..64 markets"); }
    std::sort(selection_.begin(), selection_.end());
    if (std::adjacent_find(selection_.begin(), selection_.end()) != selection_.end()) {
        detail::invalid("duplicate capture market");
    }
    for (const auto id : selection_) {
        const auto ticker = markets.find(id);
        if (!ticker) { detail::invalid("unknown capture market"); }
        commands_.push_back(Json{{"id", commands_.size() + 1U}, {"cmd", "subscribe"},
            {"params", {{"channels", {"orderbook_delta"}}, {"market_ticker", *ticker},
                        {"use_yes_price", true}}}}.dump());
    }
    acknowledged_.resize(selection_.size());
    subscriptions_.reserve(selection_.size());
}

void ReadOnlyFeed::abort() noexcept {
    if (processor_.state().connected()) { (void)processor_.close_connection(generation_); }
    faulted_ = true;
}

FeedUpdate ReadOnlyFeed::fail(const std::string_view reason) {
    abort();
    return {FeedEvent::invalidated, std::nullopt, reason};
}

FeedUpdate ReadOnlyFeed::accept(const journal::RawMarketRecord& record) {
    const auto time = record.received_at.time_since_epoch().count();
    if (record.schema_version != journal::current_schema_version || record.metadata_version != markets_.metadata_version() ||
        record.sequence != 0U || record.exchange_time_ns || time < last_time_) {
        (void)fail("envelope");
        return {FeedEvent::invalid_history, {}, "envelope"};
    }
    last_time_ = time;
    if (record.channel == "ws.attempt.v1") {
        if (attempting_ || record.connection_generation <= generation_) {
            (void)fail("attempt order");
            return {FeedEvent::invalid_history, {}, "attempt order"};
        }
        generation_ = record.connection_generation;
        attempting_ = true;
        opened_ = false;
        faulted_ = false;
        sent_ = 0U;
        std::fill(acknowledged_.begin(), acknowledged_.end(), false);
        subscriptions_.clear();
        return {};
    }
    if (!attempting_ || record.connection_generation != generation_) {
        (void)fail("generation");
        return {FeedEvent::invalid_history, {}, "generation"};
    }
    if (record.channel == "ws.close.v1") {
        if (processor_.state().connected()) { (void)processor_.close_connection(generation_); }
        attempting_ = false;
        opened_ = false;
        return {};
    }
    if (record.channel == "ws.open.v1") {
        if (opened_ || faulted_ || !processor_.open_connection(generation_)) { return fail("open order"); }
        opened_ = true;
        return {};
    }
    if (!healthy()) { return {FeedEvent::invalid_history, {}, "input after failure"}; }
    if (record.channel == "ws.send.v1") {
        if (sent_ >= commands_.size() || record.payload != commands_[sent_]) { return fail("subscription command"); }
        // A new subscription explicitly requests a new snapshot for stale books.
        const auto id = selection_[sent_];
        if (processor_.state().find_book(id) && !processor_.begin_recovery(id)) { return fail("recovery transition"); }
        ++sent_;
        return {};
    }
    // Control payloads are framed as a JSON string so an empty ping remains a
    // nonempty journal payload. Beast owns the automatic Pong response.
    if (record.channel == "ws.ping.v1" || record.channel == "ws.pong.v1") { return {}; }
    if (record.channel != "ws.receive.v1") { return fail("unknown channel"); }
    try {
        const auto root = detail::parse_strict(record.payload);
        const auto type = detail::string(root, "type");
        if (type == "subscribed") {
            const auto id = detail::integer(root, "id", sent_);
            const auto& msg = root.at("msg");
            const auto sid = detail::integer(msg, "sid", std::numeric_limits<std::uint64_t>::max());
            if (id == 0U || sid == 0U || root.contains("seq") || acknowledged_[id - 1U] ||
                detail::string(msg, "channel") != "orderbook_delta" ||
                !subscriptions_.emplace(sid, selection_[id - 1U]).second) { return fail("subscription acknowledgement"); }
            acknowledged_[id - 1U] = true;
            return {};
        }
        // Errors, unsubscribe and unexpected sequence-bearing control responses
        // terminate the generation. No data sequence is skipped or renumbered.
        if (type != "orderbook_snapshot" && type != "orderbook_delta") { return fail("venue control/error"); }
        const auto sid = detail::integer(root, "sid", std::numeric_limits<std::uint64_t>::max());
        const auto sub = subscriptions_.find(sid);
        if (sub == subscriptions_.end() || markets_.find(detail::string(root.at("msg"), "market_ticker")) != sub->second) {
            return fail("subscription market mismatch");
        }
        const auto* existing = processor_.state().find_book(sub->second);
        if (type == "orderbook_snapshot" && existing && existing->state() == book::BookState::valid) {
            return fail("unsolicited snapshot");
        }
        const auto decoded = gateway::kalshi::detail::decode_orderbook_json(
            root, record.connection_generation, record.received_at, markets_);
        const auto processed = processor_.process_decoded(decoded);
        const auto* result = std::get_if<book::BookUpdateResult>(&processed);
        if (!result || *result != book::BookUpdateResult::applied) { return fail("book rejected"); }
        return {FeedEvent::market, sub->second, "applied"};
    } catch (const ReplayError&) { return fail("invalid message"); }
    catch (const Json::exception&) { return fail("invalid message"); }
}
} // namespace eme::session
