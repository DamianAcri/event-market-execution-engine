#pragma once

#include "eme/book/order_book.hpp"

#include <chrono>
#include <cstdint>
#include <variant>
#include <vector>

namespace eme::market {

using MarketId = std::uint32_t;
using ReceiveTime = std::chrono::time_point<std::chrono::steady_clock, std::chrono::nanoseconds>;

struct BookSnapshot final {
    MarketId market_id{};
    book::StreamId stream_id{};
    book::SequenceNumber sequence{};
    ReceiveTime received_at{};
    std::vector<book::Level> bids;
    std::vector<book::Level> asks;
};

struct BookDelta final {
    MarketId market_id{};
    book::StreamId stream_id{};
    book::SequenceNumber sequence{};
    ReceiveTime received_at{};
    book::Side side{};
    core::Price price;
    core::QuantityDelta quantity_delta;
};

using NormalizedMarketEvent = std::variant<BookSnapshot, BookDelta>;

}  // namespace eme::market
