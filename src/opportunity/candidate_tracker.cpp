#include "eme/opportunity/candidate_tracker.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>

namespace eme::opportunity {
namespace {

using Evaluation = std::variant<CandidateQuote, InvalidationReason>;

[[nodiscard]] Evaluation quote(const CandidateId& id, const market::MarketState& state) {
    if (!state.connected()) { return InvalidationReason::disconnected; }
    std::array<std::int64_t, 2U> prices{};
    auto quantity = std::numeric_limits<std::int64_t>::max();
    for (std::size_t index = 0U; index < id.legs.size(); ++index) {
        const auto& leg = id.legs[index];
        const auto* book = state.find_book(leg.market_id);
        if (!book || book->state() != book::BookState::valid) {
            return InvalidationReason::book_unavailable;
        }
        const bool yes = leg.outcome == constraint::ContractOutcome::yes;
        const auto level = yes ? book->best_ask() : book->best_bid();
        if (!level) { return InvalidationReason::missing_quote; }
        // The normalized book is in YES units: buying NO consumes YES bids
        // at complementary price, using the quantity at that original bid.
        prices[index] = yes ? level->raw() : core::Price::scale - level->raw();
        quantity = std::min(quantity,
            book->quantity_at(yes ? book::Side::ask : book::Side::bid, *level).raw());
    }
    if (quantity == 0) { return InvalidationReason::missing_quote; }
    const auto sum = prices[0U] + prices[1U]; // Each is bounded by Price::scale.
    if (sum >= core::Price::scale) { return InvalidationReason::non_positive_margin; }
    static_assert(core::Price::scale * core::Quantity::scale == core::Cash::scale);
    const auto contracts = *core::Quantity::from_raw(quantity);
    const auto payout = core::contract_settlement_value(contracts);
    // With positive gross margin, acquisition cost is strictly below payout;
    // checking the latter bounds both integer products without rounding.
    if (!payout) { return InvalidationReason::arithmetic_overflow; }
    const auto cost = *core::Cash::from_raw(sum * quantity);
    return CandidateQuote{
        {*core::Price::from_raw(prices[0U]), *core::Price::from_raw(prices[1U])},
        contracts, cost, *payout, *core::Cash::from_raw(payout->raw() - cost.raw())};
}

}  // namespace

CandidateTracker::CandidateTracker(const market::MetadataVersion version,
                                   const constraint::ConstraintRegistry& constraints) {
    if (version == 0U) { throw std::invalid_argument{"candidate metadata version must be nonzero"}; }
    entries_.reserve(constraints.size());
    events_.reserve(constraints.size());
    for (const auto id : constraints.sorted_ids()) {
        const auto& compiled = *constraints.find(id);
        const auto verified = constraint::construct_guaranteed_portfolio(
            compiled, *core::Quantity::from_raw(core::Quantity::scale));
        if (compiled.guaranteed_leg_templates.size() != 2U ||
            !std::holds_alternative<constraint::GuaranteedPortfolio>(verified)) {
            throw std::invalid_argument{"unsupported candidate payoff template"};
        }
        CandidateId identity{version, id, compiled.metadata.semantic_version,
            CandidateDirection::buy_guaranteed_legs,
            {compiled.guaranteed_leg_templates[0U], compiled.guaranteed_leg_templates[1U]}};
        if (identity.legs[1U].market_id < identity.legs[0U].market_id) {
            std::swap(identity.legs[0U], identity.legs[1U]);
        }
        for (const auto& leg : identity.legs) {
            dependencies_[leg.market_id].push_back(entries_.size());
        }
        entries_.push_back({identity, std::nullopt});
    }
}

void CandidateTracker::evaluate(Entry& entry, const market::MarketState& state) {
    ++evaluated_;
    const auto evaluated = quote(entry.id, state);
    if (const auto* current = std::get_if<CandidateQuote>(&evaluated)) {
        if (!entry.current || *entry.current != *current) {
            const auto kind = entry.current ? CandidateEventKind::updated : CandidateEventKind::opened;
            if (!entry.current) { ++active_count_; }
            entry.current = *current;
            events_.push_back({entry.id, kind, *current, InvalidationReason::none});
        }
    } else if (entry.current) {
        entry.current.reset();
        --active_count_;
        events_.push_back({entry.id, CandidateEventKind::invalidated, std::nullopt,
                           std::get<InvalidationReason>(evaluated)});
    }
}

std::span<const CandidateEvent> CandidateTracker::refresh_all(const market::MarketState& state) {
    events_.clear();
    evaluated_ = 0U;
    connected_ = state.connected();
    generation_ = state.connection_generation();
    for (auto& entry : entries_) { evaluate(entry, state); }
    return events_;
}

std::span<const CandidateEvent> CandidateTracker::refresh(
    const market::MarketId changed_market, const market::MarketState& state) {
    // Full-scan reference implementation retained before the measured index optimization.
    static_cast<void>(changed_market);
    return refresh_all(state);
}

}  // namespace eme::opportunity
