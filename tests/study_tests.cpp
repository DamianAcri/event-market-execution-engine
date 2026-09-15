#include "eme/session/execution_study.hpp"
#include "eme/session/async_capture.hpp"
#include "session/study_json.hpp"
#include "test_support.hpp"

#include <chrono>
#include <fstream>
#include <sstream>

namespace {
using Json = nlohmann::json;
namespace session = eme::session;

const Json metadata = Json::parse(R"({"schema_version":1,"metadata_version":11,"venue":"kalshi",
  "markets":[{"id":1,"ticker":"A"},{"id":2,"ticker":"B"}],"constraints":[
  {"id":7,"semantic_version":1,"key":"a-implies-b","provenance":"Synthetic arithmetic fixture only",
   "relationship":{"type":"implication","antecedent":1,"consequent":2}}]})");

Json policy() {
    return Json::parse(R"({"schema_version":1,"strategy":"one_attempt_per_constraint_v1",
      "fee_provenance":"Synthetic scenario: general 0.07 coefficient, direct-account precision; not venue fee discovery",
      "capital_micro_usd":100000000,"operating_cost_micro_usd":0,
      "quantity_cap_centicontracts":500,"quantity_step_centicontracts":100,"min_margin_micro_usd":0,
      "max_book_age_ns":100000,"leg_latency_ns":[0,0],"reject_legs":[false,false],
      "available_liquidity_bps":10000,"fees":[
      {"market_id":1,"coefficient_ppm":70000,"balance_quantum_micro":100},
      {"market_id":2,"coefficient_ppm":70000,"balance_quantum_micro":100}]})");
}
Json record(const std::int64_t time, const std::string& ticker, const std::uint64_t sequence, Json payload) {
    payload["msg"]["market_ticker"] = ticker;
    payload["sid"] = ticker == "A" ? 1U : 2U;
    payload["seq"] = sequence;
    return {{"generation", 1U}, {"time_ns", time}, {"observed_at_ns", 1'800'000'000'000'000'000LL + time},
            {"sequence", sequence}, {"payload", payload}};
}
Json capture() {
    const auto a = record(1000, "A", 1U, Json::parse(R"({"type":"orderbook_snapshot","msg":{
        "yes_dollars_fp":[["0.7000","5.00"]],"no_dollars_fp":[["0.7500","5.00"]]}})"));
    const auto b = record(2000, "B", 1U, Json::parse(R"({"type":"orderbook_snapshot","msg":{
        "yes_dollars_fp":[["0.5500","5.00"]],"no_dollars_fp":[["0.6000","2.00"],["0.6500","3.00"]]}})"));
    const auto remove = record(3000, "B", 2U, Json::parse(R"({"type":"orderbook_delta","msg":{
        "price_dollars":"0.6000","delta_fp":"-2.00","side":"no"}})"));
    const auto remove2 = record(4000, "B", 3U, Json::parse(R"({"type":"orderbook_delta","msg":{
        "price_dollars":"0.6500","delta_fp":"-3.00","side":"no"}})"));
    return {{"schema_version", 1U}, {"source_kind", "synthetic"}, {"provenance", "Synthetic depth disappears after the signal; no observed trading evidence"},
        {"use_yes_price", true}, {"controls", Json::array({{{"before_record", 0U}, {"time_ns", 0U}, {"action", "open"}, {"target", 1U}},
            {{"before_record", 4U}, {"time_ns", 10'000U}, {"action", "close"}, {"target", 1U}}})},
        {"records", Json::array({a, b, remove, remove2})}};
}
void write(const std::filesystem::path& path, const Json& json) {
    std::ofstream file{path}; file << json.dump(2) << '\n';
    if (!file) { throw std::runtime_error{"write fixture"}; }
}
class Fixture final {
public:
    explicit Fixture(std::filesystem::path path = {}) : keep_{!path.empty()} {
        root = path.empty() ? std::filesystem::temp_directory_path() / ("eme-study-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) : std::move(path);
        if (!std::filesystem::create_directory(root)) { throw std::runtime_error{"fixture directory exists"}; }
    }
    ~Fixture() { if (!keep_) { std::error_code ignored; std::filesystem::remove_all(root, ignored); } }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    session::ReplayInput build(const Json& data = capture(), const Json& meta = metadata) {
        write(root / "metadata.json", meta);
        write(root / "capture.json", data);
        const auto directory = root / ("session-" + std::to_string(index_++));
        if (const auto failure = session::import_capture(root / "metadata.json", root / "capture.json", directory)) {
            throw std::runtime_error{failure->reason};
        }
        auto loaded = session::load_replay(directory, directory / "replay.json");
        if (const auto* failure = std::get_if<session::ReplayError>(&loaded)) { throw std::runtime_error{failure->reason}; }
        return std::get<session::ReplayInput>(std::move(loaded));
    }
    std::pair<Json, std::string> run(const session::ReplayInput& input, const Json& parameters = policy()) {
        write(root / "policy.json", parameters);
        std::ostringstream out;
        if (const auto failure = session::run_execution_study(input, root / "policy.json", out)) { throw std::runtime_error{failure->reason}; }
        std::istringstream lines{out.str()};
        Json last;
        for (std::string line; std::getline(lines, line);) { last = Json::parse(line); }
        if (last["type"] != "study_complete") { throw std::runtime_error{"no complete study"}; }
        return {last, out.str()};
    }
    std::filesystem::path root;
private:
    bool keep_{};
    std::size_t index_{};
};
}  // namespace

int main(const int argc, const char* const argv[]) {
    try {
        if (argc == 3 && std::string_view{argv[1]} == "--fixture") {
            Fixture fixture{argv[2]};
            const auto input = fixture.build();
            const auto result = fixture.run(input);
            std::ofstream out{fixture.root / "immediate.jsonl"}; out << result.second;
            std::cout << input.directory << '\n';
            return 0;
        }
        eme::test::Context test;
        Fixture fixture;
        const auto input = fixture.build();
        const auto [immediate, transcript] = fixture.run(input);
        test.expect(immediate["completed_pairs"] == 1U && immediate["spent_micro_usd"] == 4'804'900 &&
            immediate["fees_micro_usd"] == 154'900 && immediate["net_settlement_bound_micro_usd"] == 195'100,
            "five-contract depth sweep matches independently calculated costs and settlement floor");
        test.expect(transcript == fixture.run(input).second, "byte-identical repeated full study");
        const auto repacked_path = fixture.root / "background-copy";
        auto recorder = std::move(std::get<std::unique_ptr<session::AsyncCaptureWriter>>(
            session::create_async_capture(repacked_path, input.session.metadata)));
        auto reader = std::move(std::get<std::unique_ptr<eme::journal::RawJournalReader>>(
            eme::journal::open_raw_journal_reader(input.directory / session::journal_filename)));
        for (;;) {
            auto next = reader->read_next();
            if (std::holds_alternative<eme::journal::EndOfJournal>(next)) { break; }
            test.expect(recorder->try_append(std::move(std::get<eme::journal::RawMarketRecord>(next))) ==
                session::CaptureAppendResult::queued, "replay fixture recorded through background writer");
        }
        test.expect(std::get<session::SessionManifest>(recorder->finish()) == input.session.manifest,
            "background copy preserves manifest binding and raw data");
        const auto repacked = std::get<session::ReplayInput>(session::load_replay(repacked_path, input.directory / "replay.json"));
        test.expect(fixture.run(repacked).second == transcript, "background capture reproduces identical causal decisions and simulated fills");
        test.expect(immediate["realized_pnl_micro_usd"].is_null() && immediate["evidence_status"] == "synthetic_validation_only",
            "synthetic profit is never labeled realized profitability");
        auto delayed = policy(); delayed["leg_latency_ns"] = {2000, 2000};
        const auto loss = fixture.run(input, delayed).first;
        test.expect(loss["unbalanced_pairs"] == 1U && loss["net_settlement_bound_micro_usd"] == -1'573'500,
            "equal-time quote removals precede arrival; one completed leg can lose its full cost");
        auto rejection = policy(); rejection["reject_legs"] = {false, true};
        test.expect(fixture.run(input, rejection).first["net_settlement_bound_micro_usd"] == -1'573'500,
            "independent leg rejection stress preserves exposure");
        auto partial = policy(); partial["available_liquidity_bps"] = 5000U;
        const auto partial_result = fixture.run(input, partial).first;
        test.expect(partial_result["spent_micro_usd"] == 1'912'200 && partial_result["settlement_floor_micro_usd"] == 2'000'000,
            "partial depth, quantity grid, and fees account for actual fills only");
        auto unfunded = policy(); unfunded["capital_micro_usd"] = 1U;
        test.expect(fixture.run(input, unfunded).first["attempts"] == 0U, "no unfunded order submission");
        auto limited = policy(); limited["capital_micro_usd"] = 2'000'000U;
        const auto funded = fixture.run(input, limited).first;
        test.expect(funded["available_cash_micro_usd"].get<std::int64_t>() >= 0 && funded["settlement_floor_micro_usd"] == 2'000'000,
            "funding search reduces quantity, reserves before submission and releases unused reserve");
        auto stale = policy(); stale["max_book_age_ns"] = 0U;
        test.expect(fixture.run(input, stale).first["attempts"] == 0U, "asynchronous stale books cannot enter");
        auto expensive = policy(); expensive["fees"][0U]["coefficient_ppm"] = 1'000'000U;
        test.expect(fixture.run(input, expensive).first["attempts"] == 0U, "fees can eliminate a gross opportunity");
        auto overhead = policy(); overhead["operating_cost_micro_usd"] = 1'000'000U;
        test.expect(fixture.run(input, overhead).first["net_settlement_bound_micro_usd"] == -804'900,
            "operating overhead deducted once without inventing a trading charge");
        auto eof = capture(); eof["records"].erase(eof["records"].begin() + 2, eof["records"].end()); eof["controls"].erase(1U);
        const auto unobserved = fixture.run(fixture.build(eof), delayed).first;
        test.expect(unobserved["unobserved_orders"] == 2U && unobserved["spent_micro_usd"] == 0U &&
            unobserved["available_cash_micro_usd"] == 100'000'000U, "orders beyond recorded horizon are not filled from stale final book");
        auto duplicate = metadata;
        auto second = duplicate["constraints"][0U]; second["id"] = 8U; second["key"] = "same-liquidity";
        duplicate["constraints"].push_back(second);
        const auto shared = fixture.run(fixture.build(capture(), duplicate)).first;
        test.expect(shared["attempts"] == 2U && shared["completed_pairs"] == 1U && shared["spent_micro_usd"] == 4'804'900,
            "two simultaneous strategies cannot both consume the same depth");
        auto gap = capture();
        gap["records"][2U]["sequence"] = 4U; gap["records"][2U]["payload"]["seq"] = 4U;
        gap["records"][3U] = gap["records"][1U]; gap["records"][3U]["time_ns"] = 4000U;
        gap["records"][3U]["sequence"] = 10U; gap["records"][3U]["payload"]["seq"] = 10U;
        auto gap_input = fixture.build(gap);
        session::ReplayObserver observer;
        const auto rejected = session::replay(gap_input, observer);
        test.expect(std::get<session::ReplaySummary>(rejected).rejected_updates == 2U, "snapshot does not silently recover a stale book");
        gap["controls"].insert(gap["controls"].begin() + 1, Json{{"before_record", 3U}, {"time_ns", 3500U}, {"action", "recover"}, {"target", 2U}});
        const auto recovered = session::replay(fixture.build(gap), observer);
        test.expect(std::get<session::ReplaySummary>(recovered).rejected_updates == 1U, "explicit persisted recovery restores the book");
        auto bad_plan = Json::parse(session::detail::read_text(input.directory / "replay.json"));
        bad_plan["manifest_sha256"] = std::string(64U, '0'); write(fixture.root / "bad-plan.json", bad_plan);
        test.expect(std::holds_alternative<session::ReplayError>(session::load_replay(input.directory, fixture.root / "bad-plan.json")),
            "plan cannot be attached to another session");
        bad_plan = Json::parse(session::detail::read_text(input.directory / "replay.json"));
        bad_plan["use_yes_price"] = false; write(fixture.root / "bad-plan.json", bad_plan);
        test.expect(std::holds_alternative<session::ReplayError>(session::load_replay(input.directory, fixture.root / "bad-plan.json")), "price convention is mandatory");
        auto bad_clock = input; bad_clock.plan.controls.back().time_ns = 1;
        test.expect(std::holds_alternative<session::ReplayError>(session::replay(bad_clock, observer)), "control clock cannot regress");
        auto bad_policy = policy(); bad_policy["fees"].erase(0U); write(fixture.root / "bad-policy.json", bad_policy);
        std::ostringstream rejected_output;
        test.expect(session::run_execution_study(input, fixture.root / "bad-policy.json", rejected_output).has_value() &&
            rejected_output.str().empty(), "incomplete fee policy fails before output");
        auto sizing_policy = policy();
        sizing_policy["schema_version"] = 2U;
        sizing_policy["strategy"] = "one_attempt_net_profit_v2";
        sizing_policy["max_sizing_evaluations"] = 100'000U;
        auto sizing_capture = capture();
        sizing_capture["records"].erase(sizing_capture["records"].begin() + 2, sizing_capture["records"].end());
        sizing_capture["controls"][1U]["before_record"] = 2U;
        sizing_capture["records"][1U]["payload"]["msg"]["no_dollars_fp"][1U][0U] = "0.8000";
        const auto sizing_input = fixture.build(sizing_capture);
        test.expect(fixture.run(sizing_input).first["attempts"] == 0U, "legacy policy preserves its documented maximum-size behavior");
        const auto [sized, sized_transcript] = fixture.run(sizing_input, sizing_policy);
        test.expect(sized["completed_pairs"] == 1U && sized["settlement_floor_micro_usd"] == 2'000'000 &&
            sized["spent_micro_usd"] == 1'863'000 && sized["net_settlement_bound_micro_usd"] == 137'000 &&
            sized["strategy"] == "one_attempt_net_profit_v2", "new policy recovers smaller profitable size through actual replay fills");
        test.expect(sized_transcript == fixture.run(sizing_input, sizing_policy).second, "optimized policy replay is byte deterministic");
        sizing_policy["reject_legs"] = {false, true};
        const auto sized_rejected = fixture.run(sizing_input, sizing_policy).first;
        test.expect(sized_rejected["unbalanced_pairs"] == 1U && sized_rejected["net_settlement_bound_micro_usd"] == -629'400,
            "optimal sizing does not bypass independent leg rejection and exposure");
        sizing_policy["max_sizing_evaluations"] = 1U;
        const auto exhausted = fixture.run(sizing_input, sizing_policy).first;
        test.expect(exhausted["attempts"] == 0U && exhausted["declined"]["sizing_search_budget_exceeded"] == 1U,
            "incomplete economic search remains distinguishable from no opportunity");
        sizing_policy.erase("max_sizing_evaluations");
        write(fixture.root / "bad-policy.json", sizing_policy);
        std::ostringstream missing_budget;
        test.expect(session::run_execution_study(sizing_input, fixture.root / "bad-policy.json", missing_budget).has_value() &&
            missing_budget.str().empty(), "version two requires an explicit bounded search budget");
        bad_plan = Json::parse(session::detail::read_text(input.directory / "replay.json"));
        std::ofstream dup{fixture.root / "duplicate.json"};
        auto text = bad_plan.dump(); text.insert(1U, "\"schema_version\":1,"); dup << text; dup.close();
        test.expect(std::holds_alternative<session::ReplayError>(session::load_replay(input.directory, fixture.root / "duplicate.json")), "duplicate keys rejected");
        auto life = policy();
        life["schema_version"] = 3U; life["strategy"] = "execution_lifecycle_v3";
        life["max_sizing_evaluations"] = 100'000U;
        life["lifecycle"] = {{"execution_policy", "parallel_hold"}, {"first_leg", 0U}, {"response_latency_ns", {500U, 500U}},
            {"completion_timeout_ns", 5000U}, {"completion_loss_limit_micro_usd", 0U}, {"maximum_completion_orders", 2U},
            {"settlements", Json::array({{{"market_id", 1U}, {"yes_wins", true}, {"time_ns", 1'000'000U}, {"provenance", "Synthetic settlement scenario"}},
                {{"market_id", 2U}, {"yes_wins", true}, {"time_ns", 1'000'000U}, {"provenance", "Synthetic settlement scenario"}}})}};
        const auto [life_result, life_trace] = fixture.run(input, life);
        const auto& ledger = life_result["lifecycle"];
        test.expect(ledger["accounting_complete"] == true && ledger["cash_micro_usd"] == 100'195'100 &&
            ledger["settlement_cash_micro_usd"] == 5'000'000 && ledger["simulated_net_pnl_micro_usd"] == 195'100,
            "full acquisition, delayed responses and settlement conserve cash through positive synthetic result");
        test.expect(ledger["capital_time_micro_usd_seconds"] == 4795U &&
            ledger["capital_time_fractional_micro_usd_nanoseconds"] == 290'200'000U,
            "capital-time matches independent exact product 4804900 * 998000 nanoseconds");
        test.expect(life_trace == fixture.run(input, life).second, "complete lifecycle replay is deterministic");
        auto sequential = life; sequential["lifecycle"]["execution_policy"] = "sequential_complete";
        const auto sequence_result = fixture.run(input, sequential).first;
        test.expect(sequence_result["lifecycle"]["orders"] == 2U &&
            sequence_result["lifecycle"]["simulated_net_pnl_micro_usd"] == 195'100 &&
            sequence_result["lifecycle"]["capital_time_micro_usd_seconds"] == 4793U &&
            sequence_result["lifecycle"]["capital_time_fractional_micro_usd_nanoseconds"] == 674'500'000U,
            "sequential completion waits for response; same quiet-book profit, shorter paid capital holding");
        auto reverse = sequential; reverse["lifecycle"]["first_leg"] = 1U;
        const auto reversed_result = fixture.run(input, reverse);
        const auto first_fill = reversed_result.second.find("\"type\":\"fill\"");
        const auto preceding = reversed_result.second.rfind('\n', first_fill);
        const auto fill_line = Json::parse(reversed_result.second.substr(preceding + 1U, reversed_result.second.find('\n', first_fill) - preceding - 1U));
        test.expect(reversed_result.first["lifecycle"]["simulated_net_pnl_micro_usd"] == 195'100 && fill_line["market_id"] == 2U,
            "policy explicitly selects the first leg without changing payoff semantics");
        auto first_reject = life; first_reject["reject_legs"] = {true, false};
        for (auto& value : first_reject["lifecycle"]["settlements"]) { value["yes_wins"] = false; }
        const auto parallel_reject = fixture.run(input, first_reject).first;
        first_reject["lifecycle"]["execution_policy"] = "sequential_complete";
        const auto sequential_reject = fixture.run(input, first_reject).first;
        test.expect(parallel_reject["lifecycle"]["simulated_net_pnl_micro_usd"] == -3'231'400 &&
            sequential_reject["lifecycle"]["simulated_net_pnl_micro_usd"] == 0 &&
            sequential_reject["lifecycle"]["orders"] == 1U,
            "waiting for rejected first leg avoids second-leg loss in the declared losing world");
        auto slow_response = sequential; slow_response["lifecycle"]["response_latency_ns"] = {1500U, 0U};
        const auto slow = fixture.run(input, slow_response).first;
        test.expect(slow["lifecycle"]["orders"] == 1U && slow["lifecycle"]["simulated_net_pnl_micro_usd"] == -1'573'500,
            "waiting can lose the opportunity; zero-loss completion limit does not invent an exit");
        slow_response["lifecycle"]["completion_loss_limit_micro_usd"] = 1'000'000U;
        const auto partial_completion = fixture.run(input, slow_response).first;
        test.expect(partial_completion["lifecycle"]["orders"] == 2U &&
            partial_completion["lifecycle"]["simulated_net_pnl_micro_usd"] == -571'300,
            "bounded partial completion reduces residual loss with actual remaining depth and per-order fees");
        auto no_response = life; no_response["lifecycle"]["response_latency_ns"] = {100'000U, 100'000U};
        const auto unknown_response = fixture.run(input, no_response).first;
        test.expect(unknown_response["lifecycle"]["unknown_orders"] == 2U &&
            unknown_response["lifecycle"]["reserved_micro_usd"].get<std::int64_t>() > 0 &&
            unknown_response["lifecycle"]["simulated_net_pnl_micro_usd"].is_null(),
            "EOF before response preserves reservations and suppresses a completed PnL claim");
        auto unknown_arrival = life; unknown_arrival["leg_latency_ns"] = {100'000U, 100'000U};
        const auto unknown_fill = fixture.run(input, unknown_arrival).first;
        test.expect(unknown_fill["spent_micro_usd"] == 0 && unknown_fill["lifecycle"]["unknown_orders"] == 2U &&
            unknown_fill["lifecycle"]["reserved_micro_usd"].get<std::int64_t>() > 0,
            "future settlement annotations cannot fabricate fills beyond EOF");
        auto unresolved = life; unresolved["lifecycle"]["settlements"].erase(0U);
        test.expect(fixture.run(input, unresolved).first["lifecycle"]["simulated_net_pnl_micro_usd"].is_null(),
            "unresolved positions prevent final simulated PnL");
        auto label_change = life; label_change["lifecycle"]["settlements"][0U]["yes_wins"] = false;
        const auto decisions = [](const std::string& lines) {
            Json result = Json::array(); std::istringstream stream{lines};
            for (std::string line; std::getline(stream, line);) {
                const auto entry = Json::parse(line); const auto type = entry.at("type").get<std::string>();
                if (type == "decision" || type == "order_intent" || type == "order_arrival" || type == "order_response" || type == "fill") { result.push_back(entry); }
            }
            return result;
        };
        test.expect(decisions(life_trace) == decisions(fixture.run(input, label_change).second),
            "future settlement outcomes cannot change preceding orders/fills/responses");
        auto impossible_world = life; impossible_world["lifecycle"]["settlements"][1U]["yes_wins"] = false;
        write(fixture.root / "bad-policy.json", impossible_world); std::ostringstream invalid_world;
        test.expect(session::run_execution_study(input, fixture.root / "bad-policy.json", invalid_world).has_value() && invalid_world.str().empty(),
            "settlement scenario cannot contradict reviewed implication");
        auto early_resolution = life;
        for (auto& item : early_resolution["lifecycle"]["settlements"]) { item["time_ns"] = 2000U; }
        test.expect(fixture.run(input, early_resolution).first["attempts"] == 0U,
            "settlement at signal time is terminal before making a decision");
        auto timeout_completion = sequential; timeout_completion["lifecycle"]["completion_timeout_ns"] = 10U;
        test.expect(fixture.run(input, timeout_completion).first["lifecycle"]["orders"] == 1U,
            "completion timeout stops new exposure without claiming cancellation of a filled position");
        auto rejected_completion = sequential; rejected_completion["reject_legs"] = {false, true};
        rejected_completion["lifecycle"]["response_latency_ns"] = {0U, 0U};
        const auto repeated = fixture.run(input, rejected_completion).first;
        test.expect(repeated["lifecycle"]["orders"] == 3U && repeated["lifecycle"]["reserved_micro_usd"] == 0,
            "completion rejection retries are bounded and reservations reconcile");
        auto charged_life = life; charged_life["operating_cost_micro_usd"] = 200'000U;
        test.expect(fixture.run(input, charged_life).first["lifecycle"]["simulated_net_pnl_micro_usd"] == -4900,
            "economic result deducts operating cost once after settlement");
        auto low_fund_life = sequential; low_fund_life["capital_micro_usd"] = 2'000'000U;
        const auto low_funded = fixture.run(input, low_fund_life).first;
        test.expect(low_funded["lifecycle"]["accounting_complete"] == true && low_funded["lifecycle"]["simulated_net_pnl_micro_usd"] == 137'000,
            "small funded size completes and settles within conservative reserves");
        // Independent single-level ledger: integer ceil formula, no production
        // fee or execution helpers. Vary prices, grids, quantum, fills and worlds.
        for (std::int64_t scenario = 0; scenario < 90; ++scenario) {
            const auto q = 1 + scenario % 5;
            const std::int64_t step = scenario % 2 == 0 ? 1 : 100;
            const auto p0 = 2000 + (scenario % 7) * 101;
            const auto p1 = 5000 + (scenario % 11) * 73;
            const auto bps = std::array<std::int64_t, 4>{0, 2500, 5000, 10000}[static_cast<std::size_t>(scenario % 4)];
            const auto coefficient = scenario % 2 == 0 ? 70000LL : 0LL;
            const auto quantum = scenario % 3 == 0 ? 10000LL : 100LL;
            const bool reject0 = scenario % 7 == 0, reject1 = scenario % 11 == 0;
            const bool yes0 = scenario % 3 == 0, yes1 = scenario % 3 != 1;
            const auto decimal = [](const std::int64_t price) { return "0." + std::to_string(10000 + price).substr(1U); };
            auto quiet = capture(); quiet["records"].erase(quiet["records"].begin() + 2, quiet["records"].end());
            quiet["controls"][1U]["before_record"] = 2U;
            quiet["records"][0U]["payload"]["msg"]["yes_dollars_fp"] = Json::array({Json::array({decimal(10000 - p0), std::to_string(q) + ".00"})});
            quiet["records"][0U]["payload"]["msg"]["no_dollars_fp"] = Json::array({Json::array({"0.9500", "5.00"})});
            quiet["records"][1U]["payload"]["msg"]["yes_dollars_fp"] = Json::array({Json::array({"0.1000", "5.00"})});
            quiet["records"][1U]["payload"]["msg"]["no_dollars_fp"] = Json::array({Json::array({decimal(p1), std::to_string(q) + ".00"})});
            const auto observed = fixture.build(quiet);
            auto parameters = life; parameters["quantity_cap_centicontracts"] = q * 100;
            parameters["quantity_step_centicontracts"] = step;
            parameters["available_liquidity_bps"] = bps; parameters["reject_legs"] = {reject0, reject1};
            for (auto& fee : parameters["fees"]) { fee["coefficient_ppm"] = coefficient; fee["balance_quantum_micro"] = quantum; }
            parameters["lifecycle"]["settlements"][0U]["yes_wins"] = yes0;
            parameters["lifecycle"]["settlements"][1U]["yes_wins"] = yes1;
            const auto charge = [&](const std::int64_t quantity, const std::int64_t price) {
                const auto product = quantity * coefficient * price * (10000 - price);
                const auto fee = (product + 9'999'999'999LL) / 10'000'000'000LL;
                return ((quantity * price + fee + quantum - 1) / quantum) * quantum;
            };
            std::int64_t chosen = 0, best_margin = 0;
            for (auto candidate = step; candidate <= q * 100; candidate += step) {
                const auto margin = candidate * 10000 - charge(candidate, p0) - charge(candidate, p1);
                if (margin > best_margin) { chosen = candidate; best_margin = margin; }
            }
            const auto possible_fill = std::min(chosen, (q * 100 * bps / 10000) / step * step);
            for (const bool sequential_mode : {false, true}) {
                parameters["lifecycle"]["execution_policy"] = sequential_mode ? "sequential_complete" : "parallel_hold";
                const auto f0 = reject0 ? 0 : possible_fill;
                const auto f1 = reject1 || (sequential_mode && f0 == 0) ? 0 : possible_fill;
                const auto d0 = charge(f0, p0), d1 = charge(f1, p1);
                const auto payout = ((!yes0 ? f0 : 0) + (yes1 ? f1 : 0)) * 10000;
                const auto capital_time = d0 * 998000 + d1 * (sequential_mode ? 997500 : 998000);
                const auto result = fixture.run(observed, parameters).first;
                const auto& account = result["lifecycle"];
                test.expect(result["spent_micro_usd"] == d0 + d1 && account["settlement_cash_micro_usd"] == payout &&
                    account["simulated_net_pnl_micro_usd"] == payout - d0 - d1 && account["accounting_complete"] == true &&
                    account["capital_time_micro_usd_seconds"] == capital_time / 1'000'000'000LL &&
                    account["capital_time_fractional_micro_usd_nanoseconds"] == capital_time % 1'000'000'000LL,
                    "independent generated lifecycle ledger " + std::to_string(scenario) + (sequential_mode ? " sequential" : " parallel"));
            }
        }
        return test.result();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
