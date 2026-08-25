#include "eme/gateway/kalshi/market_registry.hpp"

#include <functional>
#include <utility>

namespace eme::gateway::kalshi {

MarketRegistrationResult MarketRegistry::register_market(
    const market::MarketId market_id,
    std::string ticker) {
    if (metadata_version_ == 0U) {
        return MarketRegistrationResult::invalid_metadata_version;
    }
    if (market_id == 0U) {
        return MarketRegistrationResult::invalid_market_id;
    }
    if (ticker.empty()) {
        return MarketRegistrationResult::invalid_ticker;
    }
    if (const auto by_ticker = market_ids_.find(ticker); by_ticker != market_ids_.end()) {
        return by_ticker->second == market_id
                   ? MarketRegistrationResult::already_registered
                   : MarketRegistrationResult::ticker_conflict;
    }
    if (tickers_.contains(market_id)) {
        return MarketRegistrationResult::market_id_conflict;
    }

    tickers_.emplace(market_id, ticker);
    market_ids_.emplace(std::move(ticker), market_id);
    return MarketRegistrationResult::registered;
}

std::optional<std::string_view> MarketRegistry::find(
    const market::MarketId market_id) const {
    const auto found = tickers_.find(market_id);
    return found == tickers_.end() ? std::nullopt
                                   : std::optional<std::string_view>{found->second};
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
