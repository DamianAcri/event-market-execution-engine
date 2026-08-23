#include "eme/book/order_book.hpp"

#include <limits>
#include <utility>

namespace eme::book {

BookUpdateResult OrderBook::apply_snapshot(
    const StreamId stream_id,
    const SequenceNumber sequence,
    const std::vector<Level>& bids,
    const std::vector<Level>& asks) {
    Levels next_bids;
    Levels next_asks;

    const auto load_levels = [](const std::vector<Level>& source, Levels& destination) {
        for (const auto& level : source) {
            if (level.quantity.raw() == 0) {
                return false;
            }
            const auto [unused, inserted] = destination.emplace(
                level.price.raw(), level.quantity.raw());
            static_cast<void>(unused);
            if (!inserted) {
                return false;
            }
        }
        return true;
    };

    if (!load_levels(bids, next_bids) || !load_levels(asks, next_asks)) {
        mark_stale();
        return BookUpdateResult::invalid_level;
    }

    bids_.swap(next_bids);
    asks_.swap(next_asks);
    stream_id_ = stream_id;
    last_sequence_ = sequence;
    state_ = BookState::valid;
    return BookUpdateResult::applied;
}

BookUpdateResult OrderBook::apply_delta(
    const StreamId stream_id,
    const SequenceNumber sequence,
    const Side side,
    const core::Price price,
    const std::int64_t quantity_delta_raw) {
    if (state_ != BookState::valid || !stream_id_.has_value() || !last_sequence_.has_value()) {
        return BookUpdateResult::requires_snapshot;
    }

    if (stream_id != *stream_id_) {
        mark_stale();
        return BookUpdateResult::stream_mismatch;
    }

    if (sequence <= *last_sequence_ || sequence - *last_sequence_ != 1U) {
        mark_stale();
        return BookUpdateResult::sequence_gap;
    }

    auto& levels = levels_for(side);
    const auto found = levels.find(price.raw());
    const std::int64_t current = found == levels.end() ? 0 : found->second;

    if ((quantity_delta_raw > 0 &&
         current > std::numeric_limits<std::int64_t>::max() - quantity_delta_raw) ||
        quantity_delta_raw == std::numeric_limits<std::int64_t>::min() ||
        (quantity_delta_raw < 0 && current < -quantity_delta_raw)) {
        mark_stale();
        return BookUpdateResult::invalid_level;
    }

    const auto updated = current + quantity_delta_raw;
    if (updated == 0) {
        if (found != levels.end()) {
            levels.erase(found);
        }
    } else {
        levels.insert_or_assign(price.raw(), updated);
    }

    last_sequence_ = sequence;
    return BookUpdateResult::applied;
}

void OrderBook::mark_stale() noexcept {
    state_ = BookState::stale;
}

std::optional<core::Price> OrderBook::best_bid() const noexcept {
    if (state_ != BookState::valid || bids_.empty()) {
        return std::nullopt;
    }
    return core::Price::from_raw(bids_.rbegin()->first);
}

std::optional<core::Price> OrderBook::best_ask() const noexcept {
    if (state_ != BookState::valid || asks_.empty()) {
        return std::nullopt;
    }
    return core::Price::from_raw(asks_.begin()->first);
}

core::Quantity OrderBook::quantity_at(const Side side, const core::Price price) const noexcept {
    if (state_ != BookState::valid) {
        return *core::Quantity::from_raw(0);
    }
    const auto& levels = levels_for(side);
    const auto found = levels.find(price.raw());
    return *core::Quantity::from_raw(found == levels.end() ? 0 : found->second);
}

std::size_t OrderBook::level_count(const Side side) const noexcept {
    return levels_for(side).size();
}

OrderBook::Levels& OrderBook::levels_for(const Side side) noexcept {
    return side == Side::bid ? bids_ : asks_;
}

const OrderBook::Levels& OrderBook::levels_for(const Side side) const noexcept {
    return side == Side::bid ? bids_ : asks_;
}

}  // namespace eme::book
