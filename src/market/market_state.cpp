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

book::BookUpdateResult MarketState::apply(const BookSnapshot& snapshot) {
    if (!accepts(snapshot.connection_generation)) {
        return book::BookUpdateResult::connection_mismatch;
    }
    const auto [found, inserted] = books_.try_emplace(snapshot.market_id);
    if (!inserted && found->second.state() == book::BookState::stale) {
        return book::BookUpdateResult::recovery_not_started;
    }
    return found->second.apply_snapshot(
        snapshot.stream_id, snapshot.sequence, snapshot.bids, snapshot.asks);
}

book::BookUpdateResult MarketState::apply(const BookDelta& delta) {
    if (!accepts(delta.connection_generation)) {
        return book::BookUpdateResult::connection_mismatch;
    }
    const auto found = books_.find(delta.market_id);
    if (found == books_.end()) {
        return book::BookUpdateResult::requires_snapshot;
    }
    return found->second.apply_delta(
        delta.stream_id, delta.sequence, delta.side, delta.price, delta.quantity_delta);
}

book::BookUpdateResult MarketState::apply(const NormalizedMarketEvent& event) {
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
