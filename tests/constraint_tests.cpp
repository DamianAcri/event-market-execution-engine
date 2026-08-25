#include "eme/constraint/payoff.hpp"
#include "test_support.hpp"

#include <limits>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace constraint = eme::constraint;

[[nodiscard]] constraint::ConstraintDefinition implication_definition() {
    return {7U, 1U, "election.a-implies-b", "research-dossier-2026-08", {constraint::Implication{2U, 1U}}};
}

[[nodiscard]] constraint::ConstraintDefinition complement_definition() {
    return {9U, 2U, "binary.complement", "exchange-rules-v2", {constraint::Complement{3U, 4U}}};
}

void test_compilation_and_registry(eme::test::Context& test) {
    constraint::ConstraintRegistry registry;
    auto invalid = implication_definition();
    invalid.id = 0U;
    test.expect(registry.add(std::move(invalid)) ==
                    constraint::ConstraintRegistrationResult::invalid_definition,
                "registry rejects definitions without stable metadata");

    const auto definition = implication_definition();
    test.expect(registry.add(definition) ==
                    constraint::ConstraintRegistrationResult::registered &&
                    registry.size() == 1U,
                "registry stores an explicitly identified compiled constraint");
    test.expect(registry.add(definition) ==
                    constraint::ConstraintRegistrationResult::already_registered,
                "exact constraint registration is idempotent");

    auto id_conflict = complement_definition();
    id_conflict.id = definition.id;
    test.expect(registry.add(std::move(id_conflict)) ==
                    constraint::ConstraintRegistrationResult::constraint_id_conflict,
                "registry rejects reuse of a stable constraint ID");
    auto key_conflict = complement_definition();
    key_conflict.key = definition.key;
    test.expect(registry.add(std::move(key_conflict)) ==
                    constraint::ConstraintRegistrationResult::key_conflict,
                "registry rejects reuse of a stable constraint key");

    const auto market_two = registry.dependencies(2U);
    const auto* compiled = registry.find(definition.id);
    test.expect(compiled != nullptr && compiled->metadata == definition &&
                    compiled->valid_worlds.size() == 3U &&
                    compiled->guaranteed_leg_templates.size() == 2U,
                "compiled model retains versioned provenance, worlds, and payoff template");
    test.expect(market_two.size() == 1U && market_two.front() == definition.id &&
                    registry.dependencies(99U).empty() && registry.find(99U) == nullptr,
                "dependency and identity lookup are bounds safe");
}

void test_implication_portfolio(eme::test::Context& test) {
    const auto compiled_result = constraint::compile_constraint(implication_definition());
    const auto* compiled = std::get_if<constraint::CompiledConstraint>(&compiled_result);
    test.expect(compiled != nullptr, "implication compiles to the canonical model");
    if (compiled == nullptr) {
        return;
    }

    const auto result = constraint::construct_guaranteed_portfolio(
        *compiled, eme::test::quantity(250));
    const auto* portfolio = std::get_if<constraint::GuaranteedPortfolio>(&result);
    test.expect(portfolio != nullptr, "implication produces a verified payoff portfolio");
    if (portfolio == nullptr) {
        return;
    }
    test.expect(portfolio->constraint_id == 7U && portfolio->legs.size() == 2U &&
                    portfolio->legs[0U].market_id == 1U &&
                    portfolio->legs[0U].outcome == constraint::ContractOutcome::yes &&
                    portfolio->legs[1U].market_id == 2U &&
                    portfolio->legs[1U].outcome == constraint::ContractOutcome::no,
                "A implies B constructs BUY YES(B) plus BUY NO(A)");
    test.expect(portfolio->minimum_payout.raw() == 2'500'000 &&
                    portfolio->worlds_verified == 3U,
                "2.50 contracts produce a distinct 2.50 cash settlement floor");
}

void test_complement_portfolio(eme::test::Context& test) {
    const auto compiled_result = constraint::compile_constraint(complement_definition());
    const auto* compiled = std::get_if<constraint::CompiledConstraint>(&compiled_result);
    test.expect(compiled != nullptr, "complement compiles to the canonical model");
    if (compiled == nullptr) {
        return;
    }
    const auto result = constraint::construct_guaranteed_portfolio(
        *compiled, eme::test::quantity(100));
    const auto* portfolio = std::get_if<constraint::GuaranteedPortfolio>(&result);
    test.expect(portfolio != nullptr && portfolio->minimum_payout.raw() == 1'000'000 &&
                    portfolio->worlds_verified == 2U,
                "complement portfolio has an exact one-dollar settlement floor");
    const auto zero = constraint::construct_guaranteed_portfolio(
        *compiled, eme::test::quantity(0));
    test.expect(std::get<constraint::VerificationError>(zero) ==
                    constraint::VerificationError::zero_quantity,
                "portfolio construction rejects a zero requested quantity");
}

void test_generic_world_oracle(eme::test::Context& test) {
    const std::vector<constraint::PayoffLeg> legs{
        {1U, constraint::ContractOutcome::yes, eme::test::quantity(100)},
        {2U, constraint::ContractOutcome::no, eme::test::quantity(100)},
    };
    const std::vector<constraint::WorldState> all_worlds{
        {{{2U, false}, {1U, false}}}, {{{2U, false}, {1U, true}}},
        {{{2U, true}, {1U, false}}}, {{{2U, true}, {1U, true}}},
    };
    const auto unrestricted = constraint::verify_payoff(all_worlds, legs);
    const auto* verification = std::get_if<constraint::PayoffVerification>(&unrestricted);
    test.expect(verification != nullptr && verification->minimum_payout.raw() == 0,
                "oracle finds no guarantee when the invalid implication world is allowed");

    const std::vector<constraint::WorldState> missing{{{{2U, false}}}};
    test.expect(std::get<constraint::VerificationError>(
                    constraint::verify_payoff(missing, legs)) ==
                    constraint::VerificationError::missing_assignment,
                "oracle rejects a world missing a portfolio market");
    const std::vector<constraint::WorldState> duplicate{
        {{{1U, true}, {1U, false}, {2U, true}}},
    };
    test.expect(std::get<constraint::VerificationError>(
                    constraint::verify_payoff(duplicate, legs)) ==
                    constraint::VerificationError::invalid_world,
                "oracle rejects duplicate assignments");

    const auto maximum = *eme::core::Quantity::from_raw(
        std::numeric_limits<std::int64_t>::max());
    const std::vector<constraint::PayoffLeg> overflowing{
        {1U, constraint::ContractOutcome::yes, maximum},
    };
    const std::vector<constraint::WorldState> one_world{{{{1U, true}}}};
    test.expect(std::get<constraint::VerificationError>(
                    constraint::verify_payoff(one_world, overflowing)) ==
                    constraint::VerificationError::payout_overflow,
                "oracle fails closed when contracts cannot be represented as cash");
}

}  // namespace

int main() {
    eme::test::Context test;
    test_compilation_and_registry(test);
    test_implication_portfolio(test);
    test_complement_portfolio(test);
    test_generic_world_oracle(test);
    return test.result();
}
