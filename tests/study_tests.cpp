#include "eme/session/execution_study.hpp"
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
        return test.result();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
