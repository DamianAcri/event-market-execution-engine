#include "cli/market_screen.hpp"
#include "eme/core/net_sizing.hpp"
#include "test_support.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>

namespace {
using Json = nlohmann::json;
namespace cli = eme::cli;
namespace kalshi = eme::gateway::kalshi;

Json metadata_json() {
    return Json::parse(R"({"schema_version":1,"metadata_version":1,"venue":"kalshi",
      "markets":[{"id":1,"ticker":"LOW"},{"id":2,"ticker":"HIGH"}],
      "constraints":[{"id":7,"semantic_version":1,"key":"high-implies-low","provenance":"synthetic reviewed threshold rules",
      "relationship":{"type":"implication","antecedent":2,"consequent":1}}]})");
}

Json fixture() {
    return Json::parse(R"({"schema_version":1,"as_of_ms":10000,"max_age_ms":100,"max_skew_ms":30,"max_total_evaluations":10000,
    "sizing":{"cap_centicontracts":300,"step_centicontracts":100,"available_cash_micro":1000000000,"minimum_margin_micro":0,"max_evaluations":1000},
    "fees":[{"market_id":1,"coefficient_ppm":0,"balance_quantum_micro":10000},{"market_id":2,"coefficient_ppm":0,"balance_quantum_micro":10000}],
    "books":[{"market_id":1,"request_time_ms":9950,"received_time_ms":9960,"yes_bids":[[1000,100]],"no_bids":[[3000,100],[6000,200]]},
             {"market_id":2,"request_time_ms":9950,"received_time_ms":9960,"yes_bids":[[7000,300]],"no_bids":[[1000,100]]}]})");
}

Json evaluate(const Json& input, const Json& metadata = metadata_json()) {
    const auto parsed = kalshi::parse_metadata_snapshot(metadata.dump());
    const auto result = cli::screen_markets(std::get<kalshi::MetadataSnapshot>(parsed), input.dump());
    if (const auto* error = std::get_if<cli::MarketScreenError>(&result)) {
        throw std::runtime_error(error->field);
    }
    return Json::parse(std::get<std::string>(result));
}

void rejection(eme::test::Context& test, const Json& input, const char* message) {
    const auto parsed = kalshi::parse_metadata_snapshot(metadata_json().dump());
    test.expect(std::holds_alternative<cli::MarketScreenError>(cli::screen_markets(
        std::get<kalshi::MetadataSnapshot>(parsed), input.dump())), message);
}

void status(eme::test::Context& test, const Json& input, const std::string& expected) {
    const auto result = evaluate(input);
    test.expect(result["constraints"][0]["status"] == expected && result["constraints"][0]["quote"].is_null(),
        "unusable relationships return the precise classification and no quote");
}

void correctness(eme::test::Context& test) {
    const auto result = evaluate(fixture());
    const auto& row = result["constraints"][0];
    const auto& quote = row["quote"];
    test.expect(result["kind"] == "indicative_rest_screen" && !result["synchronous_books"].get<bool>() &&
        !result["simulated_fills"].get<bool>() && !result["joint_capital_allocation"].get<bool>(), "REST quotes have explicit economic limits");
    test.expect(row["constraint_id"] == 7 && row["status"] == "optimal" && quote["quantity_centicontracts"] == 200 &&
        quote["net_margin_micro"] == 600000 && quote["debit_micro"] == 1400000 && quote["payout_floor_micro"] == 2000000,
        "depth cliff chooses two contracts; flat third contract is avoided by funding tie break");
    test.expect(quote["legs"][0]["outcome"] == "yes" && quote["legs"][0]["limit_price_1e4"] == 4000 &&
        quote["legs"][1]["outcome"] == "no" && quote["legs"][1]["limit_price_1e4"] == 3000,
        "threshold implication buys YES low and NO high from the opposite bid books");
    test.expect(row["one_contract_diagnostic"]["net_margin_micro"] == 300000 && row["maximum_age_ms"] == 50 && row["capture_span_ms"] == 10,
        "fixed diagnostic and conservative capture time bounds are exact");

    auto changed = fixture();
    changed["fees"][0]["coefficient_ppm"] = 70000;
    changed["fees"][1]["coefficient_ppm"] = 70000;
    const auto with_fees = evaluate(changed)["constraints"][0];
    test.expect(with_fees["one_contract_diagnostic"]["net_margin_micro"] == 260000 &&
        with_fees["quote"]["net_margin_micro"] == 530000,
        "production fee rounding is included in both diagnostic and optimized margin");

    changed = fixture();
    changed["books"][0]["no_bids"] = Json::array({Json::array({5000, 300})});
    changed["books"][1]["yes_bids"] = Json::array({Json::array({5000, 300})});
    changed["fees"][0]["coefficient_ppm"] = 70000;
    changed["fees"][1]["coefficient_ppm"] = 70000;
    const auto negative = evaluate(changed)["constraints"][0];
    test.expect(negative["status"] == "no_positive_margin" && negative["quote"].is_null() &&
        negative["one_contract_diagnostic"]["net_margin_micro"] == -40000 && negative["one_contract_diagnostic"]["gross_margin_micro"] == 0,
        "nonpositive opportunities retain a signed fully costed one-contract diagnostic");

    changed = fixture();
    changed["books"][0]["no_bids"] = Json::array({Json::array({10000, 300})});
    const auto boundary = evaluate(changed)["constraints"][0];
    test.expect(boundary["quote"]["legs"][0]["limit_price_1e4"] == 0,
        "one-dollar opposite bid maps to zero-price acquisition without indexing overflow");

    changed = fixture();
    changed["books"][1]["yes_bids"] = Json::array({Json::array({0, 300})});
    test.expect(evaluate(changed)["constraints"][0]["one_contract_diagnostic"]["net_margin_micro"] == -400000,
        "zero bid maps to one-dollar acquisition without suppressing negative diagnostics");

    changed = fixture();
    std::reverse(changed["books"].begin(), changed["books"].end());
    std::reverse(changed["fees"].begin(), changed["fees"].end());
    test.expect(evaluate(changed) == result, "input row ordering does not affect screening output");

    changed = fixture();
    changed["books"][1]["no_bids"] = Json::array({Json::array({7000, 300})});
    auto complementary = metadata_json();
    complementary["constraints"][0]["relationship"] = {{"type", "complement"}, {"left", 1}, {"right", 2}};
    const auto complement = evaluate(changed, complementary)["constraints"][0]["quote"];
    test.expect(complement["legs"][0]["outcome"] == "yes" && complement["legs"][1]["outcome"] == "yes" &&
        complement["net_margin_micro"] == 600000, "complement templates use both YES sides from the existing payoff compiler");

    changed = fixture();
    changed["sizing"]["cap_centicontracts"] = 25;
    changed["sizing"]["step_centicontracts"] = 1;
    changed["books"][0]["no_bids"] = Json::array({Json::array({10000, 25})});
    changed["books"][1]["yes_bids"] = Json::array({Json::array({10000, 25})});
    const auto fractional = evaluate(changed)["constraints"][0];
    test.expect(fractional["quote"]["quantity_centicontracts"] == 25 && fractional["quote"]["net_margin_micro"] == 250000 &&
        fractional["one_contract_diagnostic"].is_null(), "fractional sizing retains exact cents and omits unavailable whole-contract diagnostics");
}

void invalid_and_missing(eme::test::Context& test) {
    auto changed = fixture();
    changed["books"].erase(1U); status(test, changed, "missing_book");
    changed = fixture(); changed["fees"].erase(1U); status(test, changed, "missing_fee");
    changed = fixture(); changed["books"][0]["request_time_ms"] = 9800; status(test, changed, "stale_book");
    changed = fixture(); changed["books"][0]["request_time_ms"] = 9920; status(test, changed, "nonsynchronous_books");
    changed = fixture(); changed["books"][0]["no_bids"] = Json::array(); status(test, changed, "no_depth");
    changed = fixture(); changed["books"][0]["no_bids"] = Json::array({Json::array({6000, 99})});
    status(test, changed, "no_depth");
    changed = fixture(); changed["sizing"]["available_cash_micro"] = 1; status(test, changed, "insufficient_cash");
    changed = fixture(); changed["sizing"]["max_evaluations"] = 1; status(test, changed, "search_budget_exceeded");
    test.expect(!evaluate(changed)["solver_complete"].get<bool>(), "a truncated solver never reports complete");
    changed = fixture(); changed["books"][0]["request_time_ms"] = 9970; rejection(test, changed, "negative request interval rejects input");
    changed = fixture(); changed["as_of_ms"] = 9900; rejection(test, changed, "future received books reject input");
    changed = fixture(); changed["books"][0]["market_id"] = 2; rejection(test, changed, "duplicate book IDs reject input");
    changed = fixture(); changed["books"][0]["market_id"] = 99; rejection(test, changed, "unregistered book IDs reject input");
    changed = fixture(); changed["books"][0]["yes_bids"][0][1] = 0; rejection(test, changed, "zero depth is malformed rather than executable");
    changed = fixture(); changed["books"][0]["yes_bids"][0][0] = 10001; rejection(test, changed, "out-of-range price rejects input");
    changed = fixture(); changed["books"][0]["yes_bids"][0][0] = 0.5; rejection(test, changed, "floating-point normalization is forbidden");
    changed = fixture(); changed["books"][0]["no_bids"][1][0] = 3000; rejection(test, changed, "duplicate price levels reject input");
    changed = fixture(); changed["books"][0]["no_bids"][1][0] = 2000; rejection(test, changed, "unsorted price levels reject input");
    changed = fixture(); changed["fees"][0]["balance_quantum_micro"] = 1000; rejection(test, changed, "unsupported fee quantum rejects input");
    changed = fixture(); changed["fees"][0]["coefficient_ppm"] = 1000001; rejection(test, changed, "out-of-range coefficient rejects input");
    changed = fixture(); changed["max_total_evaluations"] = 0; rejection(test, changed, "zero total budget rejects input");
    changed = fixture(); changed["sizing"]["step_centicontracts"] = 50; rejection(test, changed, "unsupported sizing grid rejects input");
    changed = fixture(); changed["unexpected"] = true; rejection(test, changed, "unknown fields fail closed");
    const auto metadata = kalshi::parse_metadata_snapshot(metadata_json().dump());
    test.expect(std::holds_alternative<cli::MarketScreenError>(cli::screen_markets(std::get<kalshi::MetadataSnapshot>(metadata),
        R"({"schema_version":1,"schema_version":1})")), "duplicate JSON keys fail closed");
}

void total_budget(eme::test::Context& test) {
    auto metadata = metadata_json();
    metadata["markets"].push_back({{"id", 3}, {"ticker", "HIGHER"}});
    auto relation = metadata["constraints"][0];
    relation["id"] = 8;
    relation["key"] = "higher-implies-low";
    relation["relationship"]["antecedent"] = 3;
    metadata["constraints"].push_back(relation);
    auto input = fixture();
    auto book = input["books"][1]; book["market_id"] = 3; input["books"].push_back(book);
    auto fee = input["fees"][1]; fee["market_id"] = 3; input["fees"].push_back(fee);
    input["max_total_evaluations"] = 1;
    const auto result = evaluate(input, metadata);
    test.expect(result["evaluated_quantities"] == 1 && !result["solver_complete"].get<bool>() &&
        result["constraints"][1]["status"] == "screening_budget_exceeded" && result["constraints"][1]["quote"].is_null(),
        "total budget is shared across constraints and exhaustion remains explicit");
}

void exhaustive_wiring(eme::test::Context& test) {
    // Independently enumerate whole-contract quantities and production fill fees:
    // checks JSON bid inversion, side mapping and aggregate output, not a second optimizer.
    for (int sample = 0; sample < 40; ++sample) {
        auto input = fixture();
        input["sizing"]["cap_centicontracts"] = 500;
        input["books"][0]["no_bids"] = Json::array({Json::array({2500 + sample * 50, 300}), Json::array({5000 + sample * 50, 200})});
        input["books"][1]["yes_bids"] = Json::array({Json::array({6000 + sample * 25, 500})});
        input["fees"][0]["coefficient_ppm"] = 70000;
        input["fees"][1]["coefficient_ppm"] = 35000;
        const auto row = evaluate(input)["constraints"][0];
        std::int64_t best = 0;
        std::int64_t best_q = 0;
        for (std::int64_t q = 100; q <= 500; q += 100) {
            std::int64_t debit = 0;
            for (std::size_t leg = 0; leg < 2U; ++leg) {
                const auto& levels = input["books"][leg][leg == 0U ? "no_bids" : "yes_bids"];
                eme::core::FeeAccumulator accumulator;
                auto remaining = q;
                for (auto it = levels.rbegin(); it != levels.rend() && remaining > 0; ++it) {
                    const auto amount = std::min(remaining, (*it)[1].get<std::int64_t>());
                    const auto charge = eme::core::charge_buy_fill(eme::test::quantity(amount),
                        eme::test::price(10000 - (*it)[0].get<std::int64_t>()),
                        {input["fees"][leg]["coefficient_ppm"].get<std::uint32_t>(), 10000}, accumulator);
                    debit += charge->debit.raw();
                    remaining -= amount;
                }
            }
            const auto margin = q * 10000 - debit;
            if (margin > best) { best = margin; best_q = q; }
        }
        test.expect(best_q == 0 ? row["quote"].is_null() :
            row["quote"]["quantity_centicontracts"] == best_q && row["quote"]["net_margin_micro"] == best,
            "screen sizing matches exhaustive grid with asymmetric fees and depth changes");
    }
}

void benchmark(eme::test::Context& test) {
    auto metadata = metadata_json();
    auto input = fixture();
    metadata["markets"] = Json::array(); metadata["constraints"] = Json::array();
    input["books"] = Json::array(); input["fees"] = Json::array();
    const auto base = fixture();
    for (int pair = 0; pair < 512; ++pair) {
        for (std::size_t leg = 0; leg < 2U; ++leg) {
            const auto id = pair * 2 + static_cast<int>(leg) + 1;
            metadata["markets"].push_back({{"id", id}, {"ticker", "T" + std::to_string(id)}});
            auto book = base["books"][leg]; book["market_id"] = id; input["books"].push_back(book);
            auto fee = base["fees"][leg]; fee["market_id"] = id; input["fees"].push_back(fee);
        }
        metadata["constraints"].push_back({{"id", pair + 1}, {"semantic_version", 1}, {"key", "pair-" + std::to_string(pair)},
            {"provenance", "synthetic benchmark"}, {"relationship", {{"type", "implication"}, {"antecedent", pair * 2 + 2}, {"consequent", pair * 2 + 1}}}});
    }
    const auto parsed = kalshi::parse_metadata_snapshot(metadata.dump());
    const auto& snapshot = std::get<kalshi::MetadataSnapshot>(parsed);
    const auto bytes = input.dump();
    const auto expected = cli::screen_markets(snapshot, bytes);
    test.expect(Json::parse(std::get<std::string>(expected))["positive_quotes"] == 512, "1024-market batch includes every pair");
    constexpr int repetitions = 20;
    const auto begin = std::chrono::steady_clock::now();
    for (int repeat = 0; repeat < repetitions; ++repeat) {
        const auto actual = cli::screen_markets(snapshot, bytes);
        test.expect(std::get<std::string>(actual) == std::get<std::string>(expected), "batch benchmark outputs remain byte-identical");
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    std::cout << "screen_markets=1024 constraints=512 samples=" << repetitions << " mean_ms=" << elapsed / repetitions << '\n';
}
}  // namespace

int main(const int argc, const char* const argv[]) {
    eme::test::Context test;
    correctness(test);
    invalid_and_missing(test);
    total_budget(test);
    exhaustive_wiring(test);
    if (argc == 2 && std::string_view{argv[1]} == "--benchmark") { benchmark(test); }
    return test.result();
}
