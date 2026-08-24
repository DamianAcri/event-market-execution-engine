#include "eme/gateway/kalshi/orderbook_decoder.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace eme::gateway::kalshi {
namespace {

using Json = nlohmann::json;

[[nodiscard]] DecodeError error(const DecodeErrorCode code, const std::string_view field) {
    return DecodeError{code, std::string{field}};
}

[[nodiscard]] const Json* find_field(const Json& object, const std::string_view name) {
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &*found;
}

[[nodiscard]] std::variant<std::uint64_t, DecodeError> read_unsigned(
    const Json& object,
    const std::string_view name) {
    const auto* value = find_field(object, name);
    if (value == nullptr) {
        return error(DecodeErrorCode::missing_field, name);
    }
    if (!value->is_number_unsigned()) {
        return error(DecodeErrorCode::invalid_field_type, name);
    }
    return value->get<std::uint64_t>();
}

[[nodiscard]] std::variant<std::string, DecodeError> read_string(
    const Json& object,
    const std::string_view name) {
    const auto* value = find_field(object, name);
    if (value == nullptr) {
        return error(DecodeErrorCode::missing_field, name);
    }
    if (!value->is_string()) {
        return error(DecodeErrorCode::invalid_field_type, name);
    }
    return value->get<std::string>();
}

[[nodiscard]] std::variant<std::vector<WirePriceLevel>, DecodeError> read_levels(
    const Json& message,
    const std::string_view name) {
    const auto* levels = find_field(message, name);
    if (levels == nullptr) {
        return error(DecodeErrorCode::missing_field, name);
    }
    if (!levels->is_array()) {
        return error(DecodeErrorCode::invalid_field_type, name);
    }

    std::vector<WirePriceLevel> decoded;
    decoded.reserve(levels->size());
    for (const auto& level : *levels) {
        if (!level.is_array() || level.size() != 2U || !level[0U].is_string() ||
            !level[1U].is_string()) {
            return error(DecodeErrorCode::invalid_level, name);
        }
        decoded.push_back(WirePriceLevel{
            level[0U].get<std::string>(),
            level[1U].get<std::string>(),
        });
    }
    return decoded;
}

}  // namespace

DecodedOrderBookMessage decode_orderbook_message(
    const std::string_view raw_payload,
    const market::ConnectionGeneration connection_generation,
    const market::ReceiveTime received_at,
    const MarketRegistry& markets) {
    if (connection_generation == 0U) {
        return error(DecodeErrorCode::invalid_field_value, "connection_generation");
    }
    const auto root = Json::parse(raw_payload, nullptr, false);
    if (root.is_discarded()) {
        return error(DecodeErrorCode::invalid_json, "$");
    }
    if (!root.is_object()) {
        return error(DecodeErrorCode::invalid_root, "$");
    }

    const auto type = read_string(root, "type");
    if (std::holds_alternative<DecodeError>(type)) {
        return std::get<DecodeError>(type);
    }
    const auto stream_id = read_unsigned(root, "sid");
    if (std::holds_alternative<DecodeError>(stream_id)) {
        return std::get<DecodeError>(stream_id);
    }
    const auto sequence = read_unsigned(root, "seq");
    if (std::holds_alternative<DecodeError>(sequence)) {
        return std::get<DecodeError>(sequence);
    }

    const auto* message = find_field(root, "msg");
    if (message == nullptr) {
        return error(DecodeErrorCode::missing_field, "msg");
    }
    if (!message->is_object()) {
        return error(DecodeErrorCode::invalid_field_type, "msg");
    }
    const auto ticker = read_string(*message, "market_ticker");
    if (std::holds_alternative<DecodeError>(ticker)) {
        return std::get<DecodeError>(ticker);
    }
    const auto& ticker_value = std::get<std::string>(ticker);
    if (ticker_value.empty()) {
        return error(DecodeErrorCode::invalid_field_value, "market_ticker");
    }
    const auto market_id = markets.find(ticker_value);
    if (!market_id.has_value()) {
        return error(DecodeErrorCode::unknown_market, "market_ticker");
    }

    if (std::get<std::string>(type) == "orderbook_snapshot") {
        auto yes_bids = read_levels(*message, "yes_dollars_fp");
        if (std::holds_alternative<DecodeError>(yes_bids)) {
            return std::get<DecodeError>(yes_bids);
        }
        auto no_bids = read_levels(*message, "no_dollars_fp");
        if (std::holds_alternative<DecodeError>(no_bids)) {
            return std::get<DecodeError>(no_bids);
        }
        return WireOrderBookSnapshot{
            *market_id,
            connection_generation,
            std::get<std::uint64_t>(stream_id),
            std::get<std::uint64_t>(sequence),
            received_at,
            std::move(std::get<std::vector<WirePriceLevel>>(yes_bids)),
            std::move(std::get<std::vector<WirePriceLevel>>(no_bids)),
        };
    }

    if (std::get<std::string>(type) == "orderbook_delta") {
        const auto price = read_string(*message, "price_dollars");
        if (std::holds_alternative<DecodeError>(price)) {
            return std::get<DecodeError>(price);
        }
        const auto quantity_delta = read_string(*message, "delta_fp");
        if (std::holds_alternative<DecodeError>(quantity_delta)) {
            return std::get<DecodeError>(quantity_delta);
        }
        const auto side = read_string(*message, "side");
        if (std::holds_alternative<DecodeError>(side)) {
            return std::get<DecodeError>(side);
        }

        OutcomeSide outcome_side{};
        if (std::get<std::string>(side) == "yes") {
            outcome_side = OutcomeSide::yes;
        } else if (std::get<std::string>(side) == "no") {
            outcome_side = OutcomeSide::no;
        } else {
            return error(DecodeErrorCode::invalid_field_value, "side");
        }

        return WireOrderBookDelta{
            *market_id,
            connection_generation,
            std::get<std::uint64_t>(stream_id),
            std::get<std::uint64_t>(sequence),
            received_at,
            outcome_side,
            std::get<std::string>(price),
            std::get<std::string>(quantity_delta),
        };
    }

    return error(DecodeErrorCode::unsupported_message_type, "type");
}

}  // namespace eme::gateway::kalshi
