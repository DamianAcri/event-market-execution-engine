#include "eme/gateway/kalshi/orderbook_processor.hpp"

namespace eme::gateway::kalshi {

ProcessingResult OrderBookProcessor::process(
    const std::string_view raw_payload,
    const market::ConnectionGeneration connection_generation,
    const market::ReceiveTime received_at) {
    const auto decoded = decode_orderbook_message(
        raw_payload, connection_generation, received_at, markets_);
    if (const auto* error = std::get_if<DecodeError>(&decoded); error != nullptr) {
        return *error;
    }

    if (const auto* snapshot = std::get_if<WireOrderBookSnapshot>(&decoded);
        snapshot != nullptr) {
        const auto normalized = normalize_orderbook_snapshot(*snapshot);
        if (const auto* error = std::get_if<NormalizationError>(&normalized);
            error != nullptr) {
            return *error;
        }
        return state_.apply(std::get<market::BookSnapshot>(normalized));
    }

    const auto normalized = normalize_orderbook_delta(
        std::get<WireOrderBookDelta>(decoded));
    if (const auto* error = std::get_if<NormalizationError>(&normalized);
        error != nullptr) {
        return *error;
    }
    return state_.apply(std::get<market::BookDelta>(normalized));
}

}  // namespace eme::gateway::kalshi
