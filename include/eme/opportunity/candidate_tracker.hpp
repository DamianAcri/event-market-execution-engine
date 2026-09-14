#pragma once

#include "eme/constraint/payoff.hpp"
#include "eme/market/market_state.hpp"

#include <array>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace eme::opportunity {

enum class CandidateDirection : std::uint8_t { buy_guaranteed_legs };

// A structural tuple, not a lossy hash. Prices, quantities and connection
// generations do not change identity. Exact metadata content is session-bound.
struct CandidateId final {
    market::MetadataVersion metadata_version{};
    constraint::ConstraintId constraint_id{};
    std::uint32_t semantic_version{};
    CandidateDirection direction{CandidateDirection::buy_guaranteed_legs};
    std::array<constraint::PayoffLegTemplate, 2U> legs;
    friend bool operator==(const CandidateId&, const CandidateId&) = default;
};

// Top-of-book, equal-quantity acquisition only. Cash excludes fees, execution
// risk and funding restrictions. Never treat this gross candidate as an order.
struct CandidateQuote final {
    std::array<core::Price, 2U> acquisition_prices;
    core::Quantity quantity;
    core::Cash acquisition_cost;
    core::Cash minimum_payout;
    core::Cash gross_margin;
    friend bool operator==(const CandidateQuote&, const CandidateQuote&) = default;
};

enum class CandidateEventKind : std::uint8_t { opened, updated, invalidated };
enum class InvalidationReason : std::uint8_t {
    none, disconnected, book_unavailable, missing_quote,
    non_positive_margin, arithmetic_overflow,
};

struct CandidateEvent final {
    CandidateId id;
    CandidateEventKind kind{};
    std::optional<CandidateQuote> quote;
    InvalidationReason reason{InvalidationReason::none};
    friend bool operator==(const CandidateEvent&, const CandidateEvent&) = default;
};

class CandidateTracker final {
public:
    // Owns a compiled projection; the source registry may be destroyed/changed
    // afterwards. A new metadata version requires a new tracker.
    CandidateTracker(market::MetadataVersion version,
                     const constraint::ConstraintRegistry& constraints);

    // Call after EVERY state transition, including rejected updates that stale
    // a book. On connection/generation changes the refresh is automatically global.
    // The returned view expires at the next refresh or tracker destruction.
    [[nodiscard]] std::span<const CandidateEvent> refresh(
        market::MarketId changed_market, const market::MarketState& state);
    [[nodiscard]] std::span<const CandidateEvent> refresh_all(const market::MarketState& state);

    [[nodiscard]] std::size_t active_count() const noexcept { return active_count_; }
    [[nodiscard]] std::size_t definition_count() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t evaluations_last_refresh() const noexcept { return evaluated_; }

private:
    struct Entry final {
        CandidateId id;
        std::optional<CandidateQuote> current;
    };
    void evaluate(Entry& entry, const market::MarketState& state);

    std::vector<Entry> entries_;
    std::unordered_map<market::MarketId, std::vector<std::size_t>> dependencies_;
    std::vector<CandidateEvent> events_;
    std::optional<market::ConnectionGeneration> generation_;
    bool connected_{};
    std::size_t active_count_{};
    std::size_t evaluated_{};
};

}  // namespace eme::opportunity
