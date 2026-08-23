#pragma once

#include "eme/market/normalized_event.hpp"

#include <string>
#include <variant>
#include <vector>

namespace eme::gateway::kalshi {

enum class OutcomeSide : std::uint8_t {
    yes,
    no,
};

enum class BookPriceConvention : std::uint8_t {
    // NO levels use NO prices and must be complemented into YES asks.
    legacy_separate_scales,
    // `use_yes_price: true`: both YES and NO levels already use the YES scale.
    unified_yes_scale,
};

enum class NormalizationError : std::uint8_t {
    invalid_price,
    invalid_quantity,
    zero_quantity,
    zero_delta,
};

struct WirePriceLevel final {
    std::string price_dollars;
    std::string quantity_fp;
};

struct WireOrderBookSnapshot final {
    market::MarketId market_id{};
    book::StreamId stream_id{};
    book::SequenceNumber sequence{};
    market::ReceiveTime received_at{};
    BookPriceConvention price_convention{BookPriceConvention::unified_yes_scale};
    std::vector<WirePriceLevel> yes_bids;
    std::vector<WirePriceLevel> no_bids;
};

struct WireOrderBookDelta final {
    market::MarketId market_id{};
    book::StreamId stream_id{};
    book::SequenceNumber sequence{};
    market::ReceiveTime received_at{};
    BookPriceConvention price_convention{BookPriceConvention::unified_yes_scale};
    OutcomeSide outcome_side{OutcomeSide::yes};
    std::string price_dollars;
    std::string quantity_delta_fp;
};

using SnapshotNormalizationResult = std::variant<market::BookSnapshot, NormalizationError>;
using DeltaNormalizationResult = std::variant<market::BookDelta, NormalizationError>;

[[nodiscard]] SnapshotNormalizationResult normalize_orderbook_snapshot(
    const WireOrderBookSnapshot& wire);

[[nodiscard]] DeltaNormalizationResult normalize_orderbook_delta(
    const WireOrderBookDelta& wire);

}  // namespace eme::gateway::kalshi
