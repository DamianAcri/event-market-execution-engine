#include "study_fixture.hpp"
#include "session/study_positions.hpp"
using namespace eme::test::study;

namespace {
Json exit_policy() {
    auto result = policy();
    result["schema_version"] = 4U; result["strategy"] = "residual_exit_v4";
    result["max_sizing_evaluations"] = 100'000U;
    result["lifecycle"] = {{"execution_policy", "parallel_hold"}, {"first_leg", 0U}, {"response_latency_ns", {500U, 500U}},
        {"completion_timeout_ns", 5000U}, {"completion_loss_limit_micro_usd", 0U}, {"maximum_completion_orders", 2U},
        {"settlements", Json::array({{{"market_id", 1U}, {"yes_wins", false}, {"time_ns", 1'000'000U}, {"provenance", "Synthetic exit scenario"}},
            {{"market_id", 2U}, {"yes_wins", false}, {"time_ns", 1'000'000U}, {"provenance", "Synthetic exit scenario"}}})}};
    result["residual_exit"] = {{"mode", "reduce_once"}, {"arrival_latency_ns", 100U}, {"response_latency_ns", 300U},
        {"timeout_ns", 1'000'000U}, {"minimum_price_1e4", 1000U}, {"reject", false}, {"available_liquidity_bps", 10000U}};
    result["reject_legs"] = {true, false};
    return result;
}
Json actions(const std::string& trace) {
    Json result = Json::array(); std::istringstream lines{trace};
    for (std::string line; std::getline(lines, line);) {
        auto entry = Json::parse(line); const auto type = entry.at("type").get<std::string>();
        if (type.starts_with("exit_") || type.starts_with("order_") || type == "fill" || type == "sell_fill") { result.push_back(entry); }
    }
    return result;
}
}

int main() {
    try {
        eme::test::Context test; Fixture fixture; const auto input = fixture.build();
        const auto params = exit_policy();
        const auto [sold, trace] = fixture.run(input, params);
        test.expect(sold["lifecycle"]["orders"] == 3U && sold["lifecycle"]["accounting_complete"] == true &&
            sold["residual_exit"]["sold_centicontracts"] == 500U && sold["residual_exit"]["credit_micro_usd"] == 2'663'300 &&
            sold["residual_exit"]["fees_micro_usd"] == 86'700 && sold["lifecycle"]["settlement_cash_micro_usd"] == 0 &&
            sold["lifecycle"]["simulated_net_pnl_micro_usd"] == -568'100,
            "sell only the confirmed unpaired YES holding, at bid, with sale fees and no duplicate settlement");
        test.expect(sold["lifecycle"]["capital_time_micro_usd_seconds"] == 1U &&
            sold["lifecycle"]["capital_time_fractional_micro_usd_nanoseconds"] == 938'840'000U,
            "full exit releases paid capital at arrival: independent 3231400 * 600 product");
        test.expect(trace == fixture.run(input, params).second, "exit replay is byte deterministic");
        auto hold = params; hold["residual_exit"]["mode"] = "hold";
        test.expect(fixture.run(input, hold).first["lifecycle"]["simulated_net_pnl_micro_usd"] == -3'231'400,
            "same losing scenario retains the hold baseline");
        auto winning = params; winning["lifecycle"]["settlements"][1U]["yes_wins"] = true;
        auto hold_winner = winning; hold_winner["residual_exit"]["mode"] = "hold";
        test.expect(fixture.run(input, hold_winner).first["lifecycle"]["simulated_net_pnl_micro_usd"] == 1'768'600 &&
            actions(trace) == actions(fixture.run(input, winning).second),
            "exit can sacrifice winning settlement; future outcome never selects the earlier policy action");
        auto partial = winning; partial["residual_exit"]["available_liquidity_bps"] = 4000U;
        const auto part = fixture.run(input, partial).first;
        test.expect(part["residual_exit"]["sold_centicontracts"] == 200U && part["residual_exit"]["credit_micro_usd"] == 1'065'300 &&
            part["residual_exit"]["released_basis_micro_usd"] == 1'292'560 && part["lifecycle"]["settlement_cash_micro_usd"] == 3'000'000 &&
            part["lifecycle"]["simulated_net_pnl_micro_usd"] == 833'900,
            "partial exit allocates acquisition basis exactly and settles only retained contracts");
        auto rejected = params; rejected["residual_exit"]["reject"] = true;
        const auto reject = fixture.run(input, rejected).first;
        test.expect(reject["lifecycle"]["orders"] == 3U && reject["residual_exit"]["sold_centicontracts"] == 0U &&
            reject["lifecycle"]["simulated_net_pnl_micro_usd"] == -3'231'400, "rejected exit stays exposed without automatic retries");
        auto price = params; price["residual_exit"]["minimum_price_1e4"] = 5600U;
        test.expect(fixture.run(input, price).first["lifecycle"]["orders"] == 2U, "no sale below declared exit price floor");
        auto timeout = params; timeout["residual_exit"]["timeout_ns"] = 599U;
        test.expect(fixture.run(input, timeout).first["lifecycle"]["orders"] == 2U, "exit timeout includes arrival latency");
        auto unresolved = params; unresolved["residual_exit"]["response_latency_ns"] = 100'000U;
        const auto unknown = fixture.run(input, unresolved).first;
        test.expect(unknown["lifecycle"]["simulated_net_pnl_micro_usd"].is_null() && unknown["lifecycle"]["unknown_orders"] == 1U &&
            unknown["available_cash_micro_usd"] == 96'768'600 && unknown["residual_exit"]["credit_awaiting_response_micro_usd"] == 2'663'300,
            "sale cash cannot fund another trade before response; EOF keeps order uncertainty");
        auto beyond = params; beyond["residual_exit"]["arrival_latency_ns"] = 100'000U;
        const auto unseen = fixture.run(input, beyond).first;
        test.expect(unseen["residual_exit"]["sold_centicontracts"] == 0U && unseen["lifecycle"]["unknown_orders"] == 1U &&
            unseen["lifecycle"]["simulated_net_pnl_micro_usd"].is_null(), "future annotations never manufacture an unobserved sale");
        auto settled = winning; settled["lifecycle"]["settlements"][1U]["time_ns"] = 2600U;
        const auto tie = fixture.run(input, settled).first;
        test.expect(tie["residual_exit"]["sold_centicontracts"] == 0U && tie["lifecycle"]["settlement_cash_micro_usd"] == 5'000'000,
            "settlement wins an equal-time exit arrival and prevents selling already settled holdings");
        auto lost_depth = capture();
        lost_depth["records"][2U]["payload"]["msg"]["side"] = "yes";
        lost_depth["records"][2U]["payload"]["msg"]["price_dollars"] = "0.5500";
        lost_depth["records"][2U]["payload"]["msg"]["delta_fp"] = "-5.00";
        auto delayed = params; delayed["residual_exit"]["arrival_latency_ns"] = 700U;
        test.expect(fixture.run(fixture.build(lost_depth), delayed).first["residual_exit"]["sold_centicontracts"] == 0U,
            "exit rechecks depth at arrival instead of filling from the decision snapshot");
        auto quiet = capture(); quiet["records"].erase(quiet["records"].begin() + 2, quiet["records"].end());
        quiet["controls"][1U]["before_record"] = 2U;
        auto stale = params; stale["max_book_age_ns"] = 1500U; stale["residual_exit"]["arrival_latency_ns"] = 1500U;
        test.expect(fixture.run(fixture.build(quiet), stale).first["residual_exit"]["sold_centicontracts"] == 0U,
            "stale exit books cannot fill positions");
        auto sequential = params; sequential["reject_legs"] = {false, false};
        sequential["lifecycle"]["execution_policy"] = "sequential_complete";
        sequential["lifecycle"]["response_latency_ns"] = {1500U, 0U};
        for (auto& label : sequential["lifecycle"]["settlements"]) { label["yes_wins"] = true; }
        test.expect(fixture.run(input, sequential).first["lifecycle"]["simulated_net_pnl_micro_usd"] == -389'200,
            "sequential completion fallback sells NO through the ask book using complementary prices");
        sequential["lifecycle"]["completion_loss_limit_micro_usd"] = 1'000'000U;
        const auto paired = fixture.run(input, sequential).first;
        test.expect(paired["residual_exit"]["sold_centicontracts"] == 200U && paired["lifecycle"]["settlement_cash_micro_usd"] == 3'000'000 &&
            paired["lifecycle"]["simulated_net_pnl_micro_usd"] == -97'600, "exit preserves the matched three-contract portfolio");
        auto no_response = params; no_response["lifecycle"]["response_latency_ns"] = {100'000U, 500U};
        test.expect(fixture.run(input, no_response).first["lifecycle"]["orders"] == 2U, "no exit until both acquisition responses are known");
        auto absent_labels = params; absent_labels["lifecycle"]["settlements"] = Json::array();
        test.expect(fixture.run(input, absent_labels).first["lifecycle"]["accounting_complete"] == true,
            "fully exited holdings need no future settlement label");
        auto invalid = params; invalid["residual_exit"]["minimum_price_1e4"] = 10001U;
        write(fixture.root / "invalid-exit.json", invalid); std::ostringstream rejected_trace;
        test.expect(session::run_execution_study(input, fixture.root / "invalid-exit.json", rejected_trace).has_value() && rejected_trace.str().empty(),
            "invalid exit configuration fails before emitting any actions");
        // One physical ask can be consumed by buying YES or selling NO. Two
        // overlapping reviewed portfolios must not use that depth twice.
        auto shared_metadata = metadata;
        shared_metadata["markets"].push_back({{"id", 3U}, {"ticker", "C"}});
        shared_metadata["constraints"].push_back({{"id", 8U}, {"semantic_version", 1U}, {"key", "c-implies-a"},
            {"provenance", "Synthetic shared liquidity"}, {"relationship", {{"type", "implication"}, {"antecedent", 3U}, {"consequent", 1U}}}});
        auto shared_capture = quiet;
        auto c = record(3000, "C", 1U, Json::parse(R"({"type":"orderbook_snapshot","msg":{
            "yes_dollars_fp":[["0.8000","5.00"]],"no_dollars_fp":[["0.8500","5.00"]]}})"));
        c["payload"]["sid"] = 3U; shared_capture["records"].push_back(c);
        shared_capture["controls"][1U]["before_record"] = 3U;
        auto shared_policy = params; shared_policy["reject_legs"] = {false, true};
        shared_policy["lifecycle"]["response_latency_ns"] = {3000U, 3000U};
        shared_policy["fees"].push_back({{"market_id", 3U}, {"coefficient_ppm", 70000U}, {"balance_quantum_micro", 100U}});
        const auto shared = fixture.run(fixture.build(shared_capture, shared_metadata), shared_policy).first;
        test.expect(shared["attempts"] == 2U && shared["lifecycle"]["orders"] == 4U && shared["residual_exit"]["sold_centicontracts"] == 0U,
            "opposite-outcome acquisitions exhaust the same liquidity needed by later exits");
        session::detail::StudyPositions positions;
        positions.reserve(1U, 2U);
        const eme::constraint::PayoffLegTemplate yes_leg{1U, eme::constraint::ContractOutcome::yes};
        positions.acquire(0U, 0U, yes_leg, 100, 10001, 10);
        positions.acquire(0U, 0U, yes_leg, 200, 40001, 20);
        const auto released = positions.close(0U, 0U, 150, 30);
        bool oversell = false;
        try { (void)positions.close(0U, 0U, 151, 31); } catch (const session::ReplayError&) { oversell = true; }
        const auto closed = positions.settle({1U, true, 100});
        test.expect(released == 20001 && oversell && closed.quantity == 150 && closed.payout == 1'500'000 &&
            positions.unsettled_lots() == 0 && positions.capital_time().fractional_micro_usd_nanoseconds == 2'700'100U,
            "FIFO sale across unequal-cost lots preserves every cost unit and rejects overselling");
        // Independent arithmetic over small whole/fractional sizing grids.
        // No production fee, position or sizing helper participates in this oracle.
        for (std::int64_t scenario = 0; scenario < 40; ++scenario) {
            auto observed = quiet; auto parameters = params;
            const auto cap = (1 + scenario % 5) * 100;
            const std::int64_t step = scenario % 2 == 0 ? 1 : 100;
            const std::int64_t quantum = scenario % 3 == 0 ? 10000 : 100;
            const std::int64_t coefficient = scenario % 4 == 0 ? 0 : 70000;
            const auto bps = std::array<std::int64_t, 4>{0, 2500, 5000, 10000}[static_cast<std::size_t>(scenario % 4)];
            const bool wins = scenario % 2 == 0;
            const auto quantity_text = std::to_string(cap / 100) + ".00";
            observed["records"][1U]["payload"]["msg"]["no_dollars_fp"] = Json::array({Json::array({"0.6000", quantity_text})});
            observed["records"][1U]["payload"]["msg"]["yes_dollars_fp"] = Json::array({Json::array({"0.5500", quantity_text})});
            parameters["quantity_cap_centicontracts"] = cap; parameters["quantity_step_centicontracts"] = step;
            parameters["residual_exit"]["available_liquidity_bps"] = bps;
            parameters["lifecycle"]["settlements"][1U]["yes_wins"] = wins;
            for (auto& fee : parameters["fees"]) { fee["coefficient_ppm"] = coefficient; fee["balance_quantum_micro"] = quantum; }
            const auto fee = [&](const std::int64_t quantity, const std::int64_t price) {
                return (quantity * coefficient * price * (10000 - price) + 9'999'999'999LL) / 10'000'000'000LL;
            };
            const auto debit = [&](const std::int64_t quantity, const std::int64_t price) {
                return (quantity * price + fee(quantity, price) + quantum - 1) / quantum * quantum;
            };
            std::int64_t chosen = 0, margin = 0;
            for (auto q = step; q <= cap; q += step) {
                const auto candidate = q * 10000 - debit(q, 3000) - debit(q, 6000);
                if (candidate > margin) { chosen = q; margin = candidate; }
            }
            const auto sold_quantity = std::min(chosen, cap * bps / 10000) / step * step;
            const auto credit = (sold_quantity * 5500 - fee(sold_quantity, 5500)) / quantum * quantum;
            const auto spent = debit(chosen, 6000);
            const auto basis = chosen == 0 ? 0 : spent * sold_quantity / chosen;
            const auto payout = wins ? (chosen - sold_quantity) * 10000 : 0;
            const auto capital_time = basis * 600 + (spent - basis) * 998000;
            const auto result = fixture.run(fixture.build(observed), parameters).first;
            test.expect(result["spent_micro_usd"] == spent && result["residual_exit"]["sold_centicontracts"] == sold_quantity &&
                result["residual_exit"]["credit_micro_usd"] == credit && result["lifecycle"]["settlement_cash_micro_usd"] == payout &&
                result["lifecycle"]["simulated_net_pnl_micro_usd"] == credit + payout - spent &&
                result["lifecycle"]["capital_time_micro_usd_seconds"] == capital_time / 1'000'000'000LL &&
                result["lifecycle"]["capital_time_fractional_micro_usd_nanoseconds"] == capital_time % 1'000'000'000LL,
                "independent residual ledger " + std::to_string(scenario));
        }
        return test.result();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
