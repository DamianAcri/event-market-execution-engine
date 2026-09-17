#include "cli/basket_screen.hpp"
#include "test_support.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace {
using Json = nlohmann::json;
namespace cli = eme::cli;

Json fixture() {
    return Json::parse(R"({"schema_version":1,"markets":[{"id":1,"ticker":"LOW"},{"id":2,"ticker":"HIGH"},{"id":3,"ticker":"RANGE"}],
    "baskets":[{"id":7,"key":"synthetic-range","lower_market_id":1,"upper_market_id":2,"range_market_id":3,
    "lower_threshold_cents":10000,"upper_threshold_cents":20000,"interval_lower_cents":10001,"interval_upper_cents":20000}],
    "as_of_ms":10000,"max_age_ms":100,"max_skew_ms":30,"max_total_evaluations":10000,
    "sizing":{"cap_centicontracts":1000,"step_centicontracts":100,"available_cash_micro":1000000000,"minimum_margin_micro":0,"max_evaluations":1000},
    "fees":[{"market_id":1,"coefficient_ppm":70000,"balance_quantum_micro":10000},{"market_id":2,"coefficient_ppm":70000,"balance_quantum_micro":10000},{"market_id":3,"coefficient_ppm":70000,"balance_quantum_micro":10000}],
    "books":[{"market_id":1,"request_time_ms":9950,"received_time_ms":9960,"yes_bids":[[100,1000]],"no_bids":[[200,1000]]},
             {"market_id":2,"request_time_ms":9950,"received_time_ms":9960,"yes_bids":[[200,1000]],"no_bids":[[100,1000]]},
             {"market_id":3,"request_time_ms":9950,"received_time_ms":9960,"yes_bids":[[9900,1000]],"no_bids":[[0,1000]]}]})");
}

Json evaluate(const Json& input) {
    const auto result = cli::screen_baskets(input.dump());
    if (const auto* error = std::get_if<cli::BasketScreenError>(&result)) { throw std::runtime_error(error->field); }
    return Json::parse(std::get<std::string>(result));
}

void rejection(eme::test::Context& test, const Json& input, const char* message) {
    test.expect(std::holds_alternative<cli::BasketScreenError>(cli::screen_baskets(input.dump())), message);
}

void expect_status(eme::test::Context& test, const Json& input, const std::string& expected) {
    const auto output = evaluate(input);
    test.expect(output["baskets"][0]["status"] == expected && output["baskets"][0]["quote"].is_null(),
        "unusable basket has exact status and no economic quote");
}

void costing(eme::test::Context& test) {
    const auto result = evaluate(fixture());
    const auto& row = result["baskets"][0];
    const auto& quote = row["quote"];
    test.expect(result["kind"] == "conditional_payoff_screen" && !result["production_certificate"].get<bool>() &&
        !result["settlement_rules_verified_by_native_screen"].get<bool>() && !result["simulated_fills"].get<bool>() &&
        !result["joint_capital_allocation"].get<bool>(), "arithmetic output explicitly separates conditional payoff from execution and rule certification");
    test.expect(row["one_contract_diagnostic"]["net_margin_micro"] == 0 && quote["quantity_centicontracts"] == 1000 &&
        quote["payout_floor_micro"] == 20000000 && quote["debit_micro"] == 19750000 && quote["net_margin_micro"] == 250000,
        "one contract breaks even while ten earn conditional 25-cent margin after exact per-leg fees");
    test.expect(quote["legs"][0]["market_id"] == 1 && quote["legs"][0]["outcome"] == "yes" &&
        quote["legs"][1]["market_id"] == 2 && quote["legs"][1]["outcome"] == "no" &&
        quote["legs"][2]["market_id"] == 3 && quote["legs"][2]["outcome"] == "no", "fixed semantic orientation cannot be changed by caller");
    test.expect(result["evaluated_quantities"] == 10 && result["solver_complete"].get<bool>() && result["positive_quotes"] == 1 &&
        row["maximum_age_ms"] == 50 && row["capture_span_ms"] == 10, "bounded exact enumeration and conservative client timing are explicit");

    auto changed = fixture();
    changed["sizing"]["cap_centicontracts"] = 200;
    const auto two = evaluate(changed)["baskets"][0]["quote"];
    test.expect(two["quantity_centicontracts"] == 200 && two["net_margin_micro"] == 30000,
        "a zero-margin one-contract diagnostic never suppresses profitable two-contract quantity");

    changed = fixture();
    changed["sizing"]["cap_centicontracts"] = 100;
    expect_status(test, changed, "no_positive_margin");
    changed["books"][2]["yes_bids"][0][0] = 9800;
    test.expect(evaluate(changed)["baskets"][0]["one_contract_diagnostic"]["net_margin_micro"] == -10000,
        "negative signed diagnostic is preserved for evidence");

    changed = fixture();
    for (auto& fee : changed["fees"]) { fee["coefficient_ppm"] = 0; }
    changed["sizing"]["cap_centicontracts"] = 300;
    changed["books"][0]["no_bids"] = Json::array({Json::array({0, 150}), Json::array({5000, 150})});
    changed["books"][1]["yes_bids"] = Json::array({Json::array({5000, 300})});
    changed["books"][2]["yes_bids"] = Json::array({Json::array({5000, 300})});
    const auto cliff = evaluate(changed)["baskets"][0]["quote"];
    test.expect(cliff["quantity_centicontracts"] == 200 && cliff["notional_micro"] == 3250000 &&
        cliff["net_margin_micro"] == 750000 && cliff["legs"][0]["notional_micro"] == 1250000,
        "fractional 1.5-contract depth spans levels; equal-margin third contract loses funding tie break");

    // Every level is smaller than one contract. Together these levels fund a
    // whole contract; independently flooring level quantities would lose it.
    changed["sizing"]["cap_centicontracts"] = 100;
    changed["books"][0]["no_bids"] = Json::array({Json::array({4000, 50}), Json::array({6000, 50})});
    const auto fragments = evaluate(changed)["baskets"][0];
    test.expect(fragments["quote"]["quantity_centicontracts"] == 100 && fragments["quote"]["notional_micro"] == 1500000 &&
        fragments["quote"]["net_margin_micro"] == 500000,
        "subcontract levels are aggregated without truncation and charged at their own prices");
    // Reserve with a centicontract minimum fill is intentionally much larger
    // than assumed debit. It bounds fragmentation rather than predicting fees.
    test.expect(fragments["quote"]["reservation_micro"] == 4600300 &&
        evaluate(changed)["funding_fill_step_centicontracts"] == 1,
        "three funding reservations include worst-level prices and centicontract-fragment rounding");
    changed["sizing"]["available_cash_micro"] = 4600299;
    expect_status(test, changed, "insufficient_cash");

    changed = fixture();
    std::reverse(changed["books"].begin(), changed["books"].end());
    std::reverse(changed["markets"].begin(), changed["markets"].end());
    std::reverse(changed["fees"].begin(), changed["fees"].end());
    test.expect(evaluate(changed) == result, "input row order does not affect deterministic output");
}

void invalid_and_missing(eme::test::Context& test) {
    auto changed = fixture(); changed["books"].erase(1U); expect_status(test, changed, "missing_book");
    changed = fixture(); changed["fees"].erase(1U); expect_status(test, changed, "missing_fee");
    changed = fixture(); changed["books"][0]["request_time_ms"] = 9800; expect_status(test, changed, "stale_book");
    changed = fixture(); changed["books"][0]["request_time_ms"] = 9920; expect_status(test, changed, "nonsynchronous_books");
    changed = fixture(); changed["books"][0]["no_bids"] = Json::array(); expect_status(test, changed, "no_depth");
    changed = fixture(); changed["books"][0]["no_bids"][0][1] = 99; expect_status(test, changed, "no_depth");
    changed = fixture(); changed["sizing"]["available_cash_micro"] = 1; expect_status(test, changed, "insufficient_cash");
    changed = fixture(); changed["sizing"]["max_evaluations"] = 2; expect_status(test, changed, "search_budget_exceeded");
    const auto incomplete = evaluate(changed);
    test.expect(!incomplete["solver_complete"].get<bool>() && incomplete["positive_quotes"] == 0 &&
        incomplete["baskets"][0]["evaluated_quantities"] == 2, "budget exhaustion discards a positive partial best instead of claiming optimum");
    changed = fixture(); changed["max_total_evaluations"] = 2; expect_status(test, changed, "search_budget_exceeded");
    changed = fixture(); changed["books"][0]["request_time_ms"] = 9970; rejection(test, changed, "inverted client interval rejected");
    changed = fixture(); changed["as_of_ms"] = 9900; rejection(test, changed, "future response rejected");
    changed = fixture(); changed["books"][0]["yes_bids"][0][0] = 9900; rejection(test, changed, "crossed book rejected");
    changed = fixture(); changed["books"][0]["no_bids"][0][1] = 0; rejection(test, changed, "zero depth rejected");
    changed = fixture(); changed["books"][0]["no_bids"][0][0] = 10001; rejection(test, changed, "invalid price rejected");
    changed = fixture(); changed["books"][0]["no_bids"][0][0] = 0.5; rejection(test, changed, "floating price rejected");
    changed = fixture(); changed["books"][0]["no_bids"].push_back(Json::array({200, 100})); rejection(test, changed, "duplicate price rejected");
    changed = fixture(); changed["books"][0]["no_bids"].push_back(Json::array({100, 100})); rejection(test, changed, "unsorted prices rejected");
    changed = fixture(); changed["books"][1]["market_id"] = 1; rejection(test, changed, "duplicate books rejected");
    changed = fixture(); changed["books"][0]["market_id"] = 999; rejection(test, changed, "unknown books rejected");
    changed = fixture(); changed["fees"][0]["coefficient_ppm"] = 1000001; rejection(test, changed, "unsupported fee rejected");
    changed = fixture(); changed["fees"][0]["balance_quantum_micro"] = 1000; rejection(test, changed, "unsupported balance precision rejected");
    changed = fixture(); changed["fees"][1]["market_id"] = 1; rejection(test, changed, "duplicate fees rejected");
    changed = fixture(); changed["sizing"]["cap_centicontracts"] = 101; rejection(test, changed, "cap must be whole contracts");
    changed = fixture(); changed["sizing"]["cap_centicontracts"] = 10100; rejection(test, changed, "quantity bound enforced");
    changed = fixture(); changed["sizing"]["step_centicontracts"] = 1; rejection(test, changed, "order grid is whole contracts only");
    changed = fixture(); changed["max_total_evaluations"] = 0; rejection(test, changed, "zero budget rejected");
    changed = fixture(); changed["baskets"][0]["lower_threshold_cents"] = 10001; rejection(test, changed, "strict threshold requires gap before inclusive lower bound");
    changed = fixture(); changed["baskets"][0]["interval_upper_cents"] = 20001; rejection(test, changed, "range must fit under high threshold");
    changed = fixture(); changed["baskets"][0]["interval_lower_cents"] = 20001; rejection(test, changed, "inverted interval rejected");
    changed = fixture(); changed["baskets"][0]["upper_market_id"] = 1; rejection(test, changed, "legs must be distinct");
    changed = fixture(); changed["baskets"][0]["id"] = 0; rejection(test, changed, "basket identifiers positive");
    changed = fixture(); changed["baskets"].push_back(changed["baskets"][0]); rejection(test, changed, "duplicate baskets rejected");
    changed = fixture(); changed["markets"][1]["ticker"] = "LOW"; rejection(test, changed, "tickers unique");
    changed = fixture(); changed["markets"][1]["id"] = 1; rejection(test, changed, "market IDs unique");
    changed = fixture(); changed["baskets"][0]["payout_floor_micro"] = 3000000; rejection(test, changed, "caller cannot override payoff floor");
    test.expect(std::holds_alternative<cli::BasketScreenError>(cli::screen_baskets(R"({"schema_version":1,"schema_version":1})")),
        "duplicate JSON keys rejected");
}

void shared_budget_and_semantics(eme::test::Context& test) {
    auto input = fixture();
    input["markets"].push_back({{"id", 4}, {"ticker", "RANGE2"}});
    auto basket = input["baskets"][0];
    basket["id"] = 8;
    basket["key"] = "synthetic-range-2";
    basket["range_market_id"] = 4;
    input["baskets"].push_back(basket);
    auto book = input["books"][2]; book["market_id"] = 4; input["books"].push_back(book);
    auto fee = input["fees"][2]; fee["market_id"] = 4; input["fees"].push_back(fee);
    input["max_total_evaluations"] = 10;
    const auto result = evaluate(input);
    test.expect(result["baskets"][0]["status"] == "optimal" && result["baskets"][1]["status"] == "screening_budget_exceeded" &&
        result["baskets"][1]["quote"].is_null() && !result["solver_complete"].get<bool>() && result["evaluated_quantities"] == 10,
        "shared global budget leaves subsequent basket unevaluated while retaining completed independent first result");
    auto reversed = input;
    std::reverse(reversed["baskets"].begin(), reversed["baskets"].end());
    test.expect(evaluate(reversed) == result, "basket ID order fixes reproducible allocation of global evaluation budget");
    input["baskets"][1]["lower_threshold_cents"] = 9999;
    rejection(test, input, "reusing a threshold market with different economic meaning is rejected");
}
}  // namespace

int main() {
    eme::test::Context test;
    costing(test);
    invalid_and_missing(test);
    shared_budget_and_semantics(test);
    return test.result();
}
