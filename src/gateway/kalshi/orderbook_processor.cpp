#include "eme/gateway/kalshi/orderbook_processor.hpp"

#include <type_traits>

namespace eme::gateway::kalshi {

ProcessingResult OrderBookProcessor::process(
    const journal::RawMarketRecord& record) {
    if (record.schema_version != journal::current_schema_version) {
        return ProcessingError::schema_version_mismatch;
    }
    if (record.metadata_version != markets_.metadata_version()) {
        return ProcessingError::metadata_version_mismatch;
    }
    const auto decoded = decode_orderbook_message(
        record.payload, record.connection_generation, record.received_at, markets_);
    if (const auto* error = std::get_if<DecodeError>(&decoded); error != nullptr) {
        return *error;
    }

    const auto decoded_sequence = std::visit(
        [](const auto& message) -> book::SequenceNumber {
            using Message = std::decay_t<decltype(message)>;
            if constexpr (std::is_same_v<Message, DecodeError>) {
                return 0U;
            } else {
                return message.sequence;
            }
        },
        decoded);
    if (decoded_sequence != record.sequence) {
        return ProcessingError::sequence_mismatch;
    }

    if (const auto* snapshot = std::get_if<WireOrderBookSnapshot>(&decoded);
        snapshot != nullptr) {
        const auto normalized = normalize_orderbook_snapshot(*snapshot);
        if (const auto* error = std::get_if<NormalizationError>(&normalized);
            error != nullptr) {
            return *error;
        }
        const auto applied = state_.apply(std::get<market::BookSnapshot>(normalized));
        return std::visit([](const auto result) -> ProcessingResult { return result; }, applied);
    }

    const auto normalized = normalize_orderbook_delta(
        std::get<WireOrderBookDelta>(decoded));
    if (const auto* error = std::get_if<NormalizationError>(&normalized);
        error != nullptr) {
        return *error;
    }
    const auto applied = state_.apply(std::get<market::BookDelta>(normalized));
    return std::visit([](const auto result) -> ProcessingResult { return result; }, applied);
}

}  // namespace eme::gateway::kalshi
