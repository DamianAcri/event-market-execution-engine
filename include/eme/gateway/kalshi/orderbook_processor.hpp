#pragma once

#include "eme/gateway/kalshi/orderbook_decoder.hpp"
#include "eme/journal/raw_journal.hpp"
#include "eme/market/market_state.hpp"

#include <variant>

namespace eme::gateway::kalshi {

enum class ProcessingError : std::uint8_t {
    schema_version_mismatch,
    metadata_version_mismatch,
    sequence_mismatch,
};

using ProcessingResult = std::variant<
    book::BookUpdateResult,
    market::MarketStateError,
    DecodeError,
    NormalizationError,
    ProcessingError>;

class OrderBookProcessor final {
public:
    explicit OrderBookProcessor(const MarketRegistry& markets) : markets_{markets} {}
    OrderBookProcessor(MarketRegistry&&) = delete;

    [[nodiscard]] bool open_connection(market::ConnectionGeneration generation) {
        return state_.open_connection(generation);
    }
    [[nodiscard]] bool close_connection(market::ConnectionGeneration generation) noexcept {
        return state_.close_connection(generation);
    }
    [[nodiscard]] bool begin_recovery(market::MarketId market_id) noexcept {
        return state_.begin_recovery(market_id);
    }

    [[nodiscard]] ProcessingResult process(
        const journal::RawMarketRecord& record);

    [[nodiscard]] const market::MarketState& state() const noexcept { return state_; }

private:
    const MarketRegistry& markets_;
    market::MarketState state_;
};

}  // namespace eme::gateway::kalshi
