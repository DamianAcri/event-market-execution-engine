#pragma once

#include "eme/market/normalized_event.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace eme::gateway::kalshi {

enum class MarketRegistrationResult : std::uint8_t {
    registered,
    already_registered,
    invalid_metadata_version,
    invalid_market_id,
    invalid_ticker,
    market_id_conflict,
    ticker_conflict,
};

class MarketRegistry final {
public:
    explicit MarketRegistry(market::MetadataVersion metadata_version)
        : metadata_version_{metadata_version} {}

    [[nodiscard]] MarketRegistrationResult register_market(
        market::MarketId market_id,
        std::string ticker);
    [[nodiscard]] std::optional<market::MarketId> find(std::string_view ticker) const;
    [[nodiscard]] std::optional<std::string_view> find(market::MarketId market_id) const;
    [[nodiscard]] market::MetadataVersion metadata_version() const noexcept {
        return metadata_version_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return market_ids_.size(); }

private:
    struct TransparentStringHash final {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept;
        [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept;
    };

    std::unordered_map<std::string, market::MarketId, TransparentStringHash, std::equal_to<>>
        market_ids_;
    std::unordered_map<market::MarketId, std::string> tickers_;
    market::MetadataVersion metadata_version_{};
};

}  // namespace eme::gateway::kalshi
