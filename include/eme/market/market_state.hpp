#pragma once

#include "eme/market/normalized_event.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <unordered_map>
#include <variant>

namespace eme::market {

enum class MarketStateError : std::uint8_t {
    connection_mismatch,
};

using MarketApplyResult = std::variant<book::BookUpdateResult, MarketStateError>;

enum class SequenceScope : std::uint8_t { per_book, shared_stream };

class MarketState final {
public:
    explicit MarketState(SequenceScope scope = SequenceScope::per_book) : scope_{scope} {}
    [[nodiscard]] bool open_connection(ConnectionGeneration generation);
    [[nodiscard]] bool close_connection(ConnectionGeneration generation) noexcept;
    [[nodiscard]] bool begin_recovery(MarketId market_id) noexcept;

    [[nodiscard]] MarketApplyResult apply(const BookSnapshot& snapshot);
    [[nodiscard]] MarketApplyResult apply(const BookDelta& delta);
    [[nodiscard]] MarketApplyResult apply(const NormalizedMarketEvent& event);

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
    [[nodiscard]] bool accepts_sequence(book::StreamId stream, book::SequenceNumber sequence) const noexcept;
    [[nodiscard]] MarketApplyResult finish_shared(
        book::StreamId stream, book::SequenceNumber sequence, book::BookUpdateResult result);

    std::optional<ConnectionGeneration> connection_generation_;
    bool connected_{false};
    std::unordered_map<MarketId, book::OrderBook> books_;
    SequenceScope scope_;
    std::unordered_map<book::StreamId, book::SequenceNumber> stream_sequences_;
};

}  // namespace eme::market
