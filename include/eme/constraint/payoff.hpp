#pragma once

#include "eme/market/normalized_event.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace eme::constraint {

using ConstraintId = std::uint32_t;

enum class ContractOutcome : std::uint8_t {
    yes,
    no,
};

struct Implication final {
    market::MarketId antecedent{};
    market::MarketId consequent{};

    friend bool operator==(const Implication&, const Implication&) = default;
};

struct Complement final {
    market::MarketId left{};
    market::MarketId right{};

    friend bool operator==(const Complement&, const Complement&) = default;
};

using Relationship = std::variant<Implication, Complement>;

struct ConstraintDefinition final {
    ConstraintId id{};
    std::uint32_t semantic_version{};
    std::string key;
    std::string provenance;
    Relationship relationship;

    friend bool operator==(const ConstraintDefinition&, const ConstraintDefinition&) = default;
};

struct PayoffLeg final {
    market::MarketId market_id{};
    ContractOutcome outcome{ContractOutcome::yes};
    core::Quantity quantity;
};

struct PayoffLegTemplate final {
    market::MarketId market_id{};
    ContractOutcome outcome{ContractOutcome::yes};

    friend bool operator==(const PayoffLegTemplate&, const PayoffLegTemplate&) = default;
};

struct WorldAssignment final {
    market::MarketId market_id{};
    bool settles_yes{};

    friend bool operator==(const WorldAssignment&, const WorldAssignment&) = default;
};

struct WorldState final {
    std::vector<WorldAssignment> assignments;

    friend bool operator==(const WorldState&, const WorldState&) = default;
};

struct CompiledConstraint final {
    ConstraintDefinition metadata;
    std::vector<market::MarketId> dependent_markets;
    std::vector<WorldState> valid_worlds;
    std::vector<PayoffLegTemplate> guaranteed_leg_templates;

    friend bool operator==(const CompiledConstraint&, const CompiledConstraint&) = default;
};

enum class VerificationError : std::uint8_t {
    empty_world_set,
    empty_portfolio,
    invalid_world,
    missing_assignment,
    zero_quantity,
    payout_overflow,
    invalid_definition,
};

struct PayoffVerification final {
    core::Cash minimum_payout;
    std::size_t worlds_evaluated{};
};

using CompilationResult = std::variant<CompiledConstraint, VerificationError>;
using VerificationResult = std::variant<PayoffVerification, VerificationError>;

[[nodiscard]] CompilationResult compile_constraint(ConstraintDefinition definition);

[[nodiscard]] VerificationResult verify_payoff(
    std::span<const WorldState> valid_worlds,
    std::span<const PayoffLeg> portfolio);

struct GuaranteedPortfolio final {
    ConstraintId constraint_id{};
    std::vector<PayoffLeg> legs;
    core::Cash minimum_payout;
    std::size_t worlds_verified{};
};

using PortfolioResult = std::variant<GuaranteedPortfolio, VerificationError>;

[[nodiscard]] PortfolioResult construct_guaranteed_portfolio(
    const CompiledConstraint& constraint,
    core::Quantity quantity);

enum class ConstraintRegistrationResult : std::uint8_t {
    registered,
    already_registered,
    invalid_definition,
    constraint_id_conflict,
    key_conflict,
};

class ConstraintRegistry final {
public:
    [[nodiscard]] ConstraintRegistrationResult add(ConstraintDefinition definition);
    [[nodiscard]] const CompiledConstraint* find(ConstraintId constraint_id) const noexcept;
    [[nodiscard]] std::span<const ConstraintId> dependencies(
        market::MarketId market_id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return constraints_.size(); }
    // Startup enumeration; independent of registration/hash-table order.
    [[nodiscard]] std::vector<ConstraintId> sorted_ids() const;

private:
    std::unordered_map<ConstraintId, CompiledConstraint> constraints_;
    std::unordered_map<std::string, ConstraintId> keys_;
    std::unordered_map<market::MarketId, std::vector<ConstraintId>> dependencies_;
};

}  // namespace eme::constraint
