#pragma once

#include "eme/market/normalized_event.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace eme::gateway::kalshi {

class MarketRegistry final {
public:
    [[nodiscard]] std::optional<market::MarketId> register_market(std::string ticker);
    [[nodiscard]] std::optional<market::MarketId> find(std::string_view ticker) const;
    [[nodiscard]] std::size_t size() const noexcept { return market_ids_.size(); }

private:
    struct TransparentStringHash final {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept;
        [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept;
    };

    std::unordered_map<std::string, market::MarketId, TransparentStringHash, std::equal_to<>>
        market_ids_;
    market::MarketId next_id_{1U};
};

}  // namespace eme::gateway::kalshi
