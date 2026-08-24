#include "eme/gateway/kalshi/orderbook_normalizer.hpp"

#include <optional>
#include <string_view>

namespace eme::gateway::kalshi {
namespace {

using PriceResult = std::variant<core::Price, NormalizationError>;

[[nodiscard]] PriceResult normalize_price(
    const std::string_view price_text) {
    const auto parsed = core::Price::parse(price_text);
    if (!parsed.has_value()) {
        return NormalizationError::invalid_price;
    }
    return *parsed;
}

[[nodiscard]] std::optional<NormalizationError> append_levels(
    const std::vector<WirePriceLevel>& source,
    std::vector<book::Level>& destination) {
    destination.reserve(source.size());
    for (const auto& wire_level : source) {
        const auto normalized_price = normalize_price(wire_level.price_dollars);
        if (std::holds_alternative<NormalizationError>(normalized_price)) {
            return std::get<NormalizationError>(normalized_price);
        }

        const auto quantity = core::Quantity::parse(wire_level.quantity_fp);
        if (!quantity.has_value()) {
            return NormalizationError::invalid_quantity;
        }
        if (quantity->raw() == 0) {
            return NormalizationError::zero_quantity;
        }

        destination.push_back(book::Level{
            std::get<core::Price>(normalized_price),
            *quantity,
        });
    }
    return std::nullopt;
}

}  // namespace

SnapshotNormalizationResult normalize_orderbook_snapshot(const WireOrderBookSnapshot& wire) {
    market::BookSnapshot normalized{
        wire.market_id,
        wire.stream_id,
        wire.sequence,
        wire.received_at,
        {},
        {},
    };

    if (const auto error = append_levels(
            wire.yes_bids,
            normalized.bids);
        error.has_value()) {
        return *error;
    }

    if (const auto error = append_levels(
            wire.no_bids,
            normalized.asks);
        error.has_value()) {
        return *error;
    }

    return normalized;
}

DeltaNormalizationResult normalize_orderbook_delta(const WireOrderBookDelta& wire) {
    const auto normalized_price = normalize_price(wire.price_dollars);
    if (std::holds_alternative<NormalizationError>(normalized_price)) {
        return std::get<NormalizationError>(normalized_price);
    }

    const auto quantity_delta = core::QuantityDelta::parse(wire.quantity_delta_fp);
    if (!quantity_delta.has_value()) {
        return NormalizationError::invalid_quantity;
    }
    if (quantity_delta->raw() == 0) {
        return NormalizationError::zero_delta;
    }

    return market::BookDelta{
        wire.market_id,
        wire.stream_id,
        wire.sequence,
        wire.received_at,
        wire.outcome_side == OutcomeSide::yes ? book::Side::bid : book::Side::ask,
        std::get<core::Price>(normalized_price),
        *quantity_delta,
    };
}

}  // namespace eme::gateway::kalshi
