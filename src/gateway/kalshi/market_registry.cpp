#include "eme/gateway/kalshi/market_registry.hpp"

#include <functional>
#include <limits>
#include <utility>

namespace eme::gateway::kalshi {

std::optional<market::MarketId> MarketRegistry::register_market(std::string ticker) {
    if (ticker.empty()) {
        return std::nullopt;
    }
    if (const auto existing = market_ids_.find(ticker); existing != market_ids_.end()) {
        return existing->second;
    }
    if (next_id_ == std::numeric_limits<market::MarketId>::max()) {
        return std::nullopt;
    }

    const auto id = next_id_++;
    market_ids_.emplace(std::move(ticker), id);
    return id;
}

std::optional<market::MarketId> MarketRegistry::find(const std::string_view ticker) const {
    const auto found = market_ids_.find(ticker);
    return found == market_ids_.end() ? std::nullopt : std::optional{found->second};
}

std::size_t MarketRegistry::TransparentStringHash::operator()(
    const std::string_view value) const noexcept {
    return std::hash<std::string_view>{}(value);
}

std::size_t MarketRegistry::TransparentStringHash::operator()(
    const std::string& value) const noexcept {
    return (*this)(std::string_view{value});
}

}  // namespace eme::gateway::kalshi
