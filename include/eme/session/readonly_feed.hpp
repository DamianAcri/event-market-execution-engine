#pragma once

#include "eme/gateway/kalshi/orderbook_processor.hpp"
#include "eme/market/public_trade.hpp"

#include <span>
#include <unordered_map>
#include <vector>

namespace eme::session {

enum class FeedEvent : std::uint8_t { control, market, invalidated, invalid_history, public_trade };
struct FeedUpdate final {
    FeedEvent event{FeedEvent::control};
    std::optional<market::MarketId> market_id;
    std::string_view reason{"ok"};
    std::optional<market::PublicTrade> trade{};
};

enum class FeedProtocol : std::uint8_t { per_market_v1, shared_subscription_v1, book_and_trades_v1 };

// One connection owner. Exact WS payloads and
// controller actions share the journal; sequence is inside opaque receive bytes.
// All record envelopes use sequence=0 (never a fabricated venue sequence).
class ReadOnlyFeed final {
public:
    ReadOnlyFeed(const gateway::kalshi::MarketRegistry& markets,
                 std::span<const market::MarketId> selection,
                 FeedProtocol protocol = FeedProtocol::per_market_v1);
    [[nodiscard]] FeedUpdate accept(const journal::RawMarketRecord& record);
    [[nodiscard]] const market::MarketState& state() const noexcept { return processor_.state(); }
    [[nodiscard]] const std::vector<std::string>& commands() const noexcept { return commands_; }
    [[nodiscard]] bool healthy() const noexcept { return opened_ && !faulted_; }
    [[nodiscard]] bool ready() const noexcept;
    // Local recorder/transport failure can invalidate state even when no record
    // can be persisted. Such captures must remain incomplete.
    void abort() noexcept;
    [[nodiscard]] bool closed() const noexcept { return !attempting_; }

private:
    [[nodiscard]] FeedUpdate fail(std::string_view reason);
    const gateway::kalshi::MarketRegistry& markets_;
    gateway::kalshi::OrderBookProcessor processor_;
    FeedProtocol protocol_;
    std::vector<market::MarketId> selection_;
    std::vector<std::string> commands_;
    std::vector<bool> acknowledged_;
    std::unordered_map<std::uint64_t, market::MarketId> subscriptions_;
    market::ConnectionGeneration generation_{};
    std::int64_t last_time_{};
    std::size_t sent_{};
    std::uint64_t trade_sid_{};
    std::uint64_t trade_sequence_{};
    bool attempting_{};
    bool opened_{};
    bool faulted_{};
};

} // namespace eme::session
