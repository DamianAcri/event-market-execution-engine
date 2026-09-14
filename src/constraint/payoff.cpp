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

[[nodiscard]] bool has_valid_metadata(const ConstraintDefinition& definition) {
    return definition.id != 0U && definition.semantic_version != 0U &&
           !definition.key.empty() && !definition.provenance.empty();
}

}  // namespace

CompilationResult compile_constraint(ConstraintDefinition definition) {
    if (!has_valid_metadata(definition)) {
        return VerificationError::invalid_definition;
    }

    CompiledConstraint compiled;
    compiled.metadata = std::move(definition);
    const bool valid = std::visit(
        [&compiled](const auto& relationship) {
            using RelationshipType = std::decay_t<decltype(relationship)>;
            if constexpr (std::is_same_v<RelationshipType, Implication>) {
                if (relationship.antecedent == 0U || relationship.consequent == 0U ||
                    relationship.antecedent == relationship.consequent) {
                    return false;
                }
                compiled.dependent_markets = {
                    relationship.antecedent,
                    relationship.consequent,
                };
                compiled.valid_worlds = {
                    {{{relationship.antecedent, false}, {relationship.consequent, false}}},
                    {{{relationship.antecedent, false}, {relationship.consequent, true}}},
                    {{{relationship.antecedent, true}, {relationship.consequent, true}}},
                };
                compiled.guaranteed_leg_templates = {
                    {relationship.consequent, ContractOutcome::yes},
                    {relationship.antecedent, ContractOutcome::no},
                };
            } else {
                if (relationship.left == 0U || relationship.right == 0U ||
                    relationship.left == relationship.right) {
                    return false;
                }
                compiled.dependent_markets = {relationship.left, relationship.right};
                compiled.valid_worlds = {
                    {{{relationship.left, false}, {relationship.right, true}}},
                    {{{relationship.left, true}, {relationship.right, false}}},
                };
                compiled.guaranteed_leg_templates = {
                    {relationship.left, ContractOutcome::yes},
                    {relationship.right, ContractOutcome::yes},
                };
            }
            return true;
        },
        compiled.metadata.relationship);
    return valid ? CompilationResult{std::move(compiled)}
                 : CompilationResult{VerificationError::invalid_definition};
}

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
            if (!wins) {
                continue;
            }
            const auto settlement_value = core::contract_settlement_value(leg.quantity);
            if (!settlement_value.has_value() ||
                payout > std::numeric_limits<std::int64_t>::max() -
                             settlement_value->raw()) {
                return VerificationError::payout_overflow;
            }
            payout += settlement_value->raw();
        }
        minimum = std::min(minimum, payout);
    }

    return PayoffVerification{*core::Cash::from_raw(minimum), valid_worlds.size()};
}

PortfolioResult construct_guaranteed_portfolio(
    const CompiledConstraint& constraint,
    const core::Quantity quantity) {
    if (quantity.raw() == 0) {
        return VerificationError::zero_quantity;
    }

    std::vector<PayoffLeg> legs;
    legs.reserve(constraint.guaranteed_leg_templates.size());
    for (const auto& leg : constraint.guaranteed_leg_templates) {
        legs.push_back({leg.market_id, leg.outcome, quantity});
    }

    const auto expected_payout = core::contract_settlement_value(quantity);
    if (!expected_payout.has_value()) {
        return VerificationError::payout_overflow;
    }
    const auto verification = verify_payoff(constraint.valid_worlds, legs);
    const auto* verified = std::get_if<PayoffVerification>(&verification);
    if (verified == nullptr) {
        return std::get<VerificationError>(verification);
    }
    if (verified->minimum_payout != *expected_payout) {
        return VerificationError::invalid_definition;
    }
    return GuaranteedPortfolio{
        constraint.metadata.id,
        std::move(legs),
        verified->minimum_payout,
        verified->worlds_evaluated,
    };
}

ConstraintRegistrationResult ConstraintRegistry::add(ConstraintDefinition definition) {
    auto compiled_result = compile_constraint(std::move(definition));
    auto* compiled = std::get_if<CompiledConstraint>(&compiled_result);
    if (compiled == nullptr) {
        return ConstraintRegistrationResult::invalid_definition;
    }

    if (const auto found = constraints_.find(compiled->metadata.id);
        found != constraints_.end()) {
        return found->second == *compiled
                   ? ConstraintRegistrationResult::already_registered
                   : ConstraintRegistrationResult::constraint_id_conflict;
    }
    if (const auto found = keys_.find(compiled->metadata.key); found != keys_.end()) {
        return ConstraintRegistrationResult::key_conflict;
    }

    const auto constraint_id = compiled->metadata.id;
    const auto stored = constraints_.emplace(constraint_id, std::move(*compiled)).first;
    keys_.emplace(stored->second.metadata.key, constraint_id);
    for (const auto market_id : stored->second.dependent_markets) {
        dependencies_[market_id].push_back(constraint_id);
    }
    return ConstraintRegistrationResult::registered;
}

const CompiledConstraint* ConstraintRegistry::find(
    const ConstraintId constraint_id) const noexcept {
    const auto found = constraints_.find(constraint_id);
    return found == constraints_.end() ? nullptr : &found->second;
}

std::span<const ConstraintId> ConstraintRegistry::dependencies(
    const market::MarketId market_id) const noexcept {
    const auto found = dependencies_.find(market_id);
    return found == dependencies_.end() ? std::span<const ConstraintId>{}
                                        : std::span<const ConstraintId>{found->second};
}

}  // namespace eme::constraint
