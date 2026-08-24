#pragma once

#include "eme/market/normalized_event.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
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
};

struct Complement final {
    market::MarketId left{};
    market::MarketId right{};
};

using Relationship = std::variant<Implication, Complement>;

struct PayoffLeg final {
    market::MarketId market_id{};
    ContractOutcome outcome{ContractOutcome::yes};
    core::Quantity quantity;
};

struct WorldAssignment final {
    market::MarketId market_id{};
    bool settles_yes{};
};

struct WorldState final {
    std::vector<WorldAssignment> assignments;
};

enum class VerificationError : std::uint8_t {
    empty_world_set,
    empty_portfolio,
    invalid_world,
    missing_assignment,
    zero_quantity,
    payout_overflow,
    invalid_relationship,
};

struct PayoffVerification final {
    core::Quantity minimum_payout;
    std::size_t worlds_evaluated{};
};

using VerificationResult = std::variant<PayoffVerification, VerificationError>;

[[nodiscard]] VerificationResult verify_payoff(
    std::span<const WorldState> valid_worlds,
    std::span<const PayoffLeg> portfolio);

struct GuaranteedPortfolio final {
    ConstraintId constraint_id{};
    std::vector<PayoffLeg> legs;
    core::Quantity minimum_payout;
    std::size_t worlds_verified{};
};

using PortfolioResult = std::variant<GuaranteedPortfolio, VerificationError>;

[[nodiscard]] PortfolioResult construct_guaranteed_portfolio(
    ConstraintId constraint_id,
    const Relationship& relationship,
    core::Quantity quantity);

class ConstraintRegistry final {
public:
    [[nodiscard]] ConstraintId add(Implication relationship);
    [[nodiscard]] ConstraintId add(Complement relationship);
    [[nodiscard]] const Relationship* find(ConstraintId constraint_id) const noexcept;
    [[nodiscard]] std::span<const ConstraintId> dependencies(
        market::MarketId market_id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return relationships_.size(); }

private:
    [[nodiscard]] ConstraintId add_relationship(Relationship relationship);

    std::vector<Relationship> relationships_;
    std::unordered_map<market::MarketId, std::vector<ConstraintId>> dependencies_;
};

}  // namespace eme::constraint
