#include "eme/market/market_state.hpp"

#include <variant>

namespace eme::market {

bool MarketState::open_connection(const ConnectionGeneration generation) {
    if (generation == 0U ||
        (connection_generation_.has_value() && generation <= *connection_generation_)) {
        return false;
    }

    if (connection_generation_.has_value()) {
        invalidate_all();
    }
    connection_generation_ = generation;
    stream_sequences_.clear();
    connected_ = true;
    return true;
}

bool MarketState::close_connection(const ConnectionGeneration generation) noexcept {
    if (!connected_ || !connection_generation_.has_value() ||
        generation != *connection_generation_) {
        return false;
    }
    connected_ = false;
    invalidate_all();
    return true;
}

bool MarketState::begin_recovery(const MarketId market_id) noexcept {
    if (!connected_) {
        return false;
    }
    const auto found = books_.find(market_id);
    return found != books_.end() && found->second.begin_recovery();
}

MarketApplyResult MarketState::apply(const BookSnapshot& snapshot) {
    if (!accepts(snapshot.connection_generation)) {
        return MarketStateError::connection_mismatch;
    }
    if (scope_ == SequenceScope::shared_stream && !accepts_sequence(snapshot.stream_id, snapshot.sequence)) {
        return finish_shared(snapshot.stream_id, snapshot.sequence, book::BookUpdateResult::sequence_gap);
    }
    const auto [found, inserted] = books_.try_emplace(snapshot.market_id);
    static_cast<void>(inserted);
    const auto result = found->second.apply_snapshot(
        snapshot.stream_id, snapshot.sequence, snapshot.bids, snapshot.asks);
    return scope_ == SequenceScope::shared_stream ? finish_shared(snapshot.stream_id, snapshot.sequence, result) : result;
}

MarketApplyResult MarketState::apply(const BookDelta& delta) {
    if (!accepts(delta.connection_generation)) {
        return MarketStateError::connection_mismatch;
    }
    const auto found = books_.find(delta.market_id);
    if (found == books_.end()) {
        return scope_ == SequenceScope::shared_stream
            ? finish_shared(delta.stream_id, delta.sequence, book::BookUpdateResult::requires_snapshot)
            : MarketApplyResult{book::BookUpdateResult::requires_snapshot};
    }
    if (scope_ == SequenceScope::shared_stream) {
        const auto previous = stream_sequences_.find(delta.stream_id);
        if (previous == stream_sequences_.end() || !accepts_sequence(delta.stream_id, delta.sequence)) {
            return finish_shared(delta.stream_id, delta.sequence, book::BookUpdateResult::sequence_gap);
        }
        return finish_shared(delta.stream_id, delta.sequence, found->second.apply_delta_after(
            delta.stream_id, previous->second, delta.sequence, delta.side, delta.price, delta.quantity_delta));
    }
    return found->second.apply_delta(
        delta.stream_id, delta.sequence, delta.side, delta.price, delta.quantity_delta);
}

bool MarketState::accepts_sequence(const book::StreamId stream, const book::SequenceNumber sequence) const noexcept {
    const auto previous = stream_sequences_.find(stream);
    return previous == stream_sequences_.end() ||
        (sequence > previous->second && sequence - previous->second == 1U);
}

MarketApplyResult MarketState::finish_shared(const book::StreamId stream,
    const book::SequenceNumber sequence, const book::BookUpdateResult result) {
    if (result == book::BookUpdateResult::applied) { stream_sequences_[stream] = sequence; }
    else { (void)close_connection(*connection_generation_); }
    return result;
}

MarketApplyResult MarketState::apply(const NormalizedMarketEvent& event) {
    return std::visit([this](const auto& update) { return apply(update); }, event);
}

const book::OrderBook* MarketState::find_book(const MarketId market_id) const noexcept {
    const auto found = books_.find(market_id);
    return found == books_.end() ? nullptr : &found->second;
}

std::size_t MarketState::valid_book_count() const noexcept {
    std::size_t count = 0U;
    for (const auto& [market_id, order_book] : books_) {
        static_cast<void>(market_id);
        if (order_book.state() == book::BookState::valid) {
            ++count;
        }
    }
    return count;
}

bool MarketState::accepts(const ConnectionGeneration generation) const noexcept {
    return connected_ && connection_generation_.has_value() &&
           generation == *connection_generation_;
}

void MarketState::invalidate_all() noexcept {
    for (auto& [market_id, order_book] : books_) {
        static_cast<void>(market_id);
        order_book.mark_stale();
    }
}

}  // namespace eme::market
