#pragma once

#include "eme/market/normalized_event.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <unordered_map>

namespace eme::market {

class MarketState final {
public:
    [[nodiscard]] bool open_connection(ConnectionGeneration generation);
    [[nodiscard]] bool close_connection(ConnectionGeneration generation) noexcept;
    [[nodiscard]] bool begin_recovery(MarketId market_id) noexcept;

    [[nodiscard]] book::BookUpdateResult apply(const BookSnapshot& snapshot);
    [[nodiscard]] book::BookUpdateResult apply(const BookDelta& delta);
    [[nodiscard]] book::BookUpdateResult apply(const NormalizedMarketEvent& event);

    [[nodiscard]] bool connected() const noexcept { return connected_; }
    [[nodiscard]] std::optional<ConnectionGeneration> connection_generation() const noexcept {
        return connection_generation_;
    }
    [[nodiscard]] const book::OrderBook* find_book(MarketId market_id) const noexcept;
    [[nodiscard]] std::size_t book_count() const noexcept { return books_.size(); }
    [[nodiscard]] std::size_t valid_book_count() const noexcept;

private:
    [[nodiscard]] bool accepts(ConnectionGeneration generation) const noexcept;
    void invalidate_all() noexcept;

    std::optional<ConnectionGeneration> connection_generation_;
    bool connected_{false};
    std::unordered_map<MarketId, book::OrderBook> books_;
};

}  // namespace eme::market
