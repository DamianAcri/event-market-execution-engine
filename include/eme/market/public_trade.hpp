#pragma once

#include "eme/market/normalized_event.hpp"
#include <optional>
#include <string>

namespace eme::market {
// Observed venue trade, never a fill of one of our simulated orders. Prices are
// always YES prices, independent of the book subscription's use_yes_price flag.
struct PublicTrade final {
    MarketId market_id{};
    std::string trade_id;
    std::int64_t yes_price_1e4{};
    std::int64_t quantity_centicontracts{};
    std::uint64_t exchange_time_ms{};
    bool taker_yes{};
    std::optional<bool> block_trade{}; // absent means unknown, not false
};
} // namespace eme::market
