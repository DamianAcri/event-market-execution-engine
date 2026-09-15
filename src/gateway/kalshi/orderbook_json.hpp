#pragma once
#include "eme/gateway/kalshi/orderbook_decoder.hpp"
#include <nlohmann/json_fwd.hpp>
namespace eme::gateway::kalshi::detail {
// Internal JSON boundary shared by standalone decoding and the WS controller.
[[nodiscard]] DecodedOrderBookMessage decode_orderbook_json(
    const nlohmann::json&, market::ConnectionGeneration, market::ReceiveTime, const MarketRegistry&);
}
