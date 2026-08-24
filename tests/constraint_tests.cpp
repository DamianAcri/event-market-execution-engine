#include "eme/constraint/payoff.hpp"
#include "test_support.hpp"

#include <limits>
#include <variant>
#include <vector>

namespace {

namespace constraint = eme::constraint;

void test_registry(eme::test::Context& test) {
    constraint::ConstraintRegistry registry;
    test.expect(registry.add(constraint::Implication{0U, 2U}) == 0U &&
                    registry.add(constraint::Implication{1U, 1U}) == 0U,
                "registry rejects missing and self-referential markets");

    const auto implication = registry.add(constraint::Implication{2U, 1U});
    test.expect(implication == 1U && registry.size() == 1U,
                "registry assigns a stable ID to a curated implication");
    test.expect(registry.add(constraint::Implication{2U, 1U}) == implication &&
                    registry.size() == 1U,
                "duplicate implication registration is idempotent");

    const auto complement = registry.add(constraint::Complement{3U, 4U});
    test.expect(complement == 2U, "registry accepts a curated complement relation");
    test.expect(registry.add(constraint::Complement{4U, 3U}) == complement &&
                    registry.size() == 2U,
                "complement identity is independent of operand order");

    const auto market_two = registry.dependencies(2U);
    test.expect(market_two.size() == 1U && market_two.front() == implication,
                "dependency index maps an updated market to its constraints");
    test.expect(registry.dependencies(99U).empty(),
                "unknown market has no dependent constraints");
    test.expect(registry.find(implication) != nullptr && registry.find(0U) == nullptr &&
                    registry.find(99U) == nullptr,
                "constraint lookup is bounds checked");
}

void test_implication_portfolio(eme::test::Context& test) {
    const constraint::Relationship relation{constraint::Implication{2U, 1U}};
    const auto result = constraint::construct_guaranteed_portfolio(
        7U, relation, eme::test::quantity(250));
    const auto* portfolio = std::get_if<constraint::GuaranteedPortfolio>(&result);
    test.expect(portfolio != nullptr, "implication produces a verified payoff portfolio");
    if (portfolio == nullptr) {
        return;
    }
    test.expect(portfolio->constraint_id == 7U && portfolio->legs.size() == 2U,
                "portfolio retains constraint identity and concrete legs");
    test.expect(portfolio->legs[0U].market_id == 1U &&
                    portfolio->legs[0U].outcome == constraint::ContractOutcome::yes &&
                    portfolio->legs[1U].market_id == 2U &&
                    portfolio->legs[1U].outcome == constraint::ContractOutcome::no,
                "A implies B constructs BUY YES(B) plus BUY NO(A)");
    test.expect(portfolio->minimum_payout.raw() == 250 &&
                    portfolio->worlds_verified == 3U,
                "implication portfolio pays at least one requested unit in every valid world");
}

void test_complement_portfolio(eme::test::Context& test) {
    const constraint::Relationship relation{constraint::Complement{3U, 4U}};
    const auto result = constraint::construct_guaranteed_portfolio(
        2U, relation, eme::test::quantity(100));
    const auto* portfolio = std::get_if<constraint::GuaranteedPortfolio>(&result);
    test.expect(portfolio != nullptr, "complement produces a verified payoff portfolio");
    if (portfolio == nullptr) {
        return;
    }
    test.expect(portfolio->legs[0U].outcome == constraint::ContractOutcome::yes &&
                    portfolio->legs[1U].outcome == constraint::ContractOutcome::yes,
                "complement portfolio buys YES on both mutually complementary contracts");
    test.expect(portfolio->minimum_payout.raw() == 100 &&
                    portfolio->worlds_verified == 2U,
                "complement portfolio has an exact one-unit settlement floor");

    const auto invalid = constraint::construct_guaranteed_portfolio(
        0U, relation, eme::test::quantity(100));
    test.expect(std::get_if<constraint::VerificationError>(&invalid) != nullptr &&
                    std::get<constraint::VerificationError>(invalid) ==
                        constraint::VerificationError::invalid_relationship,
                "portfolio construction rejects an invalid constraint identity");
    const auto zero = constraint::construct_guaranteed_portfolio(
        2U, relation, eme::test::quantity(0));
    test.expect(std::get<constraint::VerificationError>(zero) ==
                    constraint::VerificationError::zero_quantity,
                "portfolio construction rejects a zero requested quantity");
}

void test_generic_world_oracle(eme::test::Context& test) {
    const std::vector<constraint::PayoffLeg> implication_legs{
        {1U, constraint::ContractOutcome::yes, eme::test::quantity(100)},
        {2U, constraint::ContractOutcome::no, eme::test::quantity(100)},
    };
    const std::vector<constraint::WorldState> all_boolean_worlds{
        {{{2U, false}, {1U, false}}},
        {{{2U, false}, {1U, true}}},
        {{{2U, true}, {1U, false}}},
        {{{2U, true}, {1U, true}}},
    };
    const auto unrestricted = constraint::verify_payoff(all_boolean_worlds, implication_legs);
    const auto* verification = std::get_if<constraint::PayoffVerification>(&unrestricted);
    test.expect(verification != nullptr && verification->minimum_payout.raw() == 0,
                "generic oracle finds no guarantee when the invalid implication world is allowed");

    const std::vector<constraint::WorldState> missing_assignment{{{{2U, false}}}};
    const auto missing = constraint::verify_payoff(missing_assignment, implication_legs);
    test.expect(std::get<constraint::VerificationError>(missing) ==
                    constraint::VerificationError::missing_assignment,
                "oracle rejects a world missing a portfolio market");

    const std::vector<constraint::WorldState> duplicate_assignment{
        {{{1U, true}, {1U, false}, {2U, true}}},
    };
    const auto duplicate = constraint::verify_payoff(duplicate_assignment, implication_legs);
    test.expect(std::get<constraint::VerificationError>(duplicate) ==
                    constraint::VerificationError::invalid_world,
                "oracle rejects contradictory duplicate assignments");

    const std::vector<constraint::PayoffLeg> zero_leg{
        {1U, constraint::ContractOutcome::yes, eme::test::quantity(0)},
    };
    const std::vector<constraint::WorldState> one_world{{{{1U, true}}}};
    const auto zero = constraint::verify_payoff(one_world, zero_leg);
    test.expect(std::get<constraint::VerificationError>(zero) ==
                    constraint::VerificationError::zero_quantity,
                "oracle rejects zero-sized legs");

    const auto maximum = *eme::core::Quantity::from_raw(
        std::numeric_limits<std::int64_t>::max());
    const std::vector<constraint::PayoffLeg> overflowing{
        {1U, constraint::ContractOutcome::yes, maximum},
        {1U, constraint::ContractOutcome::yes, maximum},
    };
    const auto overflow = constraint::verify_payoff(one_world, overflowing);
    test.expect(std::get<constraint::VerificationError>(overflow) ==
                    constraint::VerificationError::payout_overflow,
                "oracle fails closed on settlement arithmetic overflow");

    test.expect(std::get<constraint::VerificationError>(
                    constraint::verify_payoff({}, implication_legs)) ==
                    constraint::VerificationError::empty_world_set &&
                    std::get<constraint::VerificationError>(
                        constraint::verify_payoff(one_world, {})) ==
                        constraint::VerificationError::empty_portfolio,
                "oracle rejects vacuous world sets and portfolios");
}

}  // namespace

int main() {
    eme::test::Context test;
    test_registry(test);
    test_implication_portfolio(test);
    test_complement_portfolio(test);
    test_generic_world_oracle(test);
    return test.result();
}
