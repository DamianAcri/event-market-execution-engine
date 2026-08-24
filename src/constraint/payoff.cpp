#include "eme/constraint/payoff.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

namespace eme::constraint {
namespace {

[[nodiscard]] bool has_duplicate_market(const WorldState& world) {
    for (std::size_t left = 0U; left < world.assignments.size(); ++left) {
        for (std::size_t right = left + 1U; right < world.assignments.size(); ++right) {
            if (world.assignments[left].market_id == world.assignments[right].market_id) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] std::optional<bool> settlement(
    const WorldState& world,
    const market::MarketId market_id) {
    const auto found = std::find_if(
        world.assignments.begin(),
        world.assignments.end(),
        [market_id](const WorldAssignment& assignment) {
            return assignment.market_id == market_id;
        });
    return found == world.assignments.end() ? std::nullopt
                                            : std::optional{found->settles_yes};
}

[[nodiscard]] std::vector<market::MarketId> markets_for(const Relationship& relationship) {
    return std::visit(
        [](const auto& typed) {
            if constexpr (std::is_same_v<std::decay_t<decltype(typed)>, Implication>) {
                return std::vector<market::MarketId>{typed.antecedent, typed.consequent};
            } else {
                return std::vector<market::MarketId>{typed.left, typed.right};
            }
        },
        relationship);
}

[[nodiscard]] bool relationship_is_valid(const Relationship& relationship) {
    const auto markets = markets_for(relationship);
    return markets[0U] != 0U && markets[1U] != 0U && markets[0U] != markets[1U];
}

[[nodiscard]] bool relationships_equal(
    const Relationship& left,
    const Relationship& right) {
    if (left.index() != right.index()) {
        return false;
    }
    if (const auto* implication = std::get_if<Implication>(&left); implication != nullptr) {
        const auto& other = std::get<Implication>(right);
        return implication->antecedent == other.antecedent &&
               implication->consequent == other.consequent;
    }
    const auto& complement = std::get<Complement>(left);
    const auto& other = std::get<Complement>(right);
    return (complement.left == other.left && complement.right == other.right) ||
           (complement.left == other.right && complement.right == other.left);
}

}  // namespace

VerificationResult verify_payoff(
    const std::span<const WorldState> valid_worlds,
    const std::span<const PayoffLeg> portfolio) {
    if (valid_worlds.empty()) {
        return VerificationError::empty_world_set;
    }
    if (portfolio.empty()) {
        return VerificationError::empty_portfolio;
    }

    std::int64_t minimum = std::numeric_limits<std::int64_t>::max();
    for (const auto& world : valid_worlds) {
        if (has_duplicate_market(world)) {
            return VerificationError::invalid_world;
        }
        std::int64_t payout = 0;
        for (const auto& leg : portfolio) {
            if (leg.quantity.raw() == 0) {
                return VerificationError::zero_quantity;
            }
            const auto settles_yes = settlement(world, leg.market_id);
            if (!settles_yes.has_value()) {
                return VerificationError::missing_assignment;
            }
            const bool wins = leg.outcome == ContractOutcome::yes
                                  ? *settles_yes
                                  : !*settles_yes;
            if (wins) {
                if (payout > std::numeric_limits<std::int64_t>::max() -
                                 leg.quantity.raw()) {
                    return VerificationError::payout_overflow;
                }
                payout += leg.quantity.raw();
            }
        }
        minimum = std::min(minimum, payout);
    }

    return PayoffVerification{
        *core::Quantity::from_raw(minimum),
        valid_worlds.size(),
    };
}

PortfolioResult construct_guaranteed_portfolio(
    const ConstraintId constraint_id,
    const Relationship& relationship,
    const core::Quantity quantity) {
    if (quantity.raw() == 0) {
        return VerificationError::zero_quantity;
    }
    if (constraint_id == 0U || !relationship_is_valid(relationship)) {
        return VerificationError::invalid_relationship;
    }

    std::vector<WorldState> worlds;
    std::vector<PayoffLeg> legs;
    if (const auto* implication = std::get_if<Implication>(&relationship);
        implication != nullptr) {
        worlds = {
            {{{implication->antecedent, false}, {implication->consequent, false}}},
            {{{implication->antecedent, false}, {implication->consequent, true}}},
            {{{implication->antecedent, true}, {implication->consequent, true}}},
        };
        legs = {
            {implication->consequent, ContractOutcome::yes, quantity},
            {implication->antecedent, ContractOutcome::no, quantity},
        };
    } else {
        const auto& complement = std::get<Complement>(relationship);
        worlds = {
            {{{complement.left, false}, {complement.right, true}}},
            {{{complement.left, true}, {complement.right, false}}},
        };
        legs = {
            {complement.left, ContractOutcome::yes, quantity},
            {complement.right, ContractOutcome::yes, quantity},
        };
    }

    const auto verification = verify_payoff(worlds, legs);
    const auto* verified = std::get_if<PayoffVerification>(&verification);
    if (verified == nullptr || verified->minimum_payout != quantity) {
        return verified == nullptr ? std::get<VerificationError>(verification)
                                   : VerificationError::invalid_relationship;
    }
    return GuaranteedPortfolio{
        constraint_id,
        std::move(legs),
        verified->minimum_payout,
        verified->worlds_evaluated,
    };
}

ConstraintId ConstraintRegistry::add(const Implication relationship) {
    return add_relationship(relationship);
}

ConstraintId ConstraintRegistry::add(const Complement relationship) {
    return add_relationship(relationship);
}

const Relationship* ConstraintRegistry::find(const ConstraintId constraint_id) const noexcept {
    if (constraint_id == 0U || constraint_id > relationships_.size()) {
        return nullptr;
    }
    return &relationships_[constraint_id - 1U];
}

std::span<const ConstraintId> ConstraintRegistry::dependencies(
    const market::MarketId market_id) const noexcept {
    const auto found = dependencies_.find(market_id);
    return found == dependencies_.end() ? std::span<const ConstraintId>{}
                                        : std::span<const ConstraintId>{found->second};
}

ConstraintId ConstraintRegistry::add_relationship(Relationship relationship) {
    if (!relationship_is_valid(relationship)) {
        return 0U;
    }
    for (std::size_t index = 0U; index < relationships_.size(); ++index) {
        if (relationships_equal(relationships_[index], relationship)) {
            return static_cast<ConstraintId>(index + 1U);
        }
    }
    if (relationships_.size() >= std::numeric_limits<ConstraintId>::max()) {
        return 0U;
    }

    const auto constraint_id = static_cast<ConstraintId>(relationships_.size() + 1U);
    const auto markets = markets_for(relationship);
    relationships_.push_back(std::move(relationship));
    dependencies_[markets[0U]].push_back(constraint_id);
    dependencies_[markets[1U]].push_back(constraint_id);
    return constraint_id;
}

}  // namespace eme::constraint
