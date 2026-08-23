#pragma once

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
    unsupported_message_type,
};

struct DecodeError final {
    DecodeErrorCode code{};
    std::string field;
};

using DecodedOrderBookMessage =
    std::variant<WireOrderBookSnapshot, WireOrderBookDelta, DecodeError>;

// The caller resolves the venue ticker to a stable internal MarketId before
// decoding. The raw payload remains the canonical source for recorder/replay.
[[nodiscard]] DecodedOrderBookMessage decode_orderbook_message(
    std::string_view raw_payload,
    market::MarketId market_id,
    market::ReceiveTime received_at,
    BookPriceConvention price_convention);

}  // namespace eme::gateway::kalshi
