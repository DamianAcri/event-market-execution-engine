#pragma once

#include "eme/gateway/kalshi/market_registry.hpp"
#include "eme/gateway/kalshi/orderbook_normalizer.hpp"

#include <string>
#include <string_view>
#include <variant>

namespace eme::gateway::kalshi {

enum class DecodeErrorCode : std::uint8_t {
    invalid_json,
    invalid_root,
    missing_field,
    invalid_field_type,
    invalid_field_value,
    invalid_level,
    unknown_market,
    unsupported_message_type,
};

struct DecodeError final {
    DecodeErrorCode code{};
    std::string field;
};

using DecodedOrderBookMessage =
    std::variant<WireOrderBookSnapshot, WireOrderBookDelta, DecodeError>;

// Market identity is resolved from the ticker inside the payload. The raw
// payload remains the canonical source for recorder/replay.
[[nodiscard]] DecodedOrderBookMessage decode_orderbook_message(
    std::string_view raw_payload,
    market::ConnectionGeneration connection_generation,
    market::ReceiveTime received_at,
    const MarketRegistry& markets);

}  // namespace eme::gateway::kalshi
