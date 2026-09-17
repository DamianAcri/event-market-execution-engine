#pragma once

#include "eme/gateway/kalshi/metadata_snapshot.hpp"

#include <string>
#include <string_view>
#include <variant>

namespace eme::cli {

struct MarketScreenError final { std::string field; };
using MarketScreenResult = std::variant<std::string, MarketScreenError>;

// Offline, indicative full-acquisition quotes from independently fetched REST
// books. Calls the same fixed-point fee/depth sizing core as the paper engine.
// It does not simulate fills, allocate capital across quotes, or submit orders.
[[nodiscard]] MarketScreenResult screen_markets(
    const gateway::kalshi::MetadataSnapshot& metadata, std::string_view input);
[[nodiscard]] int run_market_screen_command(int argc, const char* const argv[]);

}  // namespace eme::cli
